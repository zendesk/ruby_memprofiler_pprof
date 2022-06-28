#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ruby.h>
#include <ruby/debug.h>
#include <ruby/thread.h>

#include "ruby_memprofiler_pprof.h"

struct collector_cdata {
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
    // If we're flushing, this contains the thread that's doing the flushing. This is used
    // to exclude allocations from that thread from heap profiling.
    VALUE flush_thread;

    // ======== Heap samples ========
    // A hash-table keying live VALUEs to their struct mpp_sample. This is _not_ cleared
    // when #flush is called; instead, elements are deleted when they are free'd. This is
    // used for building heap profiles.
    st_table *heap_samples;
    // Number of elements currently in the heap profile hash
    size_t heap_samples_count;
    // How big the sample table can grow
    size_t max_heap_samples;
    // This is a _copy_ of the heap samples, which is set during a collector flush and unset
    // afterwards. This is required because if a value is freed AFTER we start flushing the sample
    // and BEFORE we finish flushing it, we need to mark the VALUEs contained in the sample (e.g.
    // iseq's and cme's) if a GC happens in the meanwhile. This would normally be handled by marking
    // directly from the heap_samples hashtable, buit it will have been removed from there.
    struct mpp_sample **heap_samples_flush_copy;
    size_t heap_samples_flush_copy_count;

    // ======== Sample drop counters ========
    // Number of samples dropped for want of space in the heap allocation table.
    size_t dropped_samples_heap_bufsize;

    // String interning table used to keep constant pointers to every string; this saves memory
    // used in backtraces, and also helps us efficiently build up the pprof protobuf format (since that
    // _requires_ that strings are interned in a string table).
    struct mpp_strtab *string_table;
};

static struct collector_cdata *collector_cdata_get(VALUE self);
static VALUE collector_alloc(VALUE klass);
static VALUE collector_initialize(int argc, VALUE *argv, VALUE self);
static void collector_cdata_gc_mark(void *ptr);
static void collector_gc_free(void *ptr);
static void collector_gc_free_heap_samples(struct collector_cdata *cd);
static int collector_gc_free_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg);
static size_t collector_gc_memsize(const void *ptr);
static int collector_gc_memsize_each_heap_sample(st_data_t key, st_data_t value, st_data_t arg);
#ifdef HAVE_RB_GC_MARK_MOVABLE
static void collector_cdata_gc_compact(void *ptr);
static int collector_compact_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg);
#endif
static void collector_mark_sample_value_as_freed(struct collector_cdata *cd, VALUE freed_obj);
static void collector_tphook_newobj(VALUE tpval, void *data);
static void collector_tphook_freeobj(VALUE tpval, void *data);
static VALUE collector_start(VALUE self);
static VALUE collector_stop(VALUE self);
static VALUE collector_is_running(VALUE self);
static VALUE collector_flush(int argc, VALUE *argv, VALUE self);
struct flush_protected_ctx {
    struct collector_cdata *cd;
    struct mpp_pprof_serctx *serctx;
    bool yield_gvl;
    bool proactively_yield_gvl;
};
static VALUE flush_protected(VALUE ctxarg);
struct flush_nogvl_ctx {
    struct collector_cdata *cd;
    struct mpp_pprof_serctx *serctx;
    char *pprof_outbuf;
    size_t pprof_outbuf_len;
    char *errbuf;
    size_t sizeof_errbuf;
    int r;
    size_t actual_sample_count;
};
static void *flush_nogvl(void *ctx);
static void flush_nogvl_unblock(void *ctx);
static VALUE collector_profile(VALUE self);
static VALUE collector_live_heap_samples_count(VALUE self);
static VALUE collector_get_sample_rate(VALUE self);
static VALUE collector_set_sample_rate(VALUE self, VALUE newval);
static VALUE collector_get_max_heap_samples(VALUE self);
static VALUE collector_set_max_heap_samples(VALUE self, VALUE newval);

static const rb_data_type_t collector_cdata_type = {
    "collector_cdata",
    {
        collector_cdata_gc_mark, collector_gc_free, collector_gc_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
        collector_cdata_gc_compact,
#endif
        { 0 }, /* reserved */
    },
    /* parent, data, [ flags ] */
    NULL, NULL, 0
};

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
    rb_define_method(cCollector, "flush", collector_flush, -1);
    rb_define_method(cCollector, "profile", collector_profile, 0);
    rb_define_method(cCollector, "live_heap_samples_count", collector_live_heap_samples_count, 0);
}

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
    cd->flush_thread = Qnil;

    cd->u32_sample_rate = 0;
    cd->is_tracing = false;
    cd->heap_samples = NULL;
    cd->heap_samples_count = 0;
    cd->max_heap_samples = 0;
    cd->heap_samples_flush_copy = NULL;
    cd->heap_samples_flush_copy_count = 0;
    cd->dropped_samples_heap_bufsize = 0;
    cd->string_table = NULL;
    return v;
}

static VALUE collector_initialize(int argc, VALUE *argv, VALUE self) {
   struct collector_cdata *cd = collector_cdata_get(self);

    // Save constants
    cd->mMemprofilerPprof = rb_const_get(rb_cObject, rb_intern("MemprofilerPprof"));
    cd->cCollector = rb_const_get(cd->mMemprofilerPprof, rb_intern("Collector"));
    cd->cProfileData = rb_const_get(cd->mMemprofilerPprof, rb_intern("ProfileData"));

    // Argument parsing
    VALUE kwargs_hash = Qnil;
    rb_scan_args_kw(RB_SCAN_ARGS_LAST_HASH_KEYWORDS, argc, argv, "00:", &kwargs_hash);
    VALUE kwarg_values[2];
    ID kwarg_ids[2];
    kwarg_ids[0] = rb_intern("sample_rate");
    kwarg_ids[1] = rb_intern("max_heap_samples");
    rb_get_kwargs(kwargs_hash, kwarg_ids, 0, 2, kwarg_values);

    // Default values...
    if (kwarg_values[0] == Qundef) kwarg_values[0] = DBL2NUM(0.01);
    if (kwarg_values[1] == Qundef) kwarg_values[1] = LONG2NUM(50000);

    rb_funcall(self, rb_intern("sample_rate="), 1, kwarg_values[0]);
    rb_funcall(self, rb_intern("max_heap_samples="), 1, kwarg_values[1]);

    cd->string_table = mpp_strtab_new();
    cd->heap_samples = st_init_numtable();
    cd->heap_samples_count = 0;

    return Qnil;
}

static void collector_cdata_gc_mark(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    rb_gc_mark_movable(cd->newobj_trace);
    rb_gc_mark_movable(cd->freeobj_trace);
    rb_gc_mark_movable(cd->mMemprofilerPprof);
    rb_gc_mark_movable(cd->cCollector);
    rb_gc_mark_movable(cd->cProfileData);
    rb_gc_mark_movable(cd->flush_thread);
}

static void collector_gc_free(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    if (cd->is_tracing) {
        if (cd->newobj_trace) {
            rb_tracepoint_disable(cd->newobj_trace);
        }
        if (cd->freeobj_trace) {
            rb_tracepoint_disable(cd->freeobj_trace);
        }
    }

    collector_gc_free_heap_samples(cd);
    if (cd->string_table) {
        mpp_strtab_destroy(cd->string_table);
    }

    ruby_xfree(ptr);
}

static void collector_gc_free_heap_samples(struct collector_cdata *cd) {
    if (cd->heap_samples) {
        st_foreach(cd->heap_samples, collector_gc_free_each_heap_sample, (st_data_t)cd);
        st_free_table(cd->heap_samples);
    }
    cd->heap_samples = NULL;
}

static int collector_gc_free_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct mpp_sample *sample = (struct mpp_sample *)value;
    struct collector_cdata *cd = (struct collector_cdata *)ctxarg;
    mpp_sample_refcount_dec(sample, cd->string_table);
    return ST_CONTINUE;
}

static size_t collector_gc_memsize(const void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    size_t sz = sizeof(*cd);
    if (cd->heap_samples) {
        st_foreach(cd->heap_samples, collector_gc_memsize_each_heap_sample, (st_data_t)&sz);
        sz += st_memsize(cd->heap_samples);
    }
    if (cd->string_table) {
        sz += mpp_strtab_memsize(cd->string_table);
    }

    return sz;
}

static int collector_gc_memsize_each_heap_sample(st_data_t key, st_data_t value, st_data_t arg) {
    size_t *acc_ptr = (size_t *)arg;
    struct mpp_sample *sample = (struct mpp_sample *)value;
    *acc_ptr += mpp_sample_memsize(sample);
    return ST_CONTINUE;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
// Support VALUES we're tracking being moved away in Ruby 2.7+ with GC.compact
static void collector_cdata_gc_compact(void *ptr) {
    struct collector_cdata *cd = (struct collector_cdata *)ptr;
    cd->newobj_trace = rb_gc_location(cd->newobj_trace);
    cd->freeobj_trace = rb_gc_location(cd->freeobj_trace);
    cd->mMemprofilerPprof = rb_gc_location(cd->mMemprofilerPprof);
    cd->cCollector = rb_gc_location(cd->cCollector);
    cd->cProfileData = rb_gc_location(cd->cProfileData);
    cd->flush_thread = rb_gc_location(cd->flush_thread);

    // Keep track of allocated objects we sampled that might move.
    st_foreach(cd->heap_samples, collector_compact_each_heap_sample, (st_data_t)cd);
}

static int collector_compact_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct collector_cdata *cd = (struct collector_cdata *)ctxarg;
    struct mpp_sample *sample = (struct mpp_sample *)value;

    // Handle compaction of our weak reference to the heap sample.
    if (rb_gc_location(sample->allocated_value_weak) == sample->allocated_value_weak) {
        return ST_CONTINUE;
    } else {
        sample->allocated_value_weak = rb_gc_location(sample->allocated_value_weak);
        st_insert(cd->heap_samples, sample->allocated_value_weak, (st_data_t)sample);
        return ST_DELETE;
    }
}

#endif

static void collector_mark_sample_value_as_freed(struct collector_cdata *cd, VALUE freed_obj) {
    struct mpp_sample *sample;
    if (st_delete(cd->heap_samples, (st_data_t *)&freed_obj, (st_data_t *)&sample)) {
        // We deleted it out of live objects; free the sample
        sample->allocated_value_weak = Qundef;
        mpp_sample_refcount_dec(sample, cd->string_table);
        cd->heap_samples_count--;
    }
}

static void collector_tphook_newobj(VALUE tpval, void *data) {
    // If an object is created or freed during our newobj hook, Ruby refuses to recursively run
    // the newobj/freeobj hook! It's just silently skipped. Thus, we can wind up missing
    // some objects (not good), or we can wind up missing the fact that some objects
    // were freed (even worse!).
    // Thus, we have to make sure that
    //   1) No Ruby objects are created in this method,
    //   2) No Ruby objects are freed in this method,
    // We achieve 1 by, well, not creating any Ruby objects. 2 would happen if the GC runs; one
    // of the things that _can_ trigger the GC to run, unfortunately, is ruby_xmalloc() and
    // friends (used internally by backtracie, and by st_hash, and also by this gem's malloc
    // wrapper). So, we need to disable the GC, and re-enable it at the end of our hook.
    // The normal "disable the GC" function, rb_gc_disable(), doesn't quite do it, because _that_
    // calls rb_gc_rest() to finish off any in-progress collection! If we called that, we'd free
    // objects, and miss removing them from our sample map. So, instead, we twiddle the dont_gc
    // flag on the objspace directly with this compat wrapper.
    VALUE gc_was_already_disabled = mpp_rb_gc_disable_no_rest();

    rb_trace_arg_t *tparg;
    VALUE newobj;
    struct collector_cdata *cd = (struct collector_cdata *)data;

#ifdef HAVE_WORKING_RB_GC_FORCE_RECYCLE
    tparg = rb_tracearg_from_tracepoint(tpval);
    newobj = rb_tracearg_object(tparg);

    // Normally, any object allocated that calls the newobj hook will be freed during GC,
    // and the freeobj tracepoint will then be called. Thus, any object added to the heap sample
    // map will be removed before a different object in the same slot is creeated.
    // Unfortunately, the one place that _isn't_ true is if an object is freed manually with
    // rb_gc_force_recycle(). This is deprecated in Ruby >= 3.1, but before that the only time
    // we find out that such an object is freed is when a new object is created in the same
    // slot. Handle that by marking an existing object in the sample map as "free'd".
    collector_mark_sample_value_as_freed(cd, newobj);
#endif
    // Skip the rest of this method if we're not sampling.
    if (mpp_rand() > cd->u32_sample_rate) {
       goto out;
    }
    // Don't profile allocations that were caused by the flusher; these allocations are
    //     1) numerous,
    //     2) probably not of interest,
    //     3) guaranteed not to actually make it into a heap usage profile anyway, since
    //        they get freed at the end of the flushing routine.
    if (rb_thread_current() == cd->flush_thread) {
        goto out;
    }
    // Make sure there's enough space in our buffer
    if (cd->heap_samples_count >= cd->max_heap_samples) {
        cd->dropped_samples_heap_bufsize++;
        goto out;
    }

#ifndef HAVE_WORKING_RB_GC_FORCE_RECYCLE
    // If we don't need to do the "check every object to see if it was freed" check,
    // we can defer actually calling these functions until we've decided whether or not
    // to sample.
    tparg = rb_tracearg_from_tracepoint(tpval);
    newobj = rb_tracearg_object(tparg);
#endif

    // OK, now it's time to add to our sample buffer.
    VALUE this_thread = rb_thread_current();
    unsigned long max_frames = mpp_backtrace_frame_count(this_thread);
    struct mpp_sample *sample = mpp_sample_new(max_frames);
    sample->allocated_value_weak = newobj;
    bool more_frames = max_frames > 0;
    int frame_index = 0;
    while (more_frames) {
        char function_name_buffer[256];
        char file_name_buffer[256];

        struct mpp_backtrace_frame frame;
        mpp_strbuilder_init(&frame.file_name, file_name_buffer, sizeof(file_name_buffer));
        mpp_strbuilder_init(
            &frame.qualified_method_name, function_name_buffer, sizeof(function_name_buffer)
        );
        more_frames = mpp_capture_backtrace_frame(this_thread, frame_index, &frame);
        mpp_sample_add_frame(sample, cd->string_table, &frame);
        frame_index++;
    }

    // insert into live sample map
    int alread_existed = st_insert(cd->heap_samples, newobj, (st_data_t)sample);
    MPP_ASSERT_MSG(alread_existed == 0, "st_insert did an update in the newobj hook");
    cd->heap_samples_count++;

out:
    if (!RTEST(gc_was_already_disabled)) {
        rb_gc_enable();
    }
}


static void collector_tphook_freeobj(VALUE tpval, void *data) {
    // See discussion about disabling GC in newobj tracepoint, it all applies here too.
    VALUE gc_was_already_disabled = mpp_rb_gc_disable_no_rest();

    struct collector_cdata *cd = (struct collector_cdata *)data;

    // Definitely do _NOT_ try and run any Ruby code in here. Any allocation will crash
    // the process.
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE freed_obj = rb_tracearg_object(tparg);
    collector_mark_sample_value_as_freed(cd, freed_obj);

    if (!RTEST(gc_was_already_disabled)) {
        rb_gc_enable();
    }
}

static VALUE collector_start(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    if (cd->is_tracing) return Qnil;

    // Don't needlessly double-initialize everything
    if (cd->heap_samples_count > 0) {
        collector_gc_free_heap_samples(cd);
        cd->heap_samples = st_init_numtable();
        cd->heap_samples_count = 0;
    }
    cd->dropped_samples_heap_bufsize = 0;

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

    cd->is_tracing = true;
    return Qnil;
}

static VALUE collector_stop(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    if (!cd->is_tracing) return Qnil;
    rb_tracepoint_disable(cd->newobj_trace);
    rb_tracepoint_disable(cd->freeobj_trace);
    cd->is_tracing = false;
    // Don't clear any of our buffers - it's OK to access the profiling info after calling stop!
    return Qnil;
}


static VALUE collector_is_running(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    return cd->is_tracing ? Qtrue : Qfalse;
}

static VALUE collector_flush(int argc, VALUE *argv, VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    // kwarg handling
    VALUE kwargs_hash = Qnil;
    rb_scan_args_kw(RB_SCAN_ARGS_LAST_HASH_KEYWORDS, argc, argv, "00:", &kwargs_hash);
    VALUE kwarg_values[2];
    ID kwarg_ids[2];
    kwarg_ids[0] = rb_intern("yield_gvl");
    kwarg_ids[1] = rb_intern("proactively_yield_gvl");
    rb_get_kwargs(kwargs_hash, kwarg_ids, 0, 2, kwarg_values);

    bool yield_gvl = false;
    bool proactively_yield_gvl = false;

    if (kwarg_values[0] != Qundef) {
        yield_gvl = RTEST(kwarg_values[0]);
    }
    if (kwarg_values[1] != Qundef) {
        proactively_yield_gvl = RTEST(kwarg_values[1]);
    }

#define DO_PROACTIVE_GVL_YIELD(counter) do { \
        if ( \
            proactively_yield_gvl && \
            counter % 25 == 0 && \
            mpp_is_someone_else_waiting_for_gvl() \
        ) { \
            rb_thread_schedule(); \
        } \
    } while (0);

    struct flush_protected_ctx ctx;
    ctx.cd = cd;
    ctx.proactively_yield_gvl = proactively_yield_gvl;
    ctx.yield_gvl = yield_gvl;
    ctx.serctx = NULL;
    int jump_tag = 0;
    VALUE retval = rb_protect(flush_protected, (VALUE)&ctx, &jump_tag);

    if (ctx.serctx) mpp_pprof_serctx_destroy(ctx.serctx);
    if (cd->heap_samples_flush_copy) {
        for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
            DO_PROACTIVE_GVL_YIELD(i);
            mpp_sample_refcount_dec(cd->heap_samples_flush_copy[i], cd->string_table);
        }
        mpp_free(cd->heap_samples_flush_copy);
        cd->heap_samples_flush_copy = NULL;
        cd->heap_samples_flush_copy_count = 0;
    }
    cd->flush_thread = Qnil;

    // Now return-or-raise back to ruby.
    if (jump_tag) {
        rb_jump_tag(jump_tag);
    }
    return retval;
}

static VALUE flush_protected(VALUE ctxarg) {
    struct flush_protected_ctx *ctx = (struct flush_protected_ctx *)ctxarg;
    struct collector_cdata *cd = ctx->cd;
    bool proactively_yield_gvl = ctx->proactively_yield_gvl;

    if (cd->heap_samples_flush_copy) {
        rb_raise(rb_eRuntimeError, "ruby_memprofiler_pprof: concurrent calls to #flush are not valid");
    }
    cd->flush_thread = rb_thread_current();

    size_t dropped_samples_bufsize = cd->dropped_samples_heap_bufsize;
    cd->dropped_samples_heap_bufsize = 0;

    // Copy all the samples, and take a reference to them.
    cd->heap_samples_flush_copy_count = cd->heap_samples_count;
    cd->heap_samples_flush_copy = mpp_xmalloc(cd->heap_samples_flush_copy_count * sizeof(struct mpp_sample *));
    // The above line _could_ have triggered a GC, so now there might be less samples.
    if (cd->heap_samples_flush_copy_count > cd->heap_samples_count) {
        cd->heap_samples_flush_copy_count = cd->heap_samples_count;
    }
    // Thankfully, st_values doesn't do anything that could trigger a GC (or yield back to Ruby), so we don't
    // have to worry about what happens if something is deleted from the map while we're iterating it.
    st_values(cd->heap_samples, (st_data_t *)cd->heap_samples_flush_copy, cd->heap_samples_flush_copy_count);
    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        DO_PROACTIVE_GVL_YIELD(i);
        mpp_sample_refcount_inc(cd->heap_samples_flush_copy[i]);
    }

    DO_PROACTIVE_GVL_YIELD(0);


    // Capture their size, if possible.
    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        DO_PROACTIVE_GVL_YIELD(i);
        struct mpp_sample *sample = cd->heap_samples_flush_copy[i];
        if (mpp_is_value_still_validish(sample->allocated_value_weak)) {
            sample->allocated_value_objsize = mpp_rb_obj_memsize_of(sample->allocated_value_weak);
        } else {
            sample->allocated_value_weak = Qundef;
        }
    }
    DO_PROACTIVE_GVL_YIELD(0);

    // Begin setting up pprof serialisation.
    char errbuf[256];
    ctx->serctx = mpp_pprof_serctx_new(cd->string_table, errbuf, sizeof(errbuf));
    if (!ctx->serctx) {
        rb_raise(rb_eRuntimeError, "ruby_memprofiler_pprof: failed flushing samples: %s", errbuf);
    }
    struct mpp_pprof_serctx *serctx = ctx->serctx;
    DO_PROACTIVE_GVL_YIELD(0);

    struct flush_nogvl_ctx nogvl_ctx;
    nogvl_ctx.errbuf = errbuf;
    nogvl_ctx.sizeof_errbuf = sizeof(errbuf);
    nogvl_ctx.serctx = serctx;
    nogvl_ctx.cd = cd;
    nogvl_ctx.actual_sample_count = 0;
    
    if (ctx->yield_gvl) {
        rb_thread_call_without_gvl(flush_nogvl, &nogvl_ctx, flush_nogvl_unblock, &nogvl_ctx);
    } else {
        flush_nogvl(&nogvl_ctx);
    }

    if (nogvl_ctx.r == -1) {
        rb_raise(
            rb_eRuntimeError,
            "ruby_memprofiler_pprof: failed serialising samples: %s", nogvl_ctx.errbuf
        );
    }
    
    VALUE pprof_data = rb_str_new(nogvl_ctx.pprof_outbuf, nogvl_ctx.pprof_outbuf_len);
    VALUE profile_data = rb_class_new_instance(0, NULL, cd->cProfileData);
    rb_funcall(profile_data, rb_intern("pprof_data="), 1, pprof_data);
    rb_funcall(
        profile_data, rb_intern("heap_samples_count="), 1,
        SIZET2NUM(nogvl_ctx.actual_sample_count)
    );
    rb_funcall(
        profile_data, rb_intern("dropped_samples_heap_bufsize="),
        1, SIZET2NUM(dropped_samples_bufsize)
    );

    return profile_data;

#undef DO_PROACTIVE_GVL_YIELD
}

static void *flush_nogvl(void *ctxarg) {
    struct flush_nogvl_ctx *ctx = (struct flush_nogvl_ctx *)ctxarg;
    struct collector_cdata *cd = ctx->cd;

    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        struct mpp_sample *sample = cd->heap_samples_flush_copy[i];
        if (sample->allocated_value_weak != Qundef) {
            int r = mpp_pprof_serctx_add_sample(
                ctx->serctx, sample, ctx->errbuf, ctx->sizeof_errbuf
            );
            if (r == -1) {
                ctx->r = r;
                return NULL;
            }
            ctx->actual_sample_count++;
        }
    }

    ctx->r = mpp_pprof_serctx_serialize(
        ctx->serctx, &ctx->pprof_outbuf, &ctx->pprof_outbuf_len,
        ctx->errbuf, ctx->sizeof_errbuf
    );
    return NULL;
}

static void flush_nogvl_unblock(void *ctxarg) {
    struct flush_nogvl_ctx *ctx = (struct flush_nogvl_ctx *)ctx;
    uint8_t one = 1;
    __atomic_store(&ctx->serctx->interrupt, &one, __ATOMIC_SEQ_CST);
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
    return SIZET2NUM(cd->heap_samples_count);
}

static VALUE collector_get_sample_rate(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    return DBL2NUM(((double)cd->u32_sample_rate)/UINT32_MAX);
}

static VALUE collector_set_sample_rate(VALUE self, VALUE newval) {
    struct collector_cdata *cd = collector_cdata_get(self);
    // Convert the double sample rate (between 0 and 1) to a value between 0 and UINT32_MAX
    cd->u32_sample_rate = UINT32_MAX * NUM2DBL(newval);
    return newval;
}

static VALUE collector_get_max_heap_samples(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);
    return SIZET2NUM(cd->max_heap_samples);
}

static VALUE collector_set_max_heap_samples(VALUE self, VALUE newval) {
    struct collector_cdata *cd = collector_cdata_get(self);
    cd->max_heap_samples = NUM2SIZET(newval);
    return newval;
}
