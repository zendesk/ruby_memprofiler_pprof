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

    # We put the GC.stat calls inside the profiling calls, so the profiling must be >= the stat values.
    assert_operator profiled_allocations, :>=, gc_stat_allocations
    assert_in_delta(gc_stat_allocations, profiled_allocations, 3)
  end
end
