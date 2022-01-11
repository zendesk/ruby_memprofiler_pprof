# frozen_string_literal: true

require_relative 'test_helper'

describe MemprofilerPprof::Collector do
  it 'captures backtraces for memory allocations' do
    def dummy_fn1
      dummy_fn2
    end

    def dummy_fn2
      v1 = "a" * 1024
      v1 + dummy_fn3 + dummy_fn4
    end

    def dummy_fn3
      SecureRandom.hex(512) + dummy_fn4
    end

    def dummy_fn4
      'z' * 1024
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    profile_data = c.profile do
      xx = dummy_fn1
    end

    # Decode the sample backtraces
    pprof = DecodedProfileData.new(profile_data)
    fn2_stacks = pprof.allocation_samples_including_stack %w[dummy_fn1 dummy_fn2]
    fn3_stacks = pprof.allocation_samples_including_stack %w[dummy_fn1 dummy_fn2 dummy_fn3]
    fn4_stacks = pprof.allocation_samples_including_stack %w[dummy_fn1 dummy_fn2 dummy_fn3 dummy_fn4]

    # It's basically impossible to know how many allocations these things are doing; a lot of the allocations
    # actually come from (cfunc)'s underneath these functions too (e.g. "*"). Just assert that we got some
    # profiles.
    assert_operator fn2_stacks.size, :>=, 1
    assert_operator fn3_stacks.size, :>=, 1
    assert_operator fn4_stacks.size, :>=, 1
  end

  it 'captures backtraces for memory allocations in slowrb mode' do
    def dummy_fn1
      dummy_fn2
    end

    def dummy_fn2
      v1 = "a" * 1024
      v1 + dummy_fn3 + dummy_fn4
    end

    def dummy_fn3
      SecureRandom.hex(512) + dummy_fn4
    end

    def dummy_fn4
      'z' * 1024
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0, bt_method: :slowrb)
    profile_data = c.profile do
      xx = dummy_fn1
    end

    # Decode the sample backtraces
    pprof = DecodedProfileData.new(profile_data)
    fn2_stacks = pprof.samples_including_stack %w[dummy_fn1 dummy_fn2]
    fn3_stacks = pprof.samples_including_stack %w[dummy_fn1 dummy_fn2 dummy_fn3]
    fn4_stacks = pprof.samples_including_stack %w[dummy_fn1 dummy_fn2 dummy_fn3 dummy_fn4]

    assert_operator fn2_stacks.size, :>=, 1
    assert_operator fn3_stacks.size, :>=, 1
    assert_operator fn4_stacks.size, :>=, 1
  end

  it 'captures all allocations when sample rate is 1' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)

    # Warm up the symbol intern cache inside GC.stat
    GC.stat(:total_allocated_objects)

    objs_start = nil
    objs_stop = nil
    profile_data = c.profile do
      objs_start = GC.stat(:total_allocated_objects)
      100.times { SecureRandom.hex 50 }
      objs_stop = GC.stat(:total_allocated_objects)
    end

    pprof = DecodedProfileData.new(profile_data)
    gc_stat_allocations = objs_stop - objs_start

    assert_in_delta(gc_stat_allocations, pprof.total_allocations, 3)
  end

  it 'captures allocation sizes' do
    def big_allocation_func
      SecureRandom.hex(50000)
    end

    def medium_allocation_func
      SecureRandom.hex(5000)
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    profile_data = c.profile do
      big_allocation_func
      medium_allocation_func
    end

    pprof = DecodedProfileData.new(profile_data)
    big_bytes = pprof.total_allocation_size under: 'big_allocation_func'
    medium_bytes = pprof.total_allocation_size under: 'medium_allocation_func'

    assert_operator big_bytes, :>=, 50000
    assert_operator medium_bytes, :>=,  5000
    assert_operator medium_bytes, :<, 20000
  end

  it 'handles compaction' do
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
    end
  end

  it 'captures allocations in multiple ractors' do
    skip "Ractors can't work with newobj tracepoints: https://bugs.ruby-lang.org/issues/18464"
    skip "Ractors not available in Ruby #{RUBY_VERSION}" unless defined?(::Ractor)

    def ractor1_method
      SecureRandom.hex 50
    end

    def ractor2_method
      SecureRandom.hex 50
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    profile_data = c.profile do
      r1 = Ractor.new do
        1000.times do
          Ractor.yield ractor1_method
        end
      end
      r2 = Ractor.new do
        1000.times do
          Ractor.yield ractor2_method
        end
      end

      loop do
        Ractor.select(r1, r2)
      end
    end

    pprof = DecodedProfileData.new(profile_data)
    ractor1_stacks = pprof.samples_including_stack ['ractor1_method']
    ractor2_stacks = pprof.samples_including_stack ['ractor2_method']

    assert_operator ractor1_stacks, :>, 0
    assert_operator ractor2_stacks, :>, 0
  end

  it 'respects max allocation samples' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0, max_allocation_samples: 20)
    profile_data = c.profile do
      100.times { SecureRandom.hex 50 }
    end

    pprof = DecodedProfileData.new(profile_data)
    assert_equal 20, pprof.allocation_samples_count
    assert_operator pprof.dropped_samples_allocation_bufsize, :>=, 80
    assert_equal 20, pprof.allocation_samples.size
  end

  it 'respects max heap samples' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0, max_heap_samples: 20)
    retain = []
    profile_data = c.profile do
      100.times { retain << SecureRandom.hex(50) }
    end

    pprof = DecodedProfileData.new(profile_data)
    assert_equal 20, pprof.allocation_samples_count
    assert_operator pprof.dropped_samples_heap_bufsize, :>=, 80
    assert_equal 20, pprof.allocation_samples.size
  end

  it 'captures backtraces for retained objects' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)

    $leaky_bucket = []
    def leak_into_bucket
      $leaky_bucket << SecureRandom.hex(20)
    end

    c.start!
    1000.times { leak_into_bucket }
    profile_1 = DecodedProfileData.new c.flush
    profile_2 = DecodedProfileData.new c.flush
    $leaky_bucket = nil
    10.times { GC.start }
    profile_3 = DecodedProfileData.new c.flush
    c.stop!

    assert_operator profile_1.heap_samples_including_stack(['leak_into_bucket']).size, :>=, 1000
    assert_operator profile_2.heap_samples_including_stack(['leak_into_bucket']).size, :>=, 1000
    assert_operator profile_3.heap_samples_including_stack(['leak_into_bucket']).size, :<, 10
  end
end
