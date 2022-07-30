#!/usr/bin/env ruby

require "bundler/setup"
require "sinatra/base"
require "securerandom"
require "rack/mock"
require "thwait"
require "ruby_memprofiler_pprof"

Thread.abort_on_exception = true

# This is a mock sinatra app, which will generate all the benchmarking.
class TestApp < Sinatra::Base
  @bench_bucket = []
  get "/bench" do
    if @bench_bucket.size < 50000
      rand(0..100).times do
        @bench_bucket << SecureRandom.hex(20)
      end
    else
      rand(0..100).times do
        @bench_bucket.pop
      end
    end

    @bench_bucket.pop || "empty!"
  end
end

collector = MemprofilerPprof::Collector.new
collector.sample_rate = 1.0

threads = (0..5).map do
  Thread.new do
    loop do
      r = Rack::MockRequest.new(TestApp)
      r.get("/bench")
    end
  end
end

threads << Thread.new do
  collector.start!
  loop do
    sleep 15
    collector.flush # Flush to dev-null
  end
end

threads << Thread.new do
  collector.start!
  loop do
    sleep 50
    collector.stop!
    collector.start!
  end
end

threads << Thread.new do
  loop do
    sleep 10
    GC.compact
  end
end

ThreadsWait.all_waits(*threads)
