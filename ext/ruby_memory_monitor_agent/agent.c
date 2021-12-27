#include <ruby.h>
#include <ruby/debug.h>
#include <string.h>
#include <time.h>
#include "compat.h"

static VALUE mRubyMemoryMonitor;
static VALUE cAgent;

#define AGENT_EVENT_TYPE_NEWOBJ 1
#define AGENT_EVENT_TYPE_FREEOBJ 2

struct agent_event_newobj {
    uint64_t object_id;
    char *backtrace;
    size_t backtrace_length;
};

struct agent_event_freeobj {
    uint64_t object_id;
};

struct agent_event {
    int event_type;
    struct timespec event_time;
    union {
        struct agent_event_newobj newobj;
        struct agent_event_freeobj freeobj;
    };
};

struct agent {
    int running;
    double allocation_sample_rate;
    VALUE newobj_trace;
    VALUE freeobj_trace;
    uint64_t object_id_counter;

    size_t ev_buffer_capacity;
    size_t ev_buffer_current_count;
    struct agent_event *ev_buffer;
    st_table *live_objects;
    struct agent_event *ev_buffer_sending;
    size_t ev_buffer_sending_count;

    VALUE flush_thread;
    double flush_interval;
    VALUE logger;
};

static void agent_gc_mark(void *ptr) {
    struct agent *a = (struct agent *)ptr;
    rb_gc_mark_movable(a->newobj_trace);
    rb_gc_mark_movable(a->freeobj_trace);
    rb_gc_mark_movable(a->flush_thread);
    rb_gc_mark_movable(a->logger);
}

static void agent_gc_free(void *ptr) {
    struct agent *a = (struct agent *)ptr;
    if (a->live_objects) {
        rb_st_free_table(a->live_objects);
    }
    if (a->ev_buffer) {
        ruby_xfree(a->ev_buffer);
    }
    ruby_xfree(ptr);
}

static size_t agent_gc_memsize(const void *ptr) {
    struct agent *a = (struct agent *)ptr;
    size_t sz = sizeof(*a);
    if (a->ev_buffer) {
        sz += a->ev_buffer_capacity;
    }
    if (a->live_objects) {
        sz += rb_st_memsize(a->live_objects);
    }
    return sz;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
// Support VALUES we're tracking being moved away in Ruby 2.7+ with GC.compact
static void agent_gc_compact(void *ptr) {
    struct agent *a = (struct agent *)ptr;
    a->newobj_trace = rb_gc_location(a->newobj_trace);
    a->freeobj_trace = rb_gc_location(a->freeobj_trace);
    a->flush_thread = rb_gc_location(a->flush_thread);
    a->logger = rb_gc_location(a->logger);
}
#endif

static const rb_data_type_t agent_type = {
    "agent",
    {
        agent_gc_mark, agent_gc_free, agent_gc_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
        agent_gc_compact,
#endif
        /* reserved */
    },
    /* parent, data, [ flags ] */
};

static struct agent *agent_get(VALUE self) {
    struct agent *a;
    TypedData_Get_Struct(self, struct agent, &agent_type, a);
    return a;
}

static VALUE agent_alloc(VALUE klass) {
    struct agent *a;
    VALUE v = TypedData_Make_Struct(klass, struct agent, &agent_type, a);
    a->running = 0;
    a->allocation_sample_rate = 0;
    a->flush_interval = 0;
    a->newobj_trace = Qnil;
    a->freeobj_trace = Qnil;
    a->live_objects = NULL;
    a->ev_buffer = NULL;
    a->ev_buffer_capacity = 0;
    a->ev_buffer_current_count = 0;
    a->object_id_counter = 0;
    a->flush_thread = Qnil;
    a->logger = Qnil;
    return v;
}

static int agent_is_buffer_full(struct agent *a) {
    return a->ev_buffer_current_count >= a->ev_buffer_capacity;
}

static void agent_backtrace_as_cstr(char **bt_ret_buf, size_t *bt_cstr_len) {
    VALUE bt_arr = rb_make_backtrace();
    if (!RTEST(bt_arr)) {
        return;
    }
    VALUE bt_str = rb_ary_join(bt_arr, rb_str_new_lit("\n"));
    char *bt_cstr = rb_string_value_cstr(&bt_str);
    *bt_cstr_len = RSTRING_LEN(bt_str) + 1;
    *bt_ret_buf = ruby_xmalloc(*bt_cstr_len);
    strncpy(*bt_ret_buf, bt_cstr, *bt_cstr_len);

    RB_GC_GUARD(bt_arr);
    RB_GC_GUARD(bt_str);
}

static VALUE agent_do_flush(VALUE agent_value) {
    struct agent *a = agent_get(agent_value);

    if (RTEST(a->logger)) {
        rb_funcall(
            a->logger, rb_intern("info"), 1, rb_sprintf("printing %lu events", a->ev_buffer_sending_count)
        );
    }

    // TODO: actually send this somewhere.
    for (size_t i = 0; i < a->ev_buffer_sending_count; i++) {
        struct agent_event *ev = &a->ev_buffer_sending[i];
        VALUE ev_line;
        switch (ev->event_type) {
        case AGENT_EVENT_TYPE_NEWOBJ:
            ev_line = rb_sprintf("Event: new allocation %llu; backtrace as follows\n%s", ev->newobj.object_id, ev->newobj.backtrace);
            break;
        case AGENT_EVENT_TYPE_FREEOBJ:
            ev_line = rb_sprintf("Event: freed allocation %llu", ev->freeobj.object_id);
            break;
        default:
            ev_line = rb_sprintf("unknown event");
            break;
        }

//        if (RTEST(a->logger)) {
//            rb_funcall(
//                a->logger, rb_intern("info"), 1, ev_line
//            );
//        }
    }
    return Qnil;
}

static VALUE agent_do_swap_and_flush(VALUE agent_value) {
    struct agent *a = agent_get(agent_value);

    // This is safe because it's under the GVL and so the tracepoint can't be getting called
    // unless we allocate ruby objects here.
    a->ev_buffer_sending = a->ev_buffer;
    a->ev_buffer_sending_count = a->ev_buffer_current_count;
    a->ev_buffer = ruby_xmalloc2(a->ev_buffer_capacity, sizeof(struct agent_event));
    memset(a->ev_buffer, 0, a->ev_buffer_capacity * sizeof(struct agent_event));
    a->ev_buffer_current_count = 0;

    // Now the only record of the original buffer is on the stack - we MUST rb_protect that
    // so that an exception flushing does not leak memory.
    int jump_tag;
    rb_protect(agent_do_flush, agent_value, &jump_tag);
    for (size_t i = 0; i < a->ev_buffer_sending_count; i++) {
        struct agent_event *ev = &a->ev_buffer_sending[i];
        if (ev->event_type == AGENT_EVENT_TYPE_NEWOBJ && ev->newobj.backtrace) {
            ruby_xfree(ev->newobj.backtrace);
        }
    }
    ruby_xfree(a->ev_buffer_sending);
    a->ev_buffer_sending = NULL;
    a->ev_buffer_sending_count = 0;

    if (jump_tag) {
        rb_jump_tag(jump_tag);
    }
    return Qnil;
}

static VALUE agent_do_swap_and_flush_rescue(VALUE agent_value) {
    struct agent *a = agent_get(agent_value);
    // TODO: some error handling
    VALUE ex = rb_errinfo();
    if (RTEST(a->logger)) {
        rb_funcall(
            a->logger, rb_intern("error"), 1,
            rb_sprintf("Error flushing memory profiling info: %"PRIsVALUE, ex)
        );
    }
    return Qnil;
}

_Noreturn static VALUE agent_run_flush_thread(VALUE agent_value) {
    struct agent *a = agent_get(agent_value);
    VALUE clock_monotonic = rb_const_get_at(rb_mProcess, rb_intern("CLOCK_MONOTONIC"));

    double sleep_for = a->flush_interval;
    while (1) {
        rb_thread_wait_for(rb_time_timeval(rb_float_new(sleep_for)));

        double t1 = rb_num2dbl(rb_funcall(rb_mProcess, rb_intern("clock_gettime"), 1, clock_monotonic));
        rb_rescue2(agent_do_swap_and_flush, agent_value, (VALUE (*)(VALUE, VALUE)) agent_do_swap_and_flush_rescue, agent_value, rb_eStandardError, 0);
        double t2 = rb_num2dbl(rb_funcall(rb_mProcess, rb_intern("clock_gettime"), 1, clock_monotonic));

        sleep_for = a->flush_interval - (t2 - t1);
        if (sleep_for < 0) {
            sleep_for = 0;
        }
    }
}

static void agent_ensure_flush_thread(VALUE agent_value) {
    struct agent *a = agent_get(agent_value);
    if (RTEST(a->flush_thread)) {
        VALUE is_alive = rb_funcall(a->flush_thread, rb_intern("alive?"), 0);
        if (RTEST(is_alive)) {
            return;
        }
    }

    a->flush_thread = rb_thread_create((VALUE (*)(void *)) agent_run_flush_thread, (void *) agent_value);
}


static VALUE agent_atfork_in_child(VALUE self) {
    agent_ensure_flush_thread(self);
    return Qnil;
}

static void agent_tphook_newobj(VALUE tpval, void *data) {
    struct agent *a = (struct agent *)data;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);

    if (rb_random_real(rb_cRandom) > a->allocation_sample_rate) {
        // not sampled.
        return;
    }
    if (agent_is_buffer_full(a)) {
        // Buffer is full - need to flush before collecting more samples.
        return;
    }

    // The struct should already be pre-zeroed with ruby_xmalloc.
    struct agent_event *ev = &a->ev_buffer[a->ev_buffer_current_count];
    a->ev_buffer_current_count++;
    VALUE obj = rb_tracearg_object(tparg);
    ev->event_type = AGENT_EVENT_TYPE_NEWOBJ;
    clock_gettime(CLOCK_MONOTONIC, &ev->event_time);
    ev->newobj.object_id = a->object_id_counter++;
    agent_backtrace_as_cstr(&ev->newobj.backtrace, &ev->newobj.backtrace_length);
    rb_st_insert(a->live_objects, obj, ev->newobj.object_id);
}

static void agent_tphook_freeobj(VALUE tpval, void *data) {
    struct agent *a = (struct agent *)data;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE freed_obj = rb_tracearg_object(tparg);
    uint64_t object_id;
    if (rb_st_delete(a->live_objects, (st_data_t *)&freed_obj, (st_data_t *)&object_id)) {
        if (!agent_is_buffer_full(a)) {
            struct agent_event *ev = &a->ev_buffer[a->ev_buffer_current_count];
            a->ev_buffer_current_count++;
            ev->event_type = AGENT_EVENT_TYPE_FREEOBJ;
            clock_gettime(CLOCK_MONOTONIC, &ev->event_time);
            ev->freeobj.object_id = object_id;
        }
    }
}

static VALUE agent_initialize(VALUE self) {
    struct agent *a = agent_get(self);

    // Set the defaults (they might be overridden by the setters later)
    a->allocation_sample_rate = 0.001;
    a->flush_interval = 10000;
    a->ev_buffer_capacity = 100;

    return Qnil;
}

static VALUE agent_enable_profiling(VALUE self) {
    struct agent *a = agent_get(self);

    if (a->running++) {
        // already running!
        return Qnil;
    }

    // Allocate the active collection buffer
    a->ev_buffer = ruby_xmalloc2(a->ev_buffer_capacity, sizeof(struct agent_event));
    memset(a->ev_buffer, 0, a->ev_buffer_capacity * sizeof(struct agent_event));
    a->ev_buffer_current_count = 0;
    a->live_objects = rb_st_init_numtable();

    agent_ensure_flush_thread(self);

    if (a->newobj_trace == Qnil) {
        a->newobj_trace = rb_tracepoint_new(
            0, RUBY_INTERNAL_EVENT_NEWOBJ, agent_tphook_newobj, a
        );
    }
    if (a->freeobj_trace == Qnil) {
        a->freeobj_trace = rb_tracepoint_new(
            0, RUBY_INTERNAL_EVENT_FREEOBJ, agent_tphook_freeobj, a
        );
    }
    rb_tracepoint_enable(a->newobj_trace);
    rb_tracepoint_enable(a->freeobj_trace);
    return Qnil;
}

static VALUE agent_disable_profiling(VALUE self) {
    struct agent *a = agent_get(self);

    if (a->running > 0) {
        a->running--;
    }
    if (a->running > 0) {
        // still running
        return Qnil;
    }

    rb_tracepoint_disable(a->newobj_trace);
    rb_tracepoint_disable(a->freeobj_trace);

    ruby_xfree(a->ev_buffer);
    a->ev_buffer = NULL;
    a->ev_buffer_current_count = 0;
    rb_st_free_table(a->live_objects);
    a->live_objects = NULL;

    return Qnil;
}

static VALUE agent_allocation_sample_rate_set(VALUE self, VALUE newval) {
    struct agent *a = agent_get(self);
    a->allocation_sample_rate = rb_num2dbl(newval);
    return newval;
}

static VALUE agent_allocation_sample_rage_get(VALUE self) {
    struct agent *a = agent_get(self);
    return rb_float_new(a->allocation_sample_rate);
}

static VALUE agent_flush_interval_set(VALUE self, VALUE newval) {
  struct agent *a = agent_get(self);
  a->flush_interval = rb_num2dbl(newval);
  return newval;
}

static VALUE agent_flush_interval_get(VALUE self) {
    struct agent *a = agent_get(self);
    return rb_float_new(a->flush_interval);
}

static VALUE agent_logger_get(VALUE self) {
    struct agent *a = agent_get(self);
    return a->logger;
}

static VALUE agent_logger_set(VALUE self, VALUE newval) {
    struct agent *a = agent_get(self);
    a->logger = newval;
    return a->logger;
}



void Init_ruby_memory_monitor_agent_ext() {
    rb_ext_ractor_safe(true);

    mRubyMemoryMonitor = rb_define_module("RubyMemoryMonitor");
    cAgent = rb_define_class_under(mRubyMemoryMonitor, "Agent", rb_cObject);
    rb_define_alloc_func(cAgent, agent_alloc);

    rb_define_method(cAgent, "initialize", agent_initialize, 0);
    rb_define_method(cAgent, "allocation_sample_rate", agent_allocation_sample_rage_get, 0);
    rb_define_method(cAgent, "allocation_sample_rate=", agent_allocation_sample_rate_set, 1);
    rb_define_method(cAgent, "flush_interval", agent_flush_interval_get, 0);
    rb_define_method(cAgent, "flush_interval=", agent_flush_interval_set, 1);
    rb_define_method(cAgent, "logger", agent_logger_get, 0);
    rb_define_method(cAgent, "logger=", agent_logger_set, 1);

    rb_define_method(cAgent, "enable_profiling!", agent_enable_profiling, 0);
    rb_define_method(cAgent, "disable_profiling!", agent_disable_profiling, 0);
    rb_define_method(cAgent, "atfork_in_child", agent_atfork_in_child, 0);
}

