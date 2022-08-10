#include "ruby_memprofiler_pprof.h"
#include <backtracie.h>
#include <ruby.h>

// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample) {
  return sizeof(struct mpp_sample) + sample->frames_capacity * sizeof(raw_location);
}

// Free the sample, incl. releasing strings it interned.
void mpp_sample_free(struct mpp_sample *sample) { mpp_free(sample); }

struct mpp_sample *mpp_sample_capture(VALUE allocated_value_weak) {
  VALUE thread = rb_thread_current();
  int stack_size = backtracie_frame_count_for_thread(thread);
  struct mpp_sample *sample = mpp_xmalloc(sizeof(struct mpp_sample) + stack_size * sizeof(raw_location));
  sample->frames_capacity = stack_size;
  sample->frames_count = 0;
  sample->allocated_value_weak = allocated_value_weak;
  memset(sample->frames, 0, stack_size * sizeof(raw_location));

  for (int i = 0; i < stack_size; i++) {
    raw_location *frame = &sample->frames[sample->frames_count];
    bool is_valid = backtracie_capture_frame_for_thread(thread, i, frame);
    if (is_valid) {
      sample->frames_count++;
    }
  }

  return sample;
}

size_t mpp_sample_frame_function_name(struct mpp_sample *sample, int frame_index, char *outbuf, size_t outbuf_len) {
  return backtracie_frame_name_cstr(&sample->frames[frame_index], outbuf, outbuf_len);
}

size_t mpp_sample_frame_file_name(struct mpp_sample *sample, int frame_index, char *outbuf, size_t outbuf_len) {
  return backtracie_frame_filename_cstr(&sample->frames[frame_index], true, outbuf, outbuf_len);
}

int mpp_sample_frame_line_number(struct mpp_sample *sample, int frame_index) {
  return backtracie_frame_line_number(&sample->frames[frame_index]);
}
