# frozen_string_literal: true

require_relative "test_helper"

describe MemprofilerPprof::Collector do
  it "captures backtraces for retained objects" do
    $bucket = []
    def leak_into_bucket_1
      $bucket << SecureRandom.hex(20)
    end

    def leak_into_bucket_2
      $bucket << SecureRandom.hex(20)
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)

    c.start!
    1000.times { leak_into_bucket_1 }
    profile_1_data = c.flush
    1000.times { leak_into_bucket_2 }
    profile_2_data = c.flush
    $bucket = []
    1000.times { leak_into_bucket_2 }
    10.times { GC.start }
    # At this point, there _should_ be no more objects allocated by leak_into_bucket_1
    profile_3_data = c.flush
    c.stop!

    profile_1 = DecodedProfileData.new(profile_1_data)
    profile_2 = DecodedProfileData.new(profile_2_data)
    profile_3 = DecodedProfileData.new(profile_3_data)

    assert_operator profile_1.heap_samples_including_stack(["leak_into_bucket_1"]).size, :>=, 1000
    assert_operator profile_2.heap_samples_including_stack(["leak_into_bucket_1"]).size, :>=, 1000
    assert_operator profile_3.heap_samples_including_stack(["leak_into_bucket_1"]).size, :<, 10
  end

  it "captures allocation sizes" do
    def big_allocation_func
      SecureRandom.hex(50000)
    end

    def medium_allocation_func
      SecureRandom.hex(5000)
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    leak_list = []
    profile_data = c.profile do
      leak_list << big_allocation_func
      leak_list << medium_allocation_func
    end

    pprof = DecodedProfileData.new(profile_data)
    big_bytes = pprof.total_retained_size under: "big_allocation_func"
    medium_bytes = pprof.total_retained_size under: "medium_allocation_func"

    assert_operator big_bytes, :>=, 50000
    assert_operator medium_bytes, :>=, 5000
    assert_operator medium_bytes, :<, 20000
  end

  it "handles compaction" do
    skip "Compaction requires Ruby 2.7+" unless ::GC.respond_to?(:compact)

    def big_array_compacting_method
      the_array = Array.new(20000)
      20000.times do |i|
        the_array[i] = SecureRandom.hex(10)
        if i > 10 && i % 3 == 0
          the_array[i - 10] = nil
        end
      end
      the_array
    end

    # If we're not handling compaction correctly, this will segfault because we won't notice
    # that all the elements of the_array got moved.
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    keep_live = nil
    c.profile do
      keep_live = big_array_compacting_method
      2.times { GC.compact }
      keep_live = big_array_compacting_method
    end
  end

  it "respects max heap samples" do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0, max_heap_samples: 20)
    retain = []
    profile_data = c.profile do
      100.times { retain << SecureRandom.hex(50) }
    end

    pprof = DecodedProfileData.new(profile_data)
    assert_operator pprof.dropped_samples_heap_bufsize, :>=, 80
  end
end
