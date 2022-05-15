# frozen_string_literal: true

# This file gets loaded by libexec/ruby_memprofiler_pprof_profile as part
# of RUBYOPTS for a child process. This means we're going to run in some target
# process _before_ any of the app code is run. Our job here is to set up
# heap profiling in some default way.

require 'logger'
require 'ruby_memprofiler_pprof'

# TODO - handle some kind of atfork stuff.

collector = MemprofilerPprof::Collector.new
collector.sample_rate = 0.5
flusher = MemprofilerPprof::FileFlusher.new(collector, logger: Logger.new(STDERR))
collector.start!
flusher.start!
