require_relative 'test_helper'

describe MemprofilerPprof::Collector do
  it 'captures backtraces for memory allocations' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    c.start!
    xx = dummy_fn1
    profile_bytes = c.flush
    c.stop!

    # Make sure stuff did actually get allocated
    assert_equal 4096, xx.size

    # Decode the sample backtraces
    pprof = decode_pprof(profile_bytes)
    allocating_stacks = allocation_function_backtraces pprof

    fn2_stacks = allocating_stacks.select { |s| stack_contains? s, %w[dummy_fn1 dummy_fn2] }
    fn3_stacks = allocating_stacks.select { |s| stack_contains? s, %w[dummy_fn1 dummy_fn2 dummy_fn3] }
    fn4_stacks = allocating_stacks.select { |s| stack_contains? s, %w[dummy_fn1 dummy_fn2 dummy_fn3 dummy_fn4] }

    # It's basically impossible to know how many allocations these things are doing; a lot of the allocations
    # actually come from (cfunc)'s underneath these functions too (e.g. "*"). Just assert that we got some
    # profiles.
    assert_operator fn2_stacks.size, :>=, 1
    assert_operator fn3_stacks.size, :>=, 1
    assert_operator fn4_stacks.size, :>=, 1
  end

  it 'captures all allocations when sample rate is 1' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)

    # Warm up the symbol intern cache inside GC.stat
    GC.stat(:total_allocated_objects)

    c.start!
    objs_start = GC.stat(:total_allocated_objects)
    dummy_fn1
    objs_stop = GC.stat(:total_allocated_objects)
    profile_bytes = c.flush
    c.stop!

    pprof = decode_pprof(profile_bytes)
    profiled_allocations = total_allocations pprof
    gc_stat_allocations = objs_stop - objs_start

    assert_in_delta(gc_stat_allocations, profiled_allocations, 3)
  end

  it 'captures allocation sizes' do
    def big_allocation_func
      SecureRandom.hex(50000)
    end

    def medium_allocation_func
      SecureRandom.hex(5000)
    end

    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)

    c.start!
    big_allocation_func
    medium_allocation_func
    profile_bytes = c.flush
    c.stop!

    pprof = decode_pprof(profile_bytes)
    big_bytes = allocation_size_sum_under(pprof, 'big_allocation_func')
    medium_bytes = allocation_size_sum_under(pprof, 'medium_allocation_func')

    assert_operator big_bytes, :>=, 50000
    assert_operator medium_bytes, :>=,  5000
    assert_operator medium_bytes, :<, 10000
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
    c.start!
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

    profile_bytes = c.flush
    c.stop!

    pprof = decode_pprof(profile_bytes)
    allocating_stacks = allocation_function_backtraces pprof

    ractor1_stacks = allocating_stacks.select { |s| stack_contains? s, ['ractor1_method'] }
    ractor2_stacks = allocating_stacks.select { |s| stack_contains? s, ['ractor2_method'] }

    assert_operator ractor1_stacks, :>, 0
    assert_operator ractor2_stacks, :>, 0
  end
end
