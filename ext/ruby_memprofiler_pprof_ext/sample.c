#include "ruby_memprofiler_pprof.h"
#include <backtracie.h>
#include <ruby.h>

static int increment_mark_table_refcount(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  if (!existing) {
    *value = 1;
  } else {
    (*value)++;
  }
  return ST_CONTINUE;
}

static int decrement_mark_table_refcount(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  (*value)--;
  if (!value) {
    return ST_DELETE;
  } else {
    return ST_CONTINUE;
  }
}

// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample) {
  return sizeof(struct mpp_sample) + sample->frames_capacity * sizeof(raw_location);
}

// Free the sample, incl. releasing strings it interned.
void mpp_sample_free(struct mpp_sample *sample, st_table *mark_table) {
  // Decrement applicable refcounts.
  for (size_t i = 0; i < sample->frames_count; i++) {
    raw_location *frame = &sample->frames[i];
    if (RTEST(frame->iseq)) {
      st_update(mark_table, frame->iseq, decrement_mark_table_refcount, 0);
    }
    if (RTEST(frame->callable_method_entry)) {
      st_update(mark_table, frame->callable_method_entry, decrement_mark_table_refcount, 0);
    }
    if (RTEST(frame->self_or_self_class)) {
      st_update(mark_table, frame->self_or_self_class, decrement_mark_table_refcount, 0);
    }
  }

  mpp_free(sample);
}

struct mpp_sample *mpp_sample_capture(st_table *mark_table, VALUE allocated_value_weak, bool pretty) {
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
      // Increment refcounts
      if (RTEST(frame->iseq)) {
        st_update(mark_table, frame->iseq, increment_mark_table_refcount, 0);
      }
      if (RTEST(frame->callable_method_entry)) {
        st_update(mark_table, frame->callable_method_entry, increment_mark_table_refcount, 0);
      }
      if (RTEST(frame->self_or_self_class)) {
        st_update(mark_table, frame->self_or_self_class, increment_mark_table_refcount, 0);
      }
      sample->frames_count++;
    }
  }

  return sample;
}
