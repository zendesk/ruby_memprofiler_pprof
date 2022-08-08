#include "ruby_memprofiler_pprof.h"
#include <backtracie.h>
#include <ruby.h>

// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample) {
  return sizeof(struct mpp_sample) + sample->frames_capacity * sizeof(struct mpp_backtrace_frame);
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

struct mpp_sample *mpp_sample_capture(struct mpp_strtab *strtab, VALUE allocated_value_weak, bool pretty) {
  VALUE thread = rb_thread_current();
  int stack_size = backtracie_frame_count_for_thread(thread);
  struct mpp_sample *sample = mpp_xmalloc(sizeof(struct mpp_sample) + stack_size * sizeof(struct mpp_backtrace_frame));
  sample->frames_capacity = stack_size;
  sample->frames_count = 0;
  sample->allocated_value_weak = allocated_value_weak;

  for (int i = 0; i < stack_size; i++) {
    raw_location backtracie_loc = {0};
    bool is_valid = backtracie_capture_frame_for_thread(thread, i, &backtracie_loc);
    if (!is_valid)
      continue;

    char strbuf[256];
    struct mpp_backtrace_frame *fr = &sample->frames[sample->frames_count];
    sample->frames_count++;

    backtracie_frame_filename_cstr(&backtracie_loc, true, strbuf, sizeof(strbuf));
    mpp_strtab_intern_cstr(strtab, strbuf, &fr->file_name, &fr->file_name_len);
    backtracie_frame_name_cstr(&backtracie_loc, strbuf, sizeof(strbuf));
    mpp_strtab_intern_cstr(strtab, strbuf, &fr->function_name, &fr->function_name_len);
    fr->line_number = backtracie_frame_line_number(&backtracie_loc);
  }

  return sample;
}
