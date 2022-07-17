#include <ruby.h>
#include "ruby_memprofiler_pprof.h"

struct mpp_sample *mpp_sample_new(unsigned long frames_capacity) {
    struct mpp_sample *sample = mpp_xmalloc(
        sizeof(struct mpp_sample) + frames_capacity * sizeof(struct mpp_backtrace_frame)
    );
    sample->frames_capacity = frames_capacity;
    sample->frames_count = 0;
    sample->allocated_value_weak = Qundef;
    return sample;
}

// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample) {
    return sizeof(struct mpp_sample) +
        sample->frames_capacity * sizeof(struct mpp_backtrace_frame);
}

// Free the sample, incl. releasing strings it interned.
void mpp_sample_free(struct mpp_sample *sample, struct mpp_strtab *strtab) {
    // Unreference what was previously interned into the strtab
    for (size_t i = 0; i < sample->frames_count; i++) {
        mpp_strtab_release(strtab, sample->frames[i].function_name, sample->frames[i].function_name_len);
        mpp_strtab_release(strtab, sample->frames[i].file_name, sample->frames[i].file_name_len);
    }

    mpp_free(sample);
}
