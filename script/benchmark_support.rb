require "bundler/setup"
require "logger"
require "benchmark"
require "securerandom"
require "objspace"
require "pp"
require "ruby_memprofiler_pprof"

NUM_DISTINCT_CLASSES = 10000
NUM_DISTINCT_METHODS = 20000
BENCHMARK_LENGTH = 350000 * 2
MAX_DEPTH = 100

benchmark_classes = (0..NUM_DISTINCT_CLASSES).map do |n|
  TOPLEVEL_BINDING.eval <<~RUBY
    class BenchmarkClass#{n}
        def initialize
            @big_string = SecureRandom.hex(12) * #{n}
        end
    end
    BenchmarkClass#{n}
  RUBY
end

benchmark_methods = (0..NUM_DISTINCT_METHODS).map do |n|
  TOPLEVEL_BINDING.eval <<~RUBY
    def benchmark_method_#{n}(sc, leak_pit)
        benchmark_machine(sc, leak_pit)
    end
    method(:benchmark_method_#{n})
  RUBY
end

depth = 0
elements_in_leak_pit = 0
last_method_direction = :call
BENCHMARK_SCENARIO = (0..BENCHMARK_LENGTH).map do |n|
  if rand < 0.5
    # Call something
    pr_call = (last_method_direction == :call ? 0.85 : 0.15) * (1 - depth / MAX_DEPTH)
    if (rand < pr_call && depth < MAX_DEPTH) || depth == 0
      depth += 1
      next [:call, benchmark_methods[rand(0..NUM_DISTINCT_METHODS)]]
    else
      depth -= 1
      next [:ret]
    end
  elsif rand < 0.55 || elements_in_leak_pit == 0
    # Allocate something
    elements_in_leak_pit += 1
    next [:alloc, benchmark_classes[rand(0..NUM_DISTINCT_CLASSES)]]
  else
    elements_in_leak_pit -= 1
    next [:free]
  end
end

def benchmark_machine(sc, leak_pit)
  loop do
    cmd = sc.slice! 0
    return if cmd.nil?
    if cmd[0] == :call
      cmd[1].call sc, leak_pit
    elsif cmd[0] == :ret
      return
    elsif cmd[0] == :alloc
      leak_pit.push cmd[1].new
    elsif cmd[0] == :free
      leak_pit.pop
    end
  end
end
