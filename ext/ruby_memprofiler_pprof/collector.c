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

struct collector_cdata {
    // Internal, cross-ractor lock for this data
    pthread_mutex_t lock;

    // Global variables we need to keep a hold of
    VALUE cCollector;
    VALUE cProfileData;
    VALUE mMemprofilerPprof;

    // Ruby Tracepoint objects for our hooks
    VALUE newobj_trace;
    VALUE freeobj_trace;

    // How often (as a fraction of UINT32_MAX) we should sample allocations
    uint32_t u32_sample_rate;
    // This flag is used to make sure we detach our tracepoints as we're getting GC'd.
    bool is_tracing;

    // ======== Heap samples ========
    // A hash-table keying live VALUEs to their allocation sample. This is _not_ cleared
    // when #flush is called; instead, elements are deleted when they are free'd. This is
    // used for building heap profiles.
    st_table *heap_samples;
    // Number of elements currently in the heap profile hash
    int64_t heap_samples_count;
    // How big the sample table can grow
    int64_t max_heap_samples;

    // ======== Sample drop counters ========
    // Number of samples dropped for want of space in the heap allocation table.
    int64_t dropped_samples_heap_bufsize;

    // String interning table used to keep constant pointers to every string; this saves memory
    // used in backtraces, and also helps us efficiently build up the pprof protobuf format (since that
    // _requires_ that strings are interned in a string table).
    struct mpp_strtab *string_table;
    // Same thing, but for function locations
    struct mpp_functab *function_table;
};

// We need a global list of all collectors, so that, in our atfork handler, we can correctly lock/unlock
// all of their mutexes and guarantee correctness across forks.
static st_table *global_collectors;
static pthread_mutex_t global_collectors_lock;

static void sample_free(struct collector_cdata *cd, struct mpp_sample *s) {
    if (s->processed_into_functab) {
        mpp_functab_del_all_frames(cd->function_table, s->bt);
    }
    mpp_rb_backtrace_destroy(s->bt);
    mpp_free(s);
}

static void sample_refcount_inc(struct mpp_sample *sample) {
    MPP_ASSERT_MSG(sample->refcount, "sample refcount went from zero to one!");
    sample->refcount++;
}

static void sample_refcount_dec(struct collector_cdata *cd, struct mpp_sample *sample) {
    sample->refcount--;
    if (!sample->refcount) {
        sample_free(cd, sample);
    }
}

static int collector_cdata_gc_decrement_live_object_refcounts(st_data_t key, st_data_t value, st_data_t arg) {
    struct mpp_sample *s = (struct mpp_sample *)value;
    struct collector_cdata *cd = (struct collector_cdata *)arg;
    sample_refcount_dec(cd, s);
    return ST_CONTINUE;
}

static void collector_cdata_gc_free_heap_samples(struct collector_cdata *cd) {
    if (cd->heap_samples) {
        st_foreach(cd->heap_samples, collector_cdata_gc_decrement_live_object_refcounts, (st_data_t)cd);
        st_free_table(cd->heap_samples);
    }
    cd->heap_samples = NULL;
}

static int collector_cdata_gc_memsize_live_objects(st_data_t key, st_data_t value, st_data_t arg) {
    size_t *acc_ptr = (size_t *)arg;
    struct mpp_sample *s = (struct mpp_sample *)value;
    *acc_ptr += sizeof(*s);
    *acc_ptr += mpp_rb_backtrace_memsize(s->bt);
    return ST_CONTINUE;
}

static int collector_cdata_gc_mark_heap_samples(st_data_t key, st_data_t value, st_data_t arg) {
    struct mpp_sample *s = (struct mpp_sample *)value;
    mpp_backtrace_gc_mark(s->bt);
    return ST_CONTINUE;
}

static void collector_cdata_gc_mark(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    rb_gc_mark_movable(cd->newobj_trace);
    rb_gc_mark_movable(cd->freeobj_trace);
    rb_gc_mark_movable(cd->mMemprofilerPprof);
    rb_gc_mark_movable(cd->cCollector);
    rb_gc_mark_movable(cd->cProfileData);

    // Mark all the iseqs/CME's we have stored in the heap sample map
    st_foreach(cd->heap_samples, collector_cdata_gc_mark_heap_samples, 0);
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

    collector_cdata_gc_free_heap_samples(cd);
    if (cd->function_table) {
        mpp_functab_destroy(cd->function_table);
    }
    if (cd->string_table) {
        mpp_strtab_destroy(cd->string_table);
    }

    // Remove from global collectors list.
    mpp_pthread_mutex_lock(&global_collectors_lock);
    st_data_t cd_key = (st_data_t)cd;
    st_delete(global_collectors, &cd_key, NULL);
    mpp_pthread_mutex_unlock(&global_collectors_lock);

    mpp_pthread_mutex_unlock(&cd->lock);
    mpp_pthread_mutex_destroy(&cd->lock);

    ruby_xfree(ptr);
}

static size_t collector_cdata_memsize(const void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    size_t sz = sizeof(*cd);
    if (cd->heap_samples) {
        st_foreach(cd->heap_samples, collector_cdata_gc_memsize_live_objects, (st_data_t)&sz);
        sz += st_memsize(cd->heap_samples);
    }
    if (cd->string_table) {
        sz += mpp_strtab_memsize(cd->string_table);
    }
    if (cd->function_table) {
        sz += mpp_functab_memsize(cd->function_table);
    }

    return sz;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
// Support VALUES we're tracking being moved away in Ruby 2.7+ with GC.compact
static int collector_compact_each_heap_sample(st_data_t key, st_data_t value, st_data_t arg) {
    struct collector_cdata *cd = (struct collector_cdata *)arg;
    struct mpp_sample *sample = (struct mpp_sample *)value;

    // Handle compaction of the heap samples themselves
    mpp_backtrace_gc_compact(sample->bt);

    // Handle compaction of our weak reference to the heap sample.
    if (rb_gc_location(sample->allocated_value_weak) == sample->allocated_value_weak) {
        return ST_CONTINUE;
    } else {
        sample->allocated_value_weak = rb_gc_location(sample->allocated_value_weak);
        st_insert(cd->heap_samples, sample->allocated_value_weak, (st_data_t)sample);
        return ST_DELETE;
    }
}

static void collector_cdata_gc_compact(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    cd->newobj_trace = rb_gc_location(cd->newobj_trace);
    cd->freeobj_trace = rb_gc_location(cd->freeobj_trace);
    cd->mMemprofilerPprof = rb_gc_location(cd->mMemprofilerPprof);
    cd->cCollector = rb_gc_location(cd->cCollector);
    cd->cProfileData = rb_gc_location(cd->cProfileData);
    st_foreach(cd->heap_samples, collector_compact_each_heap_sample, (st_data_t)cd);
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

    cd->u32_sample_rate = 0;
    cd->is_tracing = false;
    cd->heap_samples = NULL;
    cd->heap_samples_count = 0;
    cd->max_heap_samples = 0;
    cd->dropped_samples_heap_bufsize = 0;
    cd->string_table = NULL;
    cd->function_table = NULL;

    // Initialize the mutex.
    // It really does need to be recursive - if we call a rb_* function while holding
    // the lock, that could trigger the GC to run and call our freeobj tracepoint,
    // which _also_ needs the lock.
    pthread_mutexattr_t mutex_attr;
    mpp_pthread_mutexattr_init(&mutex_attr);
    mpp_pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    mpp_pthread_mutex_init(&cd->lock, &mutex_attr);
    mpp_pthread_mutexattr_destroy(&mutex_attr);

    // Add us to the global list of collectors, to handle pthread_atfork.
    mpp_pthread_mutex_lock(&global_collectors_lock);
    st_insert(global_collectors, (st_data_t)cd, (st_data_t)cd);
    mpp_pthread_mutex_unlock(&global_collectors_lock);
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

    // Save constants
    cd->mMemprofilerPprof = rb_const_get(rb_cObject, rb_intern("MemprofilerPprof"));
    cd->cCollector = rb_const_get(cd->mMemprofilerPprof, rb_intern("Collector"));
    cd->cProfileData = rb_const_get(cd->mMemprofilerPprof, rb_intern("ProfileData"));

    // Argument parsing
    VALUE kwargs_hash = Qnil;
    rb_scan_args_kw(RB_SCAN_ARGS_LAST_HASH_KEYWORDS, args->argc, args->argv, "00:", &kwargs_hash);
    VALUE kwarg_values[2];
    ID kwarg_ids[2];
    kwarg_ids[0] = rb_intern("sample_rate");
    kwarg_ids[1] = rb_intern("max_heap_samples");
    rb_get_kwargs(kwargs_hash, kwarg_ids, 0, 2, kwarg_values);

    // Default values...
    if (kwarg_values[0] == Qundef) kwarg_values[0] = DBL2NUM(0.01);
    if (kwarg_values[1] == Qundef) kwarg_values[1] = LONG2NUM(50000);

    rb_funcall(args->self, rb_intern("sample_rate="), 1, kwarg_values[0]);
    rb_funcall(args->self, rb_intern("max_heap_samples="), 1, kwarg_values[1]);

    cd->string_table = mpp_strtab_new();
    cd->function_table = mpp_functab_new(cd->string_table);
    cd->heap_samples = st_init_numtable();
    cd->heap_samples_count = 0;

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
    mpp_pthread_mutex_lock(&cd->lock);
    uint32_t u32_sample_rate = cd->u32_sample_rate;
    mpp_pthread_mutex_unlock(&cd->lock);
    return DBL2NUM(((double)u32_sample_rate)/UINT32_MAX);
}

static VALUE collector_set_sample_rate(VALUE self, VALUE newval) {
    struct collector_cdata *cd = collector_cdata_get(self);
    double dbl_sample_rate = NUM2DBL(newval);
    // Convert the double sample rate (between 0 and 1) to a value between 0 and UINT32_MAX
    uint32_t new_sample_rate_uint = UINT32_MAX * dbl_sample_rate;

    mpp_pthread_mutex_lock(&cd->lock);
    cd->u32_sample_rate = new_sample_rate_uint;
    mpp_pthread_mutex_unlock(&cd->lock);

    return newval;
}

static VALUE collector_get_max_heap_samples(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    mpp_pthread_mutex_lock(&cd->lock);
    int64_t v = cd->max_heap_samples;
    mpp_pthread_mutex_unlock(&cd->lock);
    return LONG2NUM(v);
}

static VALUE collector_set_max_heap_samples(VALUE self, VALUE newval) {
    struct collector_cdata *cd = collector_cdata_get(self);
    int64_t v = NUM2LONG(newval);
    mpp_pthread_mutex_lock(&cd->lock);
    cd->max_heap_samples = v;
    mpp_pthread_mutex_unlock(&cd->lock);
    return newval;
}

static void collector_mark_sample_as_freed(struct collector_cdata *cd, VALUE freed_obj) {
    struct mpp_sample *sample;
    if (st_delete(cd->heap_samples, (st_data_t *)&freed_obj, (st_data_t *)&sample)) {
        // We deleted it out of live objects; free the sample
        sample_refcount_dec(cd, sample);
        sample->freed_by_ruby = true;
        cd->heap_samples_count--;
    }
}

struct newobj_impl_args {
    struct mpp_rb_backtrace *bt;
};

// Collects all the parts of collector_tphook_newobj that could throw.
static VALUE collector_tphook_newobj_protected(VALUE args_as_uintptr) {
    struct newobj_impl_args *args = (struct newobj_impl_args*)args_as_uintptr;
    mpp_rb_backtrace_capture(&args->bt);
    return Qnil;
}

static void collector_tphook_newobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE newobj = rb_tracearg_object(tparg);

    // These need to be initialized up here so that they're properly zero-initialized if
    // we goto out before they're otherwise filled.
    struct newobj_impl_args args = { .bt = NULL };
    VALUE original_errinfo = Qundef;
    int jump_tag = 0;

    mpp_pthread_mutex_lock(&cd->lock);

    // For every new object that is created, we _MUST_ check if there is already another VALUE with the same,
    // well, value, in our heap profiling table of live objects. This is because Ruby reserves the right to
    // simply free some kinds of internal objects (such as T_IMEMOs) by simply setting the flags value on it
    // to zero, without invoking the GC and without calling any kind of hook. So, we need to detect when such
    // an object is freed and then the RVALUE is re-used for a new object to track it appropriately.
    collector_mark_sample_as_freed(cd, newobj);

    // Skip the rest of this method if we're not sampling.
    if (mpp_rand() > cd->u32_sample_rate) {
       goto out;
    }
    // Make sure there's enough space in our buffer
    if (cd->heap_samples_count >= cd->max_heap_samples) {
        cd->dropped_samples_heap_bufsize++;
        goto out;
    }

    // OK - run our backtrace collection in here under rb_protect now so that it cannot longjmp out
    original_errinfo = rb_errinfo();
    rb_protect(collector_tphook_newobj_protected, (VALUE)&args, &jump_tag);
    if (jump_tag) goto out;

    // This looks super redundant, _BUT_ there is a narrow possibility that some of the code we invoke
    // inside the rb_protect actually does RVALUE allocations itself, and so recursively runs this hook
    // (which will work, because the &cd->lock mutex is recursive). So, we need to actually check
    // our buffer sizes _again_.
    if (cd->heap_samples_count >= cd->max_heap_samples) {
        cd->dropped_samples_heap_bufsize++;
        goto out;
    }

    // OK, now it's time to add to our sample buffer.
    struct mpp_sample *sample = mpp_xmalloc(sizeof(struct mpp_sample));
    sample->bt = args.bt;
    sample->allocated_value_weak = newobj;
    sample->processed_into_functab = false;
    sample->freed_by_ruby = false;
    sample->refcount = 1;

    // insert into live sample map
    st_insert(cd->heap_samples, newobj, (st_data_t)sample);
    cd->heap_samples_count++;

    // Clear args.bt so it doesn't get free'd below.
    args.bt = NULL;

out:
    // If this wasn't cleared, we need to free it.
    if (args.bt) mpp_rb_backtrace_destroy(args.bt);
    // If there was an exception, ignore it and restore the original errinfo.
    if (jump_tag && original_errinfo != Qundef) rb_set_errinfo(original_errinfo);

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
    collector_mark_sample_as_freed(cd, freed_obj);

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

    // Don't needlessly double-initialize everything
    if (cd->heap_samples_count > 0) {
        collector_cdata_gc_free_heap_samples(cd);
        cd->heap_samples = st_init_numtable();
        cd->heap_samples_count = 0;
    }
    cd->is_tracing = true;
    cd->dropped_samples_heap_bufsize = 0;

    // Now do the things that might raise
    rb_protect(collector_start_protected, self, &jump_tag);

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

static int collector_heap_samples_each_calc_size(st_data_t key, st_data_t val, st_data_t arg) {
    struct mpp_sample *sample = (struct mpp_sample *)val;
    struct collector_cdata *cd = (struct collector_cdata *)arg;
    MPP_ASSERT_MSG(sample->allocated_value_weak != Qundef, "undef was in heap sample map");

    // Check that the sample is, in fact, still live. This can happen if an object is freed internally
    // by Ruby without firing our freeobj hook (which Ruby is allowed to do for some kinds of objects).
    // In that case, flags will be zero and so type will be T_NONE.
    // Note that if an object is freed and then the slot is subsequently re-used for a different object,
    // our newobj hook will fire in that case and do this too. So this method captures the sequence
    // allocate -> free -> flush, but the newobj hook handles the allocate -> free -> reuse -> flush case.
    if (RB_TYPE_P(sample->allocated_value_weak, T_NONE)) {
        sample->allocated_value_weak = Qundef;
        sample_refcount_dec(cd, sample);
        cd->heap_samples_count--;
        return ST_DELETE;
    }
    MPP_ASSERT_MSG(!sample->freed_by_ruby, "freed by ruby in collector_heap_samples_each_calc_size");
    sample->current_size = rb_obj_memsize_of(sample->allocated_value_weak);
    return ST_CONTINUE;
}

struct collector_heap_samples_each_add_args {
    struct mpp_pprof_serctx *serctx;
    char *errbuf;
    size_t errbuf_len;
    int r;
};

static int collector_heap_samples_each_add(st_data_t key, st_data_t val, st_data_t arg) {
    struct mpp_sample *sample = (struct mpp_sample *)val;
    struct collector_heap_samples_each_add_args *args = (struct collector_heap_samples_each_add_args *)arg;

    int r = mpp_pprof_serctx_add_sample(args->serctx, sample, args->errbuf, args->errbuf_len);
    if (r != 0) {
        args->r = r;
        return ST_STOP;
    }
    return ST_CONTINUE;
}

struct collector_flush_prepresult_args {
    const char *pprofbuf;
    size_t pprofbuf_len;
    VALUE cProfileData;

    // Extra struff that needs to go onto the struct.
    int64_t heap_samples_count;
    int64_t dropped_samples_heap_bufsize;
};

static VALUE collector_flush_protected_heap_sample_size(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    st_foreach(cd->heap_samples, collector_heap_samples_each_calc_size, (st_data_t)cd);
    return Qnil;
}

static VALUE collector_flush_prepresult(VALUE vargs) {
    struct collector_flush_prepresult_args *args =
        (struct collector_flush_prepresult_args *)vargs;

    VALUE pprof_data = rb_str_new(args->pprofbuf, args->pprofbuf_len);
    VALUE profile_data = rb_class_new_instance(0, NULL, args->cProfileData);
    rb_funcall(profile_data, rb_intern("pprof_data="), 1, pprof_data);
    rb_funcall(profile_data, rb_intern("heap_samples_count="), 1, LONG2NUM(args->heap_samples_count));
    rb_funcall(
        profile_data, rb_intern("dropped_samples_heap_bufsize="),
        1, LONG2NUM(args->dropped_samples_heap_bufsize)
    );
    return profile_data;
}

struct add_heap_samples_to_functab_args {
    struct collector_cdata *cd;
    struct mpp_sample **samples_copy;
    int samples_copy_count;
};

static VALUE collector_add_heap_samples_to_functab(VALUE vargs) {
    struct add_heap_samples_to_functab_args *args = (struct add_heap_samples_to_functab_args *)vargs;
    for (int i = 0; i < args->samples_copy_count; i++) {
        if (!args->samples_copy[i]->processed_into_functab) {
//            struct mpp_functab *functab = args->cd->function_table;
//            struct mpp_rb_backtrace *bt = args->samples_copy[i]->bt;
//
//            uint32_t frames_count = backtracie_bt_get_frames_count(bt->backtracie);
//            bt->frame_extras = mpp_xmalloc(frames_count * sizeof(struct mpp_rb_backtrace_frame_extra));
//            for (uint32_t i = 0; i < frames_count; i++) {
//                MPP_ASSERT_MSG(!args->samples_copy[i]->freed_by_ruby, "freed by ruby in collector_add_heap_samples_to_functab (1)");
//                VALUE frame = backtracie_bt_get_frame_value(bt->backtracie, i);
//                MPP_ASSERT_MSG(!args->samples_copy[i]->freed_by_ruby, "freed by ruby in collector_add_heap_samples_to_functab (2)");
//                uint64_t id = NUM2LONG(rb_obj_id(frame));
//                MPP_ASSERT_MSG(!args->samples_copy[i]->freed_by_ruby, "freed by ruby in collector_add_heap_samples_to_functab (3)");
//                VALUE name = backtracie_bt_get_frame_method_name(bt->backtracie, i);
//                MPP_ASSERT_MSG(!args->samples_copy[i]->freed_by_ruby, "freed by ruby in collector_add_heap_samples_to_functab (4)");
//                VALUE file_name = backtracie_bt_get_frame_file_name(bt->backtracie, i);
//                MPP_ASSERT_MSG(!args->samples_copy[i]->freed_by_ruby, "freed by ruby in collector_add_heap_samples_to_functab (5)");
//                VALUE line_number = backtracie_bt_get_frame_line_number(bt->backtracie, i);
//                MPP_ASSERT_MSG(!args->samples_copy[i]->freed_by_ruby, "freed by ruby in collector_add_heap_samples_to_functab (6)");
//                VALUE fn_line_number = rb_profile_frame_first_lineno(frame);
//
//                mpp_functab_add(functab, id, name, file_name, fn_line_number);
//
//                bt->frame_extras[i].function_id = id;
//                bt->frame_extras[i].line_number = RTEST(line_number) ? NUM2LONG(line_number) : 0;
//            }

            mpp_functab_add_all_frames(args->cd->function_table, args->samples_copy[i]->bt);

            args->samples_copy[i]->processed_into_functab = 1;
        }
    }
    return Qnil;
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
    struct collector_flush_prepresult_args prepresult_args;
    int lock_held = 0;
    struct mpp_sample **samples_copy = NULL;
    int samples_copy_count = 0;

    // Whilst under the GVL, we need to get the collector lock
    mpp_pthread_mutex_lock(&cd->lock);
    lock_held = 1;

    // Get the current size for everything in the live allocations table.
    // TODO: this really needs to go into the for loop below, but i don't think it does
    // any allocations so it _probably_ won't crash.
    rb_protect(collector_flush_protected_heap_sample_size, self, &jump_tag);
    if (jump_tag) goto out;

    samples_copy_count = cd->heap_samples_count;
    samples_copy = mpp_xmalloc(samples_copy_count * sizeof(struct mpp_sample *));
    st_values(cd->heap_samples, (st_data_t *)samples_copy, samples_copy_count);

    for (int i = 0; i < samples_copy_count; i++) {
        sample_refcount_inc(samples_copy[i]);
    }

    struct add_heap_samples_to_functab_args hsargs;
    hsargs.cd = cd;
    hsargs.samples_copy = samples_copy;
    hsargs.samples_copy_count = samples_copy_count;
    rb_protect(collector_add_heap_samples_to_functab, (VALUE)&hsargs, &jump_tag);
    if (jump_tag) goto out;


    prepresult_args.heap_samples_count = cd->heap_samples_count;
    prepresult_args.dropped_samples_heap_bufsize = cd->dropped_samples_heap_bufsize;
    cd->dropped_samples_heap_bufsize = 0;
    serctx = mpp_pprof_serctx_new(cd->string_table, cd->function_table, errbuf, sizeof(errbuf));
    if (!serctx) {
        goto out;
    }

    // Now that we have the samples (and have processed the stringtab) we can
    // yield the lock.
    mpp_pthread_mutex_unlock(&cd->lock);
    lock_held = 0;

    // TODO - I think we can release the GVL at this point.

    // Add the heap samples
    struct collector_heap_samples_each_add_args heap_add_args;
    heap_add_args.serctx = serctx;
    heap_add_args.errbuf = errbuf;
    heap_add_args.errbuf_len = sizeof(errbuf);
    heap_add_args.r = 0;
    st_foreach(cd->heap_samples, collector_heap_samples_each_add, (st_data_t)&heap_add_args);
    if (heap_add_args.r != 0) goto out;

    r = mpp_pprof_serctx_serialize(serctx, &buf_out, &buflen_out, errbuf, sizeof(errbuf));
    if ( r == -1) {
        goto out;
    }

    // Annoyingly, since rb_str_new could (in theory) throw, we have to rb_protect the whole construction
    // of our return value to ensure we don't leak serctx.
    prepresult_args.pprofbuf = buf_out;
    prepresult_args.pprofbuf_len = buflen_out;
    prepresult_args.cProfileData = cd->cProfileData;
    retval = rb_protect(collector_flush_prepresult, (VALUE)&prepresult_args, &jump_tag);

    // Do cleanup here now.
out:
    if (serctx) mpp_pprof_serctx_destroy(serctx);
    for (int i = 0; i < samples_copy_count; i++) {
        sample_refcount_dec(cd, samples_copy[i]);
    }
    if (lock_held) mpp_pthread_mutex_unlock(&cd->lock);

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

static VALUE collector_profile(VALUE self) {
    rb_need_block();

    rb_funcall(self, rb_intern("start!"), 0);
    rb_yield_values(0);
    VALUE profile_output = rb_funcall(self, rb_intern("flush"), 0);
    rb_funcall(self, rb_intern("stop!"), 0);

    return profile_output;
}

static VALUE collector_live_heap_samples_count(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    mpp_pthread_mutex_lock(&cd->lock);
    int64_t counter = cd->heap_samples_count;
    mpp_pthread_mutex_unlock(&cd->lock);
    return LONG2NUM(counter);
}

static int mpp_collector_atfork_lock_el(st_data_t key, st_data_t value, st_data_t arg) {
    struct collector_cdata *cd = (struct collector_cdata *)key;
    mpp_pthread_mutex_lock(&cd->lock);
    return ST_CONTINUE;
}

static int mpp_collector_atfork_unlock_el(st_data_t key, st_data_t value, st_data_t arg) {
    struct collector_cdata *cd = (struct collector_cdata *)key;
    mpp_pthread_mutex_unlock(&cd->lock);
    return ST_CONTINUE;
}

static int mpp_collector_atfork_replace_el(st_data_t key, st_data_t value, st_data_t arg) {
    struct collector_cdata *cd = (struct collector_cdata *)key;

    // In the parent process, we simply release the mutexes, but in the child process, we have
    // to _RECREATE_ them. This is because they're recursive mutexes, and must hold some kind of
    // thread ID in them somehow; unlocking them post-fork simply doesn't work it seems.
    // It's safe to re-create the mutex at this point, because no other thread can possibly be
    // holding it since we took it pre-fork
    mpp_pthread_mutex_destroy(&cd->lock);
    pthread_mutexattr_t mutex_attr;
    mpp_pthread_mutexattr_init(&mutex_attr);
    mpp_pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    memset(&cd->lock, 0, sizeof(cd->lock));
    mpp_pthread_mutex_init(&cd->lock, &mutex_attr);
    mpp_pthread_mutexattr_destroy(&mutex_attr);

    return ST_CONTINUE;
}

static void mpp_collector_atfork_prepare() {
    mpp_pthread_mutex_lock(&global_collectors_lock);
    st_foreach(global_collectors, mpp_collector_atfork_lock_el, 0);
}

static void mpp_collector_atfork_release_parent() {
    st_foreach(global_collectors, mpp_collector_atfork_unlock_el, 0);
    mpp_pthread_mutex_unlock(&global_collectors_lock);
}

static void mpp_collector_atfork_release_child() {
    st_foreach(global_collectors, mpp_collector_atfork_replace_el, 0);
    mpp_pthread_mutex_unlock(&global_collectors_lock);
}


void mpp_setup_collector_class() {
    VALUE mMemprofilerPprof = rb_const_get(rb_cObject, rb_intern("MemprofilerPprof"));
    VALUE cCollector = rb_define_class_under(mMemprofilerPprof, "Collector", rb_cObject);
    rb_define_alloc_func(cCollector, collector_alloc);


    rb_define_method(cCollector, "initialize", collector_initialize, -1);
    rb_define_method(cCollector, "sample_rate", collector_get_sample_rate, 0);
    rb_define_method(cCollector, "sample_rate=", collector_set_sample_rate, 1);
    rb_define_method(cCollector, "max_heap_samples", collector_get_max_heap_samples, 0);
    rb_define_method(cCollector, "max_heap_samples=", collector_set_max_heap_samples, 1);
    rb_define_method(cCollector, "running?", collector_is_running, 0);
    rb_define_method(cCollector, "start!", collector_start, 0);
    rb_define_method(cCollector, "stop!", collector_stop, 0);
    rb_define_method(cCollector, "flush", collector_flush, 0);
    rb_define_method(cCollector, "profile", collector_profile, 0);
    rb_define_method(cCollector, "live_heap_samples_count", collector_live_heap_samples_count, 0);

    global_collectors = st_init_numtable();
    mpp_pthread_mutex_init(&global_collectors_lock, NULL);
    mpp_pthread_atfork(mpp_collector_atfork_prepare, mpp_collector_atfork_release_parent, mpp_collector_atfork_release_child);
}
