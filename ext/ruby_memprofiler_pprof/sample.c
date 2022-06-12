#include <ruby.h>
#include <backtracie.h>
#include "ruby_memprofiler_pprof.h"

struct sample_process_protected_ctx {
    struct mpp_sample *sample;
    struct mpp_functab *functab;
    struct mpp_sample_bt_processed *bt_processed;
    uint32_t frame_count;
};
VALUE sample_process_protected(VALUE ctxarg);

struct mpp_sample *mpp_sample_new() {
    struct mpp_sample *sample = mpp_xmalloc(sizeof(struct mpp_sample));
    sample->flags = 0;
    sample->refcount = 1;
    sample->allocated_value_weak = Qundef;
    sample->raw_backtrace = NULL;
    return sample;
}

void mpp_sample_gc_mark(struct mpp_sample *sample) {
    if (sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) {
        // "processed" sample - nothing to mark!
    } else {
        // "raw" sample - need to mark the underlying backtracie stuff.
        if (sample->raw_backtrace) {
#ifdef HAVE_RB_GC_MARK_MOVABLE
            backtracie_bt_gc_mark_moveable(sample->raw_backtrace);
#else
            backtracie_bt_gc_mark(sample->raw_backtrace);
#endif
        }

    }
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
void mpp_sample_gc_compact(struct mpp_sample *sample) {
    if (sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) {
        // "processed" sample - nothing to move!
    } else {
        // "raw" sample - need to move the backtracie stuff
        if (sample->raw_backtrace) {
            backtracie_bt_gc_compact(sample->raw_backtrace);
        }
    }
}
#endif

size_t mpp_sample_memsize(struct mpp_sample *sample) {
    size_t sz = sizeof(struct mpp_sample);
    if (sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) {
        // Need to measure the size of the "processed" backtrace
        if (sample->processed_backtrace) {
            sz += sizeof(struct mpp_sample_bt_processed);
            sz += sample->processed_backtrace->num_frames * sizeof(struct mpp_sample_bt_processed_frame);
        }
    } else {
        // Need to measure the size of the "raw" backtrace
        if (sample->raw_backtrace) {
            sz += backtracie_bt_memsize(sample->raw_backtrace);
        }
    }
    return sz;
}

uint8_t mpp_sample_refcount_inc(struct mpp_sample *sample) {
    MPP_ASSERT_MSG(sample->refcount, "mpp_sample_refcount_inc: tried to increment zero refcount!");
    sample->refcount++;
    return sample->refcount;
}

uint8_t mpp_sample_refcount_dec(struct mpp_sample *sample, struct mpp_functab *functab) {
    MPP_ASSERT_MSG(sample->refcount, "mpp_sample_refcount_dec: tried to decrement zero refcount!");
    sample->refcount--;
    if (!sample->refcount) {
        // Free the sample.
        if (sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED) {
            if (sample->processed_backtrace) {
                // Unreference what was interned into the functab
                for (size_t i = 0; i < sample->processed_backtrace->num_frames; i++) {
                    mpp_functab_deref(functab, sample->processed_backtrace->frames[i].function_id);
                }
                mpp_free(sample->processed_backtrace);
            }
        } else {
            // Haven't processed the sample into the functab; just release the raw backtracie.
            if (sample->raw_backtrace) {
                mpp_free(sample->raw_backtrace);
            }
        }
        mpp_free(sample);
        return 0;
    }
    return sample->refcount;
}

void mpp_sample_process(struct mpp_sample *sample, struct mpp_functab *functab) {
    MPP_ASSERT_MSG(!(sample->flags & MPP_SAMPLE_FLAGS_BT_PROCESSED), "mpp_sample_process called on already processed sample");
    MPP_ASSERT_MSG(sample->raw_backtrace, "mpp_sample_process called on sample with no backtrace");
    MPP_ASSERT_MSG(sample->refcount, "mpp_sample_process called with zero refcount");

    struct sample_process_protected_ctx ctx;
    ctx.sample = sample;
    ctx.functab = functab;
    ctx.frame_count = backtracie_bt_get_frames_count(sample->raw_backtrace);
    ctx.bt_processed = (struct mpp_sample_bt_processed *) mpp_xmalloc(
        sizeof(struct mpp_sample_bt_processed) + ctx.frame_count * sizeof(struct mpp_sample_bt_processed_frame)
    );
    ctx.bt_processed->num_frames = 0; // This will be incremented by sample_process_protected

    int jump_tag = 0;
    rb_protect(sample_process_protected, (VALUE)&ctx, &jump_tag);
    if (jump_tag) {
        // If an exception was thrown somewhere along the line, we need to free anything allocated by
        // the frames that _did_ work. Basically, do sample_process_protected but in reverse.
        for (size_t i = 0; i < ctx.bt_processed->num_frames; i++) {
            mpp_functab_deref(functab, ctx.bt_processed->frames[i].function_id);
        }
        mpp_free(ctx.bt_processed);
        rb_jump_tag(jump_tag);
    }

    // It worked, flip the sample into "processed" mode.
    mpp_free(sample->raw_backtrace);
    sample->raw_backtrace = NULL;
    sample->flags |= MPP_SAMPLE_FLAGS_BT_PROCESSED;
    sample->processed_backtrace = ctx.bt_processed;
}

VALUE sample_process_protected(VALUE ctxarg) {
    struct sample_process_protected_ctx *ctx = (struct sample_process_protected_ctx *)ctxarg;

    for (uint32_t i = 0; i < ctx->frame_count; i++) {
        // Extract out the names & other values from the backtrace.
        VALUE cme_or_iseq = backtracie_bt_get_frame_value(ctx->sample->raw_backtrace, i);
        VALUE function_name = backtracie_bt_get_frame_method_name(ctx->sample->raw_backtrace, i);
        VALUE file_name = backtracie_bt_get_frame_file_name(ctx->sample->raw_backtrace, i);
        unsigned long line_number = backtracie_bt_get_frame_line_number(ctx->sample->raw_backtrace, i);

        unsigned long function_id = mpp_functab_add_by_value(ctx->functab, cme_or_iseq, function_name, file_name);

        ctx->bt_processed->frames[i].function_id = function_id;
        ctx->bt_processed->frames[i].line_number = line_number;
        ctx->bt_processed->num_frames++;
    }
    return Qnil;
}

void mpp_sample_mark_value_freed(struct mpp_sample *sample) {
    MPP_ASSERT_MSG(!(sample->flags & MPP_SAMPLE_FLAGS_VALUE_FREED), "sample marked as freed twice");
    sample->flags |= MPP_SAMPLE_FLAGS_VALUE_FREED;
}
