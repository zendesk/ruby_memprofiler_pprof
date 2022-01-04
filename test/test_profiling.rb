require_relative 'test_helper'

describe MemprofilerPprof::Collector do
  it 'captures backtraces for memory allocations' do
    c = MemprofilerPprof::Collector.new
    c.sample_rate = 1.0
    c.start_profiling!
    xx = dummy_fn1
    profile_bytes = c.rotate_profile!
    c.stop_profiling!

    # Make sure stuff did actually get allocated
    assert_equal 4096, xx.size

    # Decode the sample backtraces
    pprof = decode_pprof(profile_bytes)
    allocating_stacks = allocation_function_backtraces pprof

    fn2_stacks = allocating_stacks.select { |s| stack_ends_with? s, %w[dummy_fn1 dummy_fn2] }
    fn3_stacks = allocating_stacks.select { |s| stack_ends_with? s, %w[dummy_fn1 dummy_fn2 dummy_fn3] }
    fn4_stacks = allocating_stacks.select { |s| stack_ends_with? s, %w[dummy_fn1 dummy_fn2 dummy_fn3 dummy_fn4] }

    # It's basically impossible to know how many allocations these things are doing; a lot of the allocations
    # actually come from (cfunc)'s underneath these functions too (e.g. "*"). Just assert that we got some
    # profiles.
    assert_operator fn2_stacks.size, :>=, 1
    assert_operator fn3_stacks.size, :>=, 1
    assert_operator fn4_stacks.size, :>=, 1
  end
end
