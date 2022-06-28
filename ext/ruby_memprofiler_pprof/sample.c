#include <ruby.h>
#include "ruby_memprofiler_pprof.h"

struct mpp_sample *mpp_sample_new(unsigned long frames_capacity) {
    struct mpp_sample *sample = mpp_xmalloc(
        sizeof(struct mpp_sample) + frames_capacity * sizeof(struct mpp_backtrace_frame)
    );
    sample->frames_capacity = frames_capacity;
    sample->frames_count = 0;
    sample->refcount = 1;
    sample->allocated_value_weak = Qundef;
    return sample;
}

// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample) {
    return sizeof(struct mpp_sample) +
        sample->frames_capacity * sizeof(struct mpp_backtrace_frame);
}

// Increments the refcount on sample
unsigned long mpp_sample_refcount_inc(struct mpp_sample *sample) {
    MPP_ASSERT_MSG(sample->refcount, "mpp_sample_refcount_inc: tried to increment zero refcount!");
    sample->refcount++;
    return sample->refcount;
}

// Decrements the refcount on sample, freeing its resources if it drops to zero.
unsigned long mpp_sample_refcount_dec(
    struct mpp_sample *sample, struct mpp_strtab *strtab
) {
    MPP_ASSERT_MSG(sample->refcount, "mpp_sample_refcount_dec: tried to decrement zero refcount!");
    sample->refcount--;
    if (sample->refcount) return sample->refcount;

    // We need to free the sample.
    
    // Unreference what was previously interned into the strtab
    for (size_t i = 0; i < sample->frames_count; i++) {
        mpp_strtab_release(strtab, sample->frames[i].function_name, sample->frames[i].function_name_len);
        mpp_strtab_release(strtab, sample->frames[i].file_name, sample->frames[i].file_name_len);
    }

    mpp_free(sample);
    return 0;
}
