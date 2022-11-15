# frozen_string_literal: true

module MemprofilerPprof
  class ProfileData
    attr_accessor :pprof_data, :heap_samples_count, :dropped_samples_heap_bufsize,
      :flush_duration_nsecs, :pprof_serialization_nsecs, :sample_add_nsecs,
      :sample_add_without_gvl_nsecs,
      :gvl_proactive_yield_count, :gvl_proactive_check_yield_count

    def to_s
      "<MemprofilerPprof::ProfileData:#{object_id.to_s(16)} (sample counts: " \
        "heap=#{heap_samples_count}, " \
        "dropped_heap_bufsize=#{dropped_samples_heap_bufsize}" \
        ")>"
    end
  end
end
