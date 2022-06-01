# frozen_string_literal: true

# This file gets loaded by libexec/ruby_memprofiler_pprof_profile as part
# of RUBYOPTS for a child process. This means we're going to run in some target
# process _before_ any of the app code is run. Our job here is to set up
# heap profiling in some default way.

require 'logger'
require 'ruby_memprofiler_pprof'

collector = MemprofilerPprof::Collector.new
collector.sample_rate = ENV.fetch('RUBY_MEMPROFILER_PPROF_SAMPLE_RATE', '1').to_f
collector.allocation_retain_rate = ENV.fetch('RUBY_MEMPROFILER_PPROF_ALLOC_RETAIN_RATE', '1').to_f
if ENV.key?('RUBY_MEMPROFILER_PPROF_MAX_ALLOC_SAMPLES')
  collector.max_allocation_samples = ENV['RUBY_MEMPROFILER_PPROF_MAX_ALLOC_SAMPLES'].to_i
end
if ENV.key?('RUBY_MEMPROFILER_PPROF_MAX_HEAP_SAMPLES')
  collector.max_heap_samples = ENV['RUBY_MEMPROFILER_PPROF_MAX_HEAP_SAMPLES'].to_i
end

kwargs = {
  logger: Logger.new(STDERR)
}
if ENV.key?('RUBY_MEMPROFILER_PPROF_FILE_PATTERN')
  kwargs[:pattern] = ENV['RUBY_MEMPROFILER_PPROF_FILE_PATTERN']
end

flusher = MemprofilerPprof::FileFlusher.new(collector, **kwargs)
collector.start!
flusher.start!
