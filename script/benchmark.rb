#!/usr/bin/env ruby

require_relative "benchmark_support"

Benchmark.bm(36) do |b|
  leak_pit = []
  sc = BENCHMARK_SCENARIO.dup
  GC.start
  b.report("no profiling (1)") do
    benchmark_machine(sc, leak_pit)
  end

  leak_pit = []
  sc = BENCHMARK_SCENARIO.dup
  GC.start
  b.report("no profiling (2)") do
    benchmark_machine(sc, leak_pit)
  end

  [1, 10, 100].each do |perc|
    leak_pit = []
    sc = BENCHMARK_SCENARIO.dup
    GC.start
    $collector = MemprofilerPprof::Collector.new
    $collector.sample_rate = 0.01 * perc
    b.report("with profiling (#{perc}%, no flush)") do
      $collector.start!

      benchmark_machine(sc, leak_pit)

      $collector.stop!
    end

    leak_pit = []
    sc = BENCHMARK_SCENARIO.dup
    GC.start
    b.report("with reporting (#{perc}%, with flush)") do
      $collector.start!

      benchmark_machine(sc, leak_pit)
      File.write("tmp/benchmark-#{perc}p.pb.gz", $collector.flush.pprof_data)
      $collector.stop!
    end
  end
end
