#!/usr/bin/env ruby

require 'bundler/setup'
require 'logger'

RubyMemoryMonitor.configure do |c|
  c.allocation_sample_rate = 0.01
  c.logger = Logger.new(STDERR)
end

RubyMemoryMonitor.enable_profiling!


