# frozen_string_literal: true

module MemprofilerPprof
  class ProfileData
    attr_accessor :pprof_data, :allocation_samples_count, :heap_samples_count,
      :dropped_samples_nolock, :dropped_samples_allocation_bufsize, :dropped_samples_heap_bufsize
  end
end
