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
    // Same thing, but for function locations
    struct mpp_functab *function_table;
};

static struct collector_cdata *collector_cdata_get(VALUE self);
static VALUE collector_alloc(VALUE klass);
static VALUE collector_initialize(int argc, VALUE *argv, VALUE self);
static void collector_cdata_gc_mark(void *ptr);
static int collector_gc_mark_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg);
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
static VALUE collector_flush(VALUE self);
static VALUE flush_process_samples(VALUE ctxarg);
struct flush_prepresult_ctx {
    struct collector_cdata *cd;

    // The pprof data
    const char *pprof_outbuf;
    size_t pprof_outbuf_len;

    // Extra struff that needs to go onto the struct.
    size_t heap_samples_count;
    size_t dropped_samples_heap_bufsize;

    // Output
    VALUE retval;
};
static VALUE flush_prepresult(VALUE ctxarg);
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
    rb_define_method(cCollector, "flush", collector_flush, 0);
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

    cd->u32_sample_rate = 0;
    cd->is_tracing = false;
    cd->heap_samples = NULL;
    cd->heap_samples_count = 0;
    cd->max_heap_samples = 0;
    cd->heap_samples_flush_copy = NULL;
    cd->heap_samples_flush_copy_count = 0;
    cd->dropped_samples_heap_bufsize = 0;
    cd->string_table = NULL;
    cd->function_table = NULL;
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
    cd->function_table = mpp_functab_new(cd->string_table);
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

    // Mark all the iseqs/CME's we have stored in the heap sample map
    st_foreach(cd->heap_samples, collector_gc_mark_each_heap_sample, 0);

    // And, if we're in the middle of a call to #flush and have a copy, mark those too.
    if (cd->heap_samples_flush_copy) {
        for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
            mpp_sample_gc_mark(cd->heap_samples_flush_copy[i]);
        }
    }
}

static int collector_gc_mark_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct mpp_sample *sample = (struct mpp_sample *)value;
    mpp_sample_gc_mark(sample);
    return ST_CONTINUE;
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
    if (cd->function_table) {
        mpp_functab_destroy(cd->function_table);
    }
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
    mpp_sample_refcount_dec(sample, cd->function_table);
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
    if (cd->function_table) {
        sz += mpp_functab_memsize(cd->function_table);
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
    st_foreach(cd->heap_samples, collector_compact_each_heap_sample, (st_data_t)cd);

    // If we're in the middle of a call to #flush and have a copy, move those too.
    if (cd->heap_samples_flush_copy) {
        for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
            mpp_sample_gc_compact(cd->heap_samples_flush_copy[i]);
        }
    }
}

static int collector_compact_each_heap_sample(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct collector_cdata *cd = (struct collector_cdata *)ctxarg;
    struct mpp_sample *sample = (struct mpp_sample *)value;

    // Handle compaction of the heap samples themselves
    mpp_sample_gc_compact(sample);

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
        mpp_sample_mark_value_freed(sample);
        mpp_sample_refcount_dec(sample, cd->function_table);
        cd->heap_samples_count--;
    }
}

static void collector_tphook_newobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE newobj = rb_tracearg_object(tparg);

#ifdef HAVE_WORKING_RB_GC_FORCE_RECYCLE
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
       return;
    }
    // Make sure there's enough space in our buffer
    if (cd->heap_samples_count >= cd->max_heap_samples) {
        cd->dropped_samples_heap_bufsize++;
        return;
    }

    // OK, now it's time to add to our sample buffer.
    struct mpp_sample *sample = mpp_sample_new();
    sample->allocated_value_weak = newobj;

    size_t required_bufsize = 0;
    sample->raw_backtrace = backtracie_bt_capture();

    // insert into live sample map
    int alread_existed = st_insert(cd->heap_samples, newobj, (st_data_t)sample);
    MPP_ASSERT_MSG(alread_existed == 0, "st_insert did an update in the newobj hook");
    cd->heap_samples_count++;
}

static void collector_tphook_freeobj(VALUE tpval, void *data) {
    struct collector_cdata *cd = (struct collector_cdata *)data;

    // Definitely do _NOT_ try and run any Ruby code in here. Any allocation will crash
    // the process.
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    VALUE freed_obj = rb_tracearg_object(tparg);
    collector_mark_sample_value_as_freed(cd, freed_obj);
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

static VALUE collector_flush(VALUE self) {
    struct collector_cdata *cd = collector_cdata_get(self);

    // Normally not a fan pointlessly declaring all vars at the top of a function, but in this
    // case we need it, to ensure they're all appropriately initialized for the out: block
    // in case we have to goto out.
    int jump_tag = 0;
    size_t *heap_samples_sizes = NULL;
    char errbuf[256];
    VALUE retval = Qundef;
    struct mpp_pprof_serctx *serctx = NULL;
    int r = 0;

    if (cd->heap_samples_flush_copy) {
        ruby_snprintf(errbuf, sizeof(errbuf), "concurrent calls to #flush are not valid");
        goto out;
    }

    size_t dropped_samples_bufsize = cd->dropped_samples_heap_bufsize;
    cd->dropped_samples_heap_bufsize = 0;

    // Copy all the samples, and take a reference to them.
    cd->heap_samples_flush_copy_count = cd->heap_samples_count;
    cd->heap_samples_flush_copy = mpp_xmalloc(cd->heap_samples_flush_copy_count * sizeof(struct mpp_sample *));
    st_values(cd->heap_samples, (st_data_t *)cd->heap_samples_flush_copy, cd->heap_samples_flush_copy_count);
    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        mpp_sample_refcount_inc(cd->heap_samples_flush_copy[i]);
    }

    // Process them into "processed" samples, if needed.
    rb_protect(flush_process_samples, (VALUE)cd, &jump_tag);
    if (jump_tag) goto out;

    // Capture their size, if possible.
    heap_samples_sizes = mpp_xmalloc(cd->heap_samples_flush_copy_count * sizeof(size_t));
    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        struct mpp_sample *sample = cd->heap_samples_flush_copy[i];
        if ((sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) && !(sample->flags & MPP_SAMPLE_FLAGS_VALUE_FREED)) {
            heap_samples_sizes[i] = rb_obj_memsize_of(sample->allocated_value_weak);
        }
    }

    // Begin setting up pprof serialisation.
    serctx = mpp_pprof_serctx_new(cd->string_table, cd->function_table, errbuf, sizeof(errbuf));
    if (!serctx) {
        goto out;
    }

    // Add each sample from our internal copy.
    size_t actual_sample_count = 0;
    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        struct mpp_sample *sample = cd->heap_samples_flush_copy[i];
        if ((sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) && !(sample->flags & MPP_SAMPLE_FLAGS_VALUE_FREED)) {
            r = mpp_pprof_serctx_add_sample(serctx, sample, heap_samples_sizes[i], errbuf, sizeof(errbuf));
            if (r == -1) {
                goto out;
            }
            actual_sample_count++;
        }
    }

    // And now serialise to pprof.
    // The outpout buffer actually is allocated on the upb arena, so we don't need to free it; they will be freed implicitly
    // when the serialisation context is destroyed.
    char *pprof_outbuf;
    size_t pprof_outbuf_len;
    r = mpp_pprof_serctx_serialize(serctx, &pprof_outbuf, &pprof_outbuf_len, errbuf, sizeof(errbuf));
    if (r == -1) {
        goto out;
    }

    // Annoyingly, since rb_str_new could (in theory) throw, we have to rb_protect the whole construction
    // of our return value to ensure we don't leak anything.
    struct flush_prepresult_ctx prctx = {
        .cd = cd,
        .pprof_outbuf = pprof_outbuf,
        .pprof_outbuf_len = pprof_outbuf_len,
        .heap_samples_count = actual_sample_count,
        .dropped_samples_heap_bufsize = dropped_samples_bufsize,
        .retval = Qnil,
    };
    rb_protect(flush_prepresult, (VALUE)&prctx, &jump_tag);
    if (jump_tag) {
        goto out;
    }
    // OK! we have a Ruby object representing our profile data.
    retval = prctx.retval;

 out:
    if (serctx) mpp_pprof_serctx_destroy(serctx);
    if (cd->heap_samples_flush_copy) {
        for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
            mpp_sample_refcount_dec(cd->heap_samples_flush_copy[i], cd->function_table);
        }
        mpp_free(cd->heap_samples_flush_copy);
        cd->heap_samples_flush_copy = NULL;
        cd->heap_samples_flush_copy_count = 0;
    }
    if (heap_samples_sizes) {
        mpp_free(heap_samples_sizes);
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
}

static VALUE flush_process_samples(VALUE ctxarg) {
    struct collector_cdata *cd = (struct collector_cdata *)ctxarg;
    for (size_t i = 0; i < cd->heap_samples_flush_copy_count; i++) {
        struct mpp_sample *sample = cd->heap_samples_flush_copy[i];
        if (!(sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) && !(sample->flags & MPP_SAMPLE_FLAGS_VALUE_FREED)) {
            mpp_sample_process(sample, cd->function_table);
        }
    }
    return Qnil;
}

static VALUE flush_prepresult(VALUE ctxarg) {
    struct flush_prepresult_ctx *ctx = (struct flush_prepresult_ctx *)ctxarg;

    VALUE pprof_data = rb_str_new(ctx->pprof_outbuf, ctx->pprof_outbuf_len);
    VALUE profile_data = rb_class_new_instance(0, NULL, ctx->cd->cProfileData);
    rb_funcall(profile_data, rb_intern("pprof_data="), 1, pprof_data);
    rb_funcall(profile_data, rb_intern("heap_samples_count="), 1, SIZET2NUM(ctx->heap_samples_count));
    rb_funcall(
        profile_data, rb_intern("dropped_samples_heap_bufsize="),
        1, SIZET2NUM(ctx->dropped_samples_heap_bufsize)
    );

    ctx->retval = profile_data;
    return Qnil;
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
