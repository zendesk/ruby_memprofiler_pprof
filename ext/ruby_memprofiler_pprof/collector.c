#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ruby.h>
#include <ruby/debug.h>
#include <vm_core.h>
#include <iseq.h>

#include "ruby_memprofiler_pprof.h"


static rb_callable_method_entry_t *
check_method_entry(VALUE obj, int can_be_svar)
{
    if (obj == Qfalse) return NULL;

#if VM_CHECK_MODE > 0
    if (!RB_TYPE_P(obj, T_IMEMO)) rb_bug("check_method_entry: unknown type: %s", rb_obj_info(obj));
#endif

    switch (imemo_type(obj)) {
    case imemo_ment:
        return (rb_callable_method_entry_t *)obj;
    case imemo_cref:
        return NULL;
    case imemo_svar:
        if (can_be_svar) {
            return check_method_entry(((struct vm_svar *)obj)->cref_or_me, 0);
        }
    default:
#if VM_CHECK_MODE > 0
        rb_bug("check_method_entry: svar should not be there:");
#endif
        return NULL;
    }
}

__attribute__(( visibility("hidden") ))
const rb_callable_method_entry_t *
rb_vm_frame_method_entry(const rb_control_frame_t *cfp)
{
    const VALUE *ep = cfp->ep;
    rb_callable_method_entry_t *me;

    while (!VM_ENV_LOCAL_P(ep)) {
        if ((me = check_method_entry(ep[VM_ENV_DATA_INDEX_ME_CREF], 0)) != NULL) return me;
        ep = VM_ENV_PREV_EP(ep);
    }
    return check_method_entry(ep[VM_ENV_DATA_INDEX_ME_CREF], 1);
}


static VALUE mMemprofilerPprof;
static VALUE cCollector;

struct collector_cdata {
    VALUE newobj_trace;                 // Ruby Tracepoint object for newobj hook
    VALUE freeobj_trace;                // Ruby Tracepoint object for freeobj hook
    st_table *live_objects;             // List of currently-live objects we sampled
    uint32_t u32_sample_rate;           // How often (as a fraction of UINT32_MAX) we should sample allocations; must
                                        //   be accessed through atomics.
    int is_tracing;                     // This flag is used to make sure we detach our tracepoints as we're
                                        //   getting GC'd.

    pthread_mutex_t lock;               // Internal lock for sample list
    struct internal_sample *samples;    // Head of a linked list of samples
    size_t sample_count;                // Number of elements currently in the sample list

    struct str_intern_tab string_tab;   // String interning table
    VALUE native_cfunc_str;             // Retained VALUE representation of "(native cfunc)"
};

static void internal_sample_decrement_refcount(struct internal_sample *s) {
    s->refcount--;
    if (!s->refcount) {
        mpp_free(s->bt_frames);
        mpp_free(s);
    }
}

static int collector_cdata_gc_decrement_live_object_refcounts(st_data_t key, st_data_t value, st_data_t arg) {
    struct internal_sample *s = (struct internal_sample *)value;
    internal_sample_decrement_refcount(s);
    return ST_CONTINUE;
}

static void collector_cdata_gc_free_live_object_table(struct collector_cdata *cd) {
    if (cd->live_objects) {
        rb_st_foreach(cd->live_objects, collector_cdata_gc_decrement_live_object_refcounts, 0);
        rb_st_free_table(cd->live_objects);
    }
    cd->live_objects = NULL;
}

static void internal_sample_list_decrement_refcount(struct internal_sample *s) {
    while (s) {
        struct internal_sample *next_s = s->next_alloc;
        internal_sample_decrement_refcount(s);
        s = next_s;
    }
}

static void collector_cdata_gc_decrement_sample_refcounts(struct collector_cdata *cd) {
    internal_sample_list_decrement_refcount(cd->samples);
    cd->samples = NULL;
}

static int collector_cdata_gc_memsize_live_objects(st_data_t key, st_data_t value, st_data_t arg) {
    size_t *acc_ptr = (size_t *)arg;
    struct internal_sample *s = (struct internal_sample *)value;

    // Only consider the live object list to be holding the backtrace, for accounting purposes, if it's
    // not also in the allocation sample list.
    if (s->refcount == 1) {
        *acc_ptr += sizeof(*s);
        *acc_ptr += s->bt_frames_count * sizeof(*s->bt_frames);
    }
    return ST_CONTINUE;
}

static void collector_cdata_gc_free_strtab(struct collector_cdata *cd) {
    if (cd->string_tab.initialized) {
        mpp_strtab_destroy(&cd->string_tab);
    }
}

static void collector_cdata_gc_mark(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    rb_gc_mark_movable(cd->newobj_trace);
    rb_gc_mark_movable(cd->freeobj_trace);
    rb_gc_mark_movable(cd->native_cfunc_str);
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

    // Needed in case there are any in-flight tracepoints we just disabled above.
    mpp_pthread_mutex_lock(&cd->lock);

    collector_cdata_gc_free_live_object_table(cd);
    collector_cdata_gc_decrement_sample_refcounts(cd);
    collector_cdata_gc_free_strtab(cd);

    mpp_pthread_mutex_unlock(&cd->lock);
    mpp_pthread_mutex_destroy(&cd->lock);

    ruby_xfree(ptr);
}

static size_t collector_cdata_memsize(const void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    size_t sz = sizeof(*cd);
    if (cd->live_objects) {
        rb_st_foreach(cd->live_objects, collector_cdata_gc_memsize_live_objects, (st_data_t)&sz);
        sz += rb_st_memsize(cd->live_objects);
    }
    if (cd->string_tab.initialized) {
        sz += mpp_strtab_memsize(&cd->string_tab);
    }
    struct internal_sample *s = cd->samples;
    while (s) {
        sz += sizeof(*s);
        sz += s->bt_frames_count * sizeof(*s->bt_frames);
        s = s->next_alloc;
    }

    return sz;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
// Support VALUES we're tracking being moved away in Ruby 2.7+ with GC.compact
static void collector_cdata_gc_compact(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    cd->newobj_trace = rb_gc_location(cd->newobj_trace);
    cd->freeobj_trace = rb_gc_location(cd->freeobj_trace);
    cd->native_cfunc_str = rb_gc_location(cd->native_cfunc_str);
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

    cd->newobj_trace = Qnil;
    cd->freeobj_trace = Qnil;
    cd->is_tracing = 0;
    cd->samples = NULL;
    cd->sample_count = 0;
    cd->live_objects = NULL;
    memset(&cd->string_tab, 0, sizeof(cd->string_tab));
    __atomic_store_n(&cd->u32_sample_rate, 0, __ATOMIC_SEQ_CST);
    cd->native_cfunc_str = rb_str_new_cstr("(native cfunc)");
    mpp_pthread_mutex_init(&cd->lock, 0);

    return v;
}

struct newobj_impl_args {
    struct collector_cdata *cd;
    VALUE tpval;
};

static VALUE collector_tphook_newobj_impl(VALUE args_as_uintptr) {
    struct newobj_impl_args *args = (struct newobj_impl_args*)args_as_uintptr;
    struct collector_cdata *cd = args->cd;
    VALUE tpval = args->tpval;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE newobj = rb_tracearg_object(tparg);

    // Create the sample object.
    struct internal_sample *sample = mpp_xmalloc(sizeof(struct internal_sample));

    // Take inspiration from vm_backtrace.c's backtrace_each to iterate through the backtrace frames.
    const rb_control_frame_t *last_cfp = GET_EC()->cfp;
    const rb_control_frame_t *start_cfp = RUBY_VM_END_CONTROL_FRAME(GET_EC());

    // Allegedly, according to vm_backtrace.c, we need to skip the first two control frames because they
    // are "dummy frames", whatever that means.
    start_cfp = RUBY_VM_NEXT_CONTROL_FRAME(start_cfp);
    start_cfp = RUBY_VM_NEXT_CONTROL_FRAME(start_cfp);

    // Calculate how many frames are in this backtrace.
    ptrdiff_t backtrace_size;
    if (start_cfp < last_cfp) {
        backtrace_size = 0;
    } else {
        backtrace_size = start_cfp - last_cfp + 1;
    }

    sample->bt_frames = mpp_xmalloc(sizeof(struct internal_sample_bt_frame) * backtrace_size);
    sample->bt_frames_count = 0; // Will be incremented.

    ptrdiff_t i;
    const rb_control_frame_t *cfp;
    for (i = 0, cfp = start_cfp; i < backtrace_size; i++, cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp)) {
        int found = 0;

        uint64_t lineno;
        VALUE filepath_val;
        VALUE method_name;
        uint64_t method_id;

        if (cfp->iseq) {
            if (cfp->pc) {
                // I believe means this backtrace frame is ruby code

                // These two lines are essentially calc_lineno
                size_t iseq_pos = (size_t)(cfp->pc - cfp->iseq->body->iseq_encoded);
                lineno = rb_iseq_line_no(cfp->iseq, iseq_pos);
                filepath_val = rb_iseq_path(cfp->iseq);
                method_name = rb_iseq_method_name(cfp->iseq);

                // The method name RString should be the same one, so use its object ID
                // as our method id.
                method_id = FIX2ULONG(rb_obj_id(method_name));

                found = 1;
            }
        } else if (RUBYVM_CFUNC_FRAME_P(cfp)) {
            // I believe means that this backtrace frame is a call to a cfunc
            const rb_callable_method_entry_t *me = rb_vm_frame_method_entry(cfp);

            // The code in vm_backtrace.c actually just puts the previous file/line combo in the
            // cfunc case. That seems confusing and pointless to me, so let's just go with a constant.
            filepath_val = cd->native_cfunc_str;
            method_name = rb_id2str(me->def->original_id);

            // Use the sym ID for the cfunc (which is interned forever) as the method id..
            method_id = me->def->original_id;

            lineno = 0;

            found = 1;
        } else {
            // No idea what this means. It's silently ignored in vm_backtrace.c. Guess we will too.
        }

        if (found) {
            // Use the lower 48 bits of this (which is the sizeof an address on x86_64), + the line number
            // in the top 16 bits, asd the "location id".
            // For cfuncs, this will obviously just be the same as the method ID, which is fine.
            uint64_t location_id = (lineno << 48)  | (FIX2ULONG(method_name) & 0xFFFFFFFFFFFF0000);
            if (location_id) {
                size_t frame_ix = sample->bt_frames_count;
                mpp_strtab_intern_rbstr(&cd->string_tab, filepath_val, &sample->bt_frames[frame_ix].filename, NULL);
                mpp_strtab_intern_rbstr(&cd->string_tab, method_name, &sample->bt_frames[frame_ix].function_name, NULL);
                sample->bt_frames[frame_ix].lineno = lineno;
                sample->bt_frames[frame_ix].function_id = method_id;
                sample->bt_frames[frame_ix].location_id = location_id;
                sample->bt_frames_count++;
            }
        }
    }

    // Set the sample refcount to two. Once because it's going in the allocation sampling buffer,
    // and once because it's going in the heap profiling set.
    sample->refcount = 2;

    // Insert into allocation profiling list.
    // TODO: enforce a limit on how many things can go here.
    sample->next_alloc = cd->samples;
    cd->samples = sample;

    // And into the heap profiling list.
    rb_st_insert(cd->live_objects, newobj, (st_data_t)sample);

    return Qnil;
}

static void collector_tphook_newobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;
    // Before doing anything involving ruby (slow, because we need to rb_protect it), do the sample
    // random number check in C first. This will be the fast path in most executions of this method.
    // Also do it with an atomic because it's happening before the lock.
    uint32_t sample_rate = __atomic_load_n(&cd->u32_sample_rate, __ATOMIC_SEQ_CST);
    if (mpp_rand() > sample_rate) {
       return;
    }

    // If we can't acquire the mutex (perhaps another ractor has it?), don't block! just skip this
    // sample.
    if (mpp_pthread_mutex_trylock(&cd->lock) != 0) {
        return;
    }

    // OK - run our code in here under rb_protect now so that it cannot longjmp out
    struct newobj_impl_args args;
    args.cd = cd;
    args.tpval = tpval;
    int jump_tag;
    VALUE original_errinfo = rb_errinfo();
    rb_protect(collector_tphook_newobj_impl, (VALUE)&args, &jump_tag);

    // Intentionally ignore the jump tag from rb_protect.
    if (jump_tag) {
        // TODO - delete debugging crap.
        fprintf(stderr, "got exception from protect\n");
        VALUE errinfo = rb_errinfo();
        fprintf(stderr, "errinfo is %lx\n", errinfo);
        VALUE errinfo_str = rb_funcall(errinfo, rb_intern("inspect"), 0);
        fprintf(stderr, "string VALUE errinfo is %lx\n", errinfo_str);
        fprintf(stderr, "exception: %s\n", StringValueCStr(errinfo_str));

        rb_set_errinfo(original_errinfo);
    }

    mpp_pthread_mutex_unlock(&cd->lock);
}

static void collector_tphook_freeobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;

    // We unfortunately do really need the mutex here, because if we don't handle this, we might
    // leave an allocation kicking around in live_objects that has been freed.
    mpp_pthread_mutex_lock(&cd->lock);

    // Definitely do _NOT_ try and run any Ruby code in here. Any allocation will crash
    // the process.
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE freed_obj = rb_tracearg_object(tparg);

    struct internal_sample *sample;
    if (rb_st_delete(cd->live_objects, (st_data_t *)&freed_obj, (st_data_t *)&sample)) {
        // We deleted it out of live objects; decrement its refcount.
        internal_sample_decrement_refcount(sample);
    }

    mpp_pthread_mutex_unlock(&cd->lock);
}

static VALUE collector_enable_profiling_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    mpp_pthread_mutex_lock(&cd->lock);

    collector_cdata_gc_free_live_object_table(cd);
    collector_cdata_gc_decrement_sample_refcounts(cd);
    collector_cdata_gc_free_strtab(cd);

    cd->samples = NULL;
    cd->sample_count = 0;
    cd->live_objects = st_init_numtable();
    mpp_strtab_init(&cd->string_tab);

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

    mpp_pthread_mutex_unlock(&cd->lock);

    return Qnil;
}

static VALUE collector_disable_profiling_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    mpp_pthread_mutex_lock(&cd->lock);

    rb_tracepoint_disable(cd->newobj_trace);
    rb_tracepoint_disable(cd->freeobj_trace);
    cd->is_tracing = 0;

    // Don't clear any of our buffers - it's OK to access the profiling info after calling disable_profiling!

    mpp_pthread_mutex_unlock(&cd->lock);


    return Qnil;
}

static VALUE collector_get_sample_rate_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    uint32_t sample_rate = __atomic_load_n(&cd->u32_sample_rate, __ATOMIC_SEQ_CST);
    return rb_dbl2big(((double)sample_rate)/UINT32_MAX);
}

static VALUE collector_set_sample_rate_cimpl(VALUE self, VALUE new_sample_rate_value) {
    struct collector_cdata *cd = collector_cdata_get(self);
    double dbl_sample_rate = rb_num2dbl(new_sample_rate_value);
    // Convert the double sample rate (between 0 and 1) to a value between 0 and UINT32_MAX
    uint32_t new_sample_rate_uint = UINT32_MAX * dbl_sample_rate;
    __atomic_store_n(&cd->u32_sample_rate, new_sample_rate_uint, __ATOMIC_SEQ_CST);
    return Qnil;
}

static VALUE collector_get_running_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    mpp_pthread_mutex_lock(&cd->lock);
    int running = cd->is_tracing;
    mpp_pthread_mutex_unlock(&cd->lock);
    return running ? Qtrue : Qfalse;
}

static VALUE collector_rotate_profile_cimpl(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    // Whilst under the GVL, we need to get the collector lock
    mpp_pthread_mutex_lock(&cd->lock);

    // TODO: yield the GVL here.
    struct pprof_serialize_state *state = rmmp_pprof_serialize_init();
    struct str_intern_tab_index strtab_ix;
    mpp_strtab_index(&cd->string_tab, &strtab_ix);
    rmmp_pprof_serialize_add_strtab(state, &strtab_ix);

    struct internal_sample *sample_list = cd->samples;
    cd->samples = NULL;
    // Now that we have the samples (and have processed the stringtab) we can
    // yield the lock.
    mpp_pthread_mutex_unlock(&cd->lock);

    rmmp_pprof_serialize_add_alloc_samples(state, sample_list);
    internal_sample_list_decrement_refcount(sample_list);


    char *outbuf;
    size_t outbuf_len;
    int abort_flag = 0;
    rmmp_pprof_serialize_to_memory(state, &outbuf, &outbuf_len, &abort_flag);

    // Copy the value back into ruby world.
    VALUE retstring = rb_str_new(outbuf, outbuf_len);

    rmmp_pprof_serialize_destroy(state);
    mpp_strtab_index_destroy(&strtab_ix);
    return retstring;
}

void setup_collector_class() {
    cCollector = rb_define_class_under(mMemprofilerPprof, "Collector", rb_cObject);
    rb_define_alloc_func(cCollector, collector_alloc);

    rb_define_private_method(cCollector, "enable_profiling_cimpl", collector_enable_profiling_cimpl, 0);
    rb_define_private_method(cCollector, "disable_profiling_cimpl", collector_disable_profiling_cimpl, 0);
    rb_define_private_method(cCollector, "get_sample_rate_cimpl", collector_get_sample_rate_cimpl, 0);
    rb_define_private_method(cCollector, "set_sample_rate_cimpl", collector_set_sample_rate_cimpl, 1);
    rb_define_private_method(cCollector, "rotate_profile_cimpl", collector_rotate_profile_cimpl, 0);
    rb_define_private_method(cCollector, "get_running_cimpl", collector_get_running_cimpl, 0);
}

void Init_ruby_memprofiler_pprof_ext() {
    mpp_rand_init();
    rb_ext_ractor_safe(true);
    mMemprofilerPprof = rb_define_module("MemprofilerPprof");
    setup_collector_class();
}
