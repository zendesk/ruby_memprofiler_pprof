#!/usr/bin/env ruby

require_relative 'benchmark_support'

leak_pit = []
sc = BENCHMARK_SCENARIO.dup
GC.start
$collector = MemprofilerPprof::Collector.new
$collector.sample_rate = 0.1
time = Benchmark.measure do
    $collector.start!
    benchmark_machine(sc, leak_pit)
    File.open("tmp/benchmark-10p.pb.gz", 'w') do |f|
        f.write $collector.flush.pprof_data
    end
    $collector.stop!
end

puts time
