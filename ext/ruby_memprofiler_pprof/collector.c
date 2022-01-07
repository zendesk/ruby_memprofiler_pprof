#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ruby.h>
#include <ruby/debug.h>
#include <vm_core.h>
#include <iseq.h>

#include "ruby_memprofiler_pprof.h"

VALUE cCollector;

struct collector_cdata {
    VALUE newobj_trace;                 // Ruby Tracepoint object for newobj hook
    VALUE freeobj_trace;                // Ruby Tracepoint object for freeobj hook
    st_table *live_objects;             // List of currently-live objects we sampled
    uint32_t u32_sample_rate;           // How often (as a fraction of UINT32_MAX) we should sample allocations; must
                                        //   be accessed through atomics.
    bool is_tracing;                    // This flag is used to make sure we detach our tracepoints as we're
                                        //   getting GC'd.
    int64_t max_samples;                // Max number of samples to collect in the *samples buffer.

    pthread_mutex_t lock;               // Internal lock for sample list
    struct mpp_sample *samples;         // Head of a linked list of samples
    size_t sample_count;                // Number of elements currently in the sample list

    struct mpp_strtab *string_tab;      // String interning table
    struct mpp_rb_loctab *loctab;       // Backtrace location table
};

static void internal_sample_decrement_refcount(struct collector_cdata *cd, struct mpp_sample *s) {
    s->refcount--;
    if (!s->refcount) {
        mpp_rb_backtrace_destroy(cd->loctab, s->bt);
        mpp_free(s);
    }
}

static int collector_cdata_gc_decrement_live_object_refcounts(st_data_t key, st_data_t value, st_data_t arg) {
    struct mpp_sample *s = (struct mpp_sample *)value;
    struct collector_cdata *cd = (struct collector_cdata *)arg;
    internal_sample_decrement_refcount(cd, s);
    return ST_CONTINUE;
}

static void collector_cdata_gc_free_live_object_table(struct collector_cdata *cd) {
    if (cd->live_objects) {
        st_foreach(cd->live_objects, collector_cdata_gc_decrement_live_object_refcounts, (st_data_t)cd);
        st_free_table(cd->live_objects);
    }
    cd->live_objects = NULL;
}

static void internal_sample_list_decrement_refcount(struct collector_cdata *cd, struct mpp_sample *s) {
    while (s) {
        struct mpp_sample *next_s = s->next_alloc;
        internal_sample_decrement_refcount(cd, s);
        s = next_s;
    }
}

static void collector_cdata_gc_decrement_sample_refcounts(struct collector_cdata *cd) {
    internal_sample_list_decrement_refcount(cd, cd->samples);
    cd->samples = NULL;
}

static int collector_cdata_gc_memsize_live_objects(st_data_t key, st_data_t value, st_data_t arg) {
    size_t *acc_ptr = (size_t *)arg;
    struct mpp_sample *s = (struct mpp_sample *)value;

    // Only consider the live object list to be holding the backtrace, for accounting purposes, if it's
    // not also in the allocation sample list.
    if (s->refcount == 1) {
        *acc_ptr += sizeof(*s);
        *acc_ptr += mpp_rb_backtrace_memsize(s->bt);
    }
    return ST_CONTINUE;
}

static void collector_cdata_gc_free_loctab(struct collector_cdata *cd) {
    if (cd->loctab) {
        mpp_rb_loctab_destroy(cd->loctab);
    }
}

static void collector_cdata_gc_free_strtab(struct collector_cdata *cd) {
    if (cd->string_tab) {
        mpp_strtab_destroy(cd->string_tab);
    }
}

static void collector_cdata_gc_mark(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    rb_gc_mark_movable(cd->newobj_trace);
    rb_gc_mark_movable(cd->freeobj_trace);
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
    collector_cdata_gc_free_loctab(cd);
    collector_cdata_gc_free_strtab(cd);

    mpp_pthread_mutex_unlock(&cd->lock);
    mpp_pthread_mutex_destroy(&cd->lock);

    ruby_xfree(ptr);
}

static size_t collector_cdata_memsize(const void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    size_t sz = sizeof(*cd);
    if (cd->live_objects) {
        st_foreach(cd->live_objects, collector_cdata_gc_memsize_live_objects, (st_data_t)&sz);
        sz += st_memsize(cd->live_objects);
    }
    if (cd->string_tab) {
        sz += mpp_strtab_memsize(cd->string_tab);
    }
    if (cd->loctab) {
        sz += mpp_rb_loctab_memsize(cd->loctab);
    }
    struct mpp_sample *s = cd->samples;
    while (s) {
        sz += sizeof(*s);
        sz += mpp_rb_backtrace_memsize(s->bt);
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
}
#endif

static const rb_data_type_t collector_cdata_type = {
    "collector_cdata",
    {
        collector_cdata_gc_mark, collector_cdata_gc_free, collector_cdata_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
        collector_cdata_gc_compact,
#endif
        { 0 }, /* reserved */
    },
    /* parent, data, [ flags ] */
    NULL, NULL, 0
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
    cd->string_tab = NULL;
    __atomic_store_n(&cd->u32_sample_rate, 0, __ATOMIC_SEQ_CST);
    mpp_pthread_mutex_init(&cd->lock, 0);

    return v;
}

struct initialize_protected_args {
    int argc;
    VALUE *argv;
    VALUE self;
    struct collector_cdata *cd;
};

static VALUE collector_initialize_protected(VALUE vargs) {
    struct initialize_protected_args *args = (struct initialize_protected_args *)vargs;
    struct collector_cdata *cd = args->cd;

    // Argument parsing
    VALUE kwargs_hash = Qnil;
    rb_scan_args_kw(RB_SCAN_ARGS_LAST_HASH_KEYWORDS, args->argc, args->argv, "00:", &kwargs_hash);
    VALUE kwarg_values[2];
    ID kwarg_ids[2];
    kwarg_ids[0] = rb_intern("sample_rate");
    kwarg_ids[1] = rb_intern("max_samples");
    rb_get_kwargs(kwargs_hash, kwarg_ids, 0, 2, kwarg_values);

    // Default values...
    if (kwarg_values[0] == Qundef) kwarg_values[0] = DBL2NUM(0.01);
    if (kwarg_values[1] == Qundef) kwarg_values[1] = LONG2NUM(10000);

    // Convert the double sample rate (between 0 and 1) to a value between 0 and UINT32_MAX
    // Don't know if setting it with an atomic is _strictly_ nescessary, but I did say
    // "all access to u32_sample_rate is through atomics" so it's probably easier to than not.
    __atomic_store_n(&cd->u32_sample_rate, (uint32_t)(UINT32_MAX * NUM2DBL(kwarg_values[0])), __ATOMIC_SEQ_CST);
    cd->max_samples = NUM2LONG(kwarg_values[1]);

    return Qnil;
}

static VALUE collector_initialize(int argc, VALUE *argv, VALUE self) {
    // Need to do this rb_protect dance to ensure that all access to collector_cdata is through the mutex.
    struct initialize_protected_args args;
    args.argc = argc;
    args.argv = argv;
    args.self = self;
    args.cd = collector_cdata_get(self);

    mpp_pthread_mutex_lock(&args.cd->lock);
    int jump_tag = 0;
    VALUE r = rb_protect(collector_initialize_protected, (VALUE)&args, &jump_tag);
    mpp_pthread_mutex_unlock(&args.cd->lock);
    if (jump_tag) rb_jump_tag(jump_tag);
    return r;
}

static VALUE collector_get_sample_rate(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    uint32_t sample_rate = __atomic_load_n(&cd->u32_sample_rate, __ATOMIC_SEQ_CST);
    return DBL2NUM(((double)sample_rate)/UINT32_MAX);
}

static VALUE collector_set_sample_rate(VALUE self, VALUE newval) {
    struct collector_cdata *cd = collector_cdata_get(self);
    double dbl_sample_rate = NUM2DBL(newval);
    // Convert the double sample rate (between 0 and 1) to a value between 0 and UINT32_MAX
    uint32_t new_sample_rate_uint = UINT32_MAX * dbl_sample_rate;
    __atomic_store_n(&cd->u32_sample_rate, new_sample_rate_uint, __ATOMIC_SEQ_CST);
    return newval;
}

static VALUE collector_get_max_samples(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    mpp_pthread_mutex_lock(&cd->lock);
    int64_t v = cd->max_samples;
    mpp_pthread_mutex_unlock(&cd->lock);
    return LONG2NUM(v);
}

static VALUE collector_set_max_samples(VALUE self, VALUE newval) {
    struct collector_cdata *cd = collector_cdata_get(self);
    int64_t v = NUM2LONG(newval);
    mpp_pthread_mutex_lock(&cd->lock);
    cd->max_samples = v;
    mpp_pthread_mutex_unlock(&cd->lock);
    return newval;
}

struct newobj_impl_args {
    struct collector_cdata *cd;
    struct mpp_rb_backtrace *bt;
    VALUE tpval;
    VALUE newobj;
};

// Collects all the parts of collector_tphook_newobj that could throw.
static VALUE collector_tphook_newobj_protected(VALUE args_as_uintptr) {
    struct newobj_impl_args *args = (struct newobj_impl_args*)args_as_uintptr;
    struct collector_cdata *cd = args->cd;
    VALUE tpval = args->tpval;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    args->newobj = rb_tracearg_object(tparg);
    mpp_rb_backtrace_capture(cd->loctab, &args->bt);
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
    args.bt = NULL;
    int jump_tag;
    VALUE original_errinfo = rb_errinfo();
    rb_protect(collector_tphook_newobj_protected, (VALUE)&args, &jump_tag);

    // Intentionally ignore the jump tag from rb_protect.
    if (jump_tag) {
        // Free the stuff that was created by _protected above
        if (args.bt) mpp_rb_backtrace_destroy(cd->loctab, args.bt);
        rb_set_errinfo(original_errinfo);
    } else {
        // No error was thrown, add it to our sample buffers.
        struct mpp_sample *sample = mpp_xmalloc(sizeof(struct mpp_sample));
        // Set the sample refcount to two. Once because it's going in the allocation sampling buffer,
        // and once because it's going in the heap profiling set.
        sample->refcount = 2;
        sample->bt = args.bt;

        // Insert into allocation profiling list.
        // TODO: enforce a limit on how many things can go here.
        sample->next_alloc = cd->samples;
        cd->samples = sample;
        // And into the heap profiling list.
        st_insert(cd->live_objects, args.newobj, (st_data_t)sample);
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

    struct mpp_sample *sample;
    if (st_delete(cd->live_objects, (st_data_t *)&freed_obj, (st_data_t *)&sample)) {
        // We deleted it out of live objects; decrement its refcount.
        internal_sample_decrement_refcount(cd, sample);
    }

    mpp_pthread_mutex_unlock(&cd->lock);
}

static VALUE collector_start_protected(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

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
    return Qnil;
}

static VALUE collector_start(VALUE self) {
    int jump_tag = 0;
    struct collector_cdata *cd = collector_cdata_get(self);
    mpp_pthread_mutex_lock(&cd->lock);
    if (cd->is_tracing) goto out;

    // Do the things that might throw first.
    rb_protect(collector_start_protected, self, &jump_tag);
    if (jump_tag) goto out;

    // Now it's safe to allocate our memory here without risk of it getting
    // stranded. Note that the tracepoint we installed above cannot actually _run_
    // until we release the lock below so it's perfectly safe to have the tracepoint
    // installed right now.
    collector_cdata_gc_free_live_object_table(cd);
    collector_cdata_gc_decrement_sample_refcounts(cd);
    collector_cdata_gc_free_loctab(cd);
    collector_cdata_gc_free_strtab(cd);

    cd->samples = NULL;
    cd->sample_count = 0;
    cd->live_objects = st_init_numtable();
    cd->string_tab = mpp_strtab_new();
    cd->loctab = mpp_rb_loctab_new(cd->string_tab);
    cd->is_tracing = true;

out:
    mpp_pthread_mutex_unlock(&cd->lock);
    if (jump_tag) {
        rb_jump_tag(jump_tag);
    }
    return Qnil;
}

static VALUE collector_stop_protected(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    rb_tracepoint_disable(cd->newobj_trace);
    rb_tracepoint_disable(cd->freeobj_trace);
    return Qnil;
}

static VALUE collector_stop(VALUE self) {
    int jump_tag = 0;
    struct collector_cdata *cd = collector_cdata_get(self);
    mpp_pthread_mutex_lock(&cd->lock);
    if (!cd->is_tracing) goto out;

    rb_protect(collector_stop_protected, self, &jump_tag);
    if (jump_tag) goto out;

    cd->is_tracing = false;
    // Don't clear any of our buffers - it's OK to access the profiling info after calling stop!
out:
    mpp_pthread_mutex_unlock(&cd->lock);
    if (jump_tag) {
        rb_jump_tag(jump_tag);
    }
    return Qnil;
}

static VALUE collector_is_running(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    mpp_pthread_mutex_lock(&cd->lock);
    bool running = cd->is_tracing;
    mpp_pthread_mutex_unlock(&cd->lock);
    return running ? Qtrue : Qfalse;
}

struct collector_rotate_profile_cimpl_prepresult_args {
    const char *pprofbuf;
    size_t pprofbuf_len;
};

static VALUE collector_rotate_profile_cimpl_prepresult(VALUE vargs) {
    struct collector_rotate_profile_cimpl_prepresult_args *args =
        (struct collector_rotate_profile_cimpl_prepresult_args *)vargs;
    return rb_str_new(args->pprofbuf, args->pprofbuf_len);
}

static VALUE collector_flush(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    struct mpp_pprof_serctx *serctx = NULL;
    char *buf_out;
    size_t buflen_out;
    char errbuf[256];
    int jump_tag = 0;
    int r = 0;
    VALUE retval = Qundef;

    // Whilst under the GVL, we need to get the collector lock
    mpp_pthread_mutex_lock(&cd->lock);

    struct mpp_sample *sample_list = cd->samples;
    cd->samples = NULL;

    serctx = mpp_pprof_serctx_new();
    MPP_ASSERT_MSG(serctx, "mpp_pprof_serctx_new failed??");
    r = mpp_pprof_serctx_set_loctab(serctx, cd->loctab, errbuf, sizeof(errbuf));
    if (r == -1) {
        goto out;
    }

    // Now that we have the samples (and have processed the stringtab) we can
    // yield the lock.
    mpp_pthread_mutex_unlock(&cd->lock);

    struct mpp_sample *s = sample_list;
    while (s) {
        r = mpp_pprof_serctx_add_sample(serctx, s, errbuf, sizeof(errbuf));
        if (r == -1) {
            goto out;
        }
        s = s->next_alloc;
    }


    r = mpp_pprof_serctx_serialize(serctx, &buf_out, &buflen_out, errbuf, sizeof(errbuf));
    if ( r == -1) {
        goto out;
    }
    // Annoyingly, since rb_str_new could (in theory) throw, we have to rb_protect the whole construction
    // of our return value to ensure we don't leak serctx.
    struct collector_rotate_profile_cimpl_prepresult_args prepresult_args;
    prepresult_args.pprofbuf = buf_out;
    prepresult_args.pprofbuf_len = buflen_out;
    retval = rb_protect(collector_rotate_profile_cimpl_prepresult, (VALUE)&prepresult_args, &jump_tag);

    // Do cleanup here now.
out:
    if (serctx) {
        mpp_pprof_serctx_destroy(serctx);
    }

    // Now return-or-raise back to ruby.
    if (jump_tag) {
        rb_jump_tag(jump_tag);
    }
    if (retval == Qundef) {
        // Means we have an error to construct and throw
        rb_raise(rb_eRuntimeError, "ruby_memprofiler_pprof failed serializing pprof protobuf: %s", errbuf);
    }
    return retval;

    RB_GC_GUARD(self);
}

void mpp_setup_collector_class() {
    cCollector = rb_define_class_under(mMemprofilerPprof, "Collector", rb_cObject);
    rb_define_alloc_func(cCollector, collector_alloc);

    rb_define_method(cCollector, "initialize", collector_initialize, -1);
    rb_define_method(cCollector, "sample_rate", collector_get_sample_rate, 0);
    rb_define_method(cCollector, "sample_rate=", collector_set_sample_rate, 1);
    rb_define_method(cCollector, "max_samples", collector_get_max_samples, 0);
    rb_define_method(cCollector, "max_samples=", collector_set_max_samples, 1);
    rb_define_method(cCollector, "running?", collector_is_running, 0);
    rb_define_method(cCollector, "start!", collector_start, 0);
    rb_define_method(cCollector, "stop!", collector_stop, 0);
    rb_define_method(cCollector, "flush", collector_flush, 0);
}
