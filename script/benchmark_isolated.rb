#!/usr/bin/env ruby

require_relative "benchmark_support"

leak_pit = []
sc = BENCHMARK_SCENARIO.dup
GC.start
$collector = MemprofilerPprof::Collector.new
$collector.sample_rate = 0.1
time = Benchmark.measure do
  $collector.start!
  benchmark_machine(sc, leak_pit)
  File.write("tmp/benchmark-10p.pb.gz", $collector.flush.pprof_data)
  $collector.stop!
end

puts time
