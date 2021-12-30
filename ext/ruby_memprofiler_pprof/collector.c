#include <ruby.h>
#include <ruby/debug.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat.h"

static VALUE mMemprofilerPprof;
static VALUE cCollector;
static ID ivar_sample_rate;
static ID ivar_sample_buffer;
static ID ivar_sample_buffer_next_ix;
static ID ivar_missed_samples;
static ID ivar_max_samples;
static ID sym_caller_locations;

struct collector_cdata {
    VALUE newobj_trace;
    VALUE freeobj_trace;

    // Keeps track of what objects that we sampled are still live; we used this to handle
    // heap profiles (as opposed to allocation profiles).
    st_table *live_objects;

    // Note that we keep a copy of this in an ivar as well, but we need to have access to it
    // in contexts where it's not necessarily safe to call Ruby.
    uint32_t u32_sample_rate;

    // Reference to self. Note that we actually have to mark this because otherwise we might
    // not notice if it got moved.
    VALUE self;

    // This flag is set very briefly if we're in the middle of swapping over the sample buffer
    // ivars; a memory allocation in the middle of that needs to not be samplked.
    int ivar_swapping;

    // This flag is used to make sure we detach our tracepoints as we're getting GC'd.
    int is_tracing;
};

static int collector_cdata_gc_mark_live_objects(st_data_t key, st_data_t value, st_data_t arg) {
    rb_gc_mark_movable(value);
    return ST_CONTINUE;
}

static void collector_cdata_gc_mark(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    rb_gc_mark_movable(cd->newobj_trace);
    rb_gc_mark_movable(cd->freeobj_trace);
    rb_gc_mark_movable(cd->self);

    if (cd->live_objects) {
        rb_st_foreach(cd->live_objects, collector_cdata_gc_mark_live_objects, (st_data_t)cd);
    }
}

static void collector_cdata_gc_free(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    if (cd->is_tracing) {
        if (cd->newobj_trace) {
            rb_tracepoint_disable(cd->newobj_trace);
        }
        if (cd->freeobj_trace) {
            rb_tracepoint_disable(cd->freeobj_trace);
        }
    }
    if (cd->live_objects) {
        rb_st_free_table(cd->live_objects);
    }
    ruby_xfree(ptr);
}

static size_t collector_cdata_memsize(const void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    size_t sz = sizeof(*cd);
    if (cd->live_objects) {
        sz += rb_st_memsize(cd->live_objects);
    }
    return sz;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
// Support VALUES we're tracking being moved away in Ruby 2.7+ with GC.compact
static int collector_cdata_gc_compact_live_objects_check(st_data_t key, st_data_t value, st_data_t arg, int replace) {
    if (rb_gc_location(value) != value) {
        return ST_REPLACE;
    }
    return ST_CONTINUE;
}

static int collector_cdata_gc_compact_live_objects_replace(st_data_t *key, st_data_t *value, st_data_t arg, int replace) {
    *value = rb_gc_location(*value);
    return ST_CONTINUE;
}

static void collector_cdata_gc_compact(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    cd->newobj_trace = rb_gc_location(cd->newobj_trace);
    cd->freeobj_trace = rb_gc_location(cd->freeobj_trace);
    cd->self = rb_gc_location(cd->self);
    if (cd->live_objects) {
        rb_st_foreach_with_replace(
            cd->live_objects,
            collector_cdata_gc_compact_live_objects_check,
            collector_cdata_gc_compact_live_objects_replace,
            (st_data_t)cd
        );
    }
}
#endif

static const rb_data_type_t collector_cdata_type = {
    "collector_cdata",
    {
        collector_cdata_gc_mark, collector_cdata_gc_free, collector_cdata_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
        collector_cdata_gc_compact,
#endif
        /* reserved */
    },
    /* parent, data, [ flags ] */
};

static struct collector_cdata *collector_cdata_get(VALUE self) {
    struct collector_cdata *a;
    TypedData_Get_Struct(self, struct collector_cdata, &collector_cdata_type, a);
    return a;
}

static VALUE collector_alloc(VALUE klass) {
    struct collector_cdata *cd;
    VALUE v = TypedData_Make_Struct(klass, struct collector_cdata, &collector_cdata_type, cd);
    cd->live_objects = NULL;
    cd->self = v;
    cd->newobj_trace = Qnil;
    cd->freeobj_trace = Qnil;
    cd->u32_sample_rate = 0;
    cd->ivar_swapping = 0;
    cd->is_tracing = 0;
    return v;
}

struct newobj_impl_args {
    struct collector_cdata *cd;
    VALUE self;
    VALUE tpval;
};

static VALUE collector_tphook_newobj_impl(VALUE args_as_uintptr) {
    struct newobj_impl_args *args = (struct newobj_impl_args*)args_as_uintptr;
    struct collector_cdata *cd = args->cd;
    VALUE self = args->self;
    VALUE tpval = args->tpval;

    unsigned long sample_buffer_next_ix = rb_num2ulong_inline(rb_ivar_get(self, ivar_sample_buffer_next_ix));
    VALUE sample_buffer = rb_ivar_get(self, ivar_sample_buffer);
    unsigned long sample_buffer_len = rb_array_len(sample_buffer);

    if (sample_buffer_next_ix >= sample_buffer_len) {
        // Sample buffer is full.
        unsigned long missed_samples = rb_num2ulong_inline(rb_ivar_get(self, ivar_missed_samples));
        missed_samples++;
        rb_ivar_set(self, ivar_missed_samples, rb_ulong2num_inline(missed_samples));
        return Qnil;
    }

    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE newobj = rb_tracearg_object(tparg);

    // Sample buffer not fill. Let's add an element.
    VALUE el = rb_ary_entry(sample_buffer, (long)sample_buffer_next_ix);
    rb_ivar_set(self, ivar_sample_buffer_next_ix, rb_ulong2num_inline(sample_buffer_next_ix + 1));

    RSTRUCT_SET(el, 0, Qtrue);
    // RSTRUCT_SET(el, 1, rb_make_backtrace());
    RSTRUCT_SET(el, 1, rb_funcall(rb_mKernel, sym_caller_locations, 0));
    VALUE newobj_klass = rb_class_of(newobj);
    VALUE newobj_klass_real = rb_class_real(rb_class_of(newobj));
    VALUE best_klass_name;
    if (RTEST(newobj_klass_real)) {
        best_klass_name = rb_class_path(newobj_klass_real);
    } else if (RTEST(newobj_klass)) {
        best_klass_name = rb_class_path(newobj_klass);
    } else {
        // This actually happens, a lot. At the very least because THROW_DATA_NEW creates an object
        // with a zero class in Ruby 2.7 (but apparently not in ruby 3?)
        // Later if desired we can peek into the RBasic flags at least by looking at *((VALUE *) newobj).
        best_klass_name = rb_str_new2("(unknown class)");
    }
    RSTRUCT_SET(el, 2, best_klass_name);
    // TODO - collect more!

    // Keep a reference to this allocation in our list of live objects, for heap profiling later.
    rb_st_insert(cd->live_objects, newobj, el);

    return Qnil;
}

static void collector_tphook_newobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;
    // Before doing anything involving ruby (slow, because we need to rb_protect it), do the sample
    // random number check in C first. This will be the fast path in most executions of this method.
    if (rmm_pprof_rand() > cd->u32_sample_rate) {
       return;
    }

    if (cd->ivar_swapping) {
        return;
    }

    // OK - run our ruby code in here under rb_protect now so that it cannot longjmp out
    struct newobj_impl_args args;
    args.cd = cd;
    args.self = cd->self;
    args.tpval = tpval;
    int jump_tag;
    VALUE original_errinfo = rb_errinfo();
    rb_protect(collector_tphook_newobj_impl, (VALUE)&args, &jump_tag);

    // Intentionally ignore the jump tag from rb_protect.
    if (jump_tag) {
        fprintf(stderr, "got exception from protect\n");
        VALUE errinfo = rb_errinfo();
        fprintf(stderr, "errinfo is %lx\n", errinfo);
        VALUE errinfo_str = rb_funcall(errinfo, rb_intern("inspect"), 0);
        fprintf(stderr, "string VALUE errinfo is %lx\n", errinfo_str);
        fprintf(stderr, "exception: %s\n", StringValueCStr(errinfo_str));
    }
    rb_set_errinfo(original_errinfo);
}

static void collector_tphook_freeobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;

    // Definitely do _NOT_ try and run any Ruby code in here. Any allocation will crash
    // the process.
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE freed_obj = rb_tracearg_object(tparg);
    rb_st_delete(cd->live_objects, (st_data_t *)&freed_obj, NULL);
}

static VALUE collector_enable_profiling_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    cd->live_objects = rb_st_init_numtable();

    if (cd->newobj_trace == Qnil) {
        cd->newobj_trace = rb_tracepoint_new(
            0, RUBY_INTERNAL_EVENT_NEWOBJ, collector_tphook_newobj, cd
        );
    }
    if (cd->freeobj_trace == Qnil) {
        cd->freeobj_trace = rb_tracepoint_new(
            0, RUBY_INTERNAL_EVENT_FREEOBJ, collector_tphook_freeobj, cd
        );
    }
    rb_tracepoint_enable(cd->newobj_trace);
    rb_tracepoint_enable(cd->freeobj_trace);
    cd->is_tracing = 1;

    return Qnil;
}

static VALUE collector_disable_profiling_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    rb_tracepoint_disable(cd->newobj_trace);
    rb_tracepoint_disable(cd->freeobj_trace);

    rb_st_free_table(cd->live_objects);
    cd->live_objects = NULL;
    cd->is_tracing = 0;

    return Qnil;
}

static VALUE collector_set_sample_rate_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    double dbl_sample_rate = rb_num2dbl(rb_ivar_get(self, ivar_sample_rate));
    // Convert the double sample rate (between 0 and 1) to a value between 0 and UINT32_MAX
    cd->u32_sample_rate = UINT32_MAX * dbl_sample_rate;
    return Qnil;
}

static VALUE collector_swap_sample_buffer_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    VALUE sample_struct = rb_const_get_at(mMemprofilerPprof, rb_intern("Sample"));

    // This is actually protected by the GVL, it doesn't need atomic access.
    cd->ivar_swapping = 1;

    VALUE old_sample_buffer = rb_ivar_get(self, ivar_sample_buffer);
    long max_sample_size = rb_num2long_inline(rb_ivar_get(self, ivar_max_samples));
    VALUE new_sample_buffer = rb_ary_new_capa(max_sample_size);
    for (int i = 0; i < max_sample_size; i++) {
        VALUE new_sample = rb_struct_new(sample_struct, Qnil, Qnil, Qnil, Qnil);
        rb_ary_push(new_sample_buffer, new_sample);
    }
    rb_ivar_set(self, ivar_sample_buffer, new_sample_buffer);
    rb_ivar_set(self, ivar_sample_buffer_next_ix, rb_ulong2num_inline(0));

    cd->ivar_swapping = 0;
    return old_sample_buffer;
}

void setup_collector_class() {
    cCollector = rb_define_class_under(mMemprofilerPprof, "Collector", rb_cObject);
    rb_define_alloc_func(cCollector, collector_alloc);

    ivar_sample_rate = rb_intern_const("@sample_rate");
    ivar_sample_buffer = rb_intern_const("@sample_buffer");
    ivar_sample_buffer_next_ix = rb_intern_const("@sample_buffer_next_ix");
    ivar_missed_samples = rb_intern_const("@missed_samples");
    ivar_max_samples = rb_intern_const("@max_samples");
    sym_caller_locations = rb_intern_const("caller_locations");

    rb_define_private_method(cCollector, "enable_profiling_cimpl", collector_enable_profiling_cimpl, 0);
    rb_define_private_method(cCollector, "disable_profiling_cimpl", collector_disable_profiling_cimpl, 0);
    rb_define_private_method(cCollector, "set_sample_rate_cimpl", collector_set_sample_rate_cimpl, 0);
    rb_define_private_method(cCollector, "swap_sample_buffer_cimpl", collector_swap_sample_buffer_cimpl, 0);
}

void Init_ruby_memprofiler_pprof_ext() {
    rmm_pprof_rand_init();
    // This extension is decidedly _NOT_ ractor safe at the moment - multiple threads can write to the
    // global Collector instance through the tracepoint so it absolutely must be protected by the GVL.
    rb_ext_ractor_safe(false);
    mMemprofilerPprof = rb_define_module("MemprofilerPprof");
    setup_collector_class();
}
