# frozen_string_literal: true

require 'fileutils'
require 'forwardable'

module MemprofilerPprof
  class FileFlusher
    extend Forwardable

    def initialize(
      collector, pattern: 'tmp/profiles/mem-%{pid}-%{isotime}.pprof', interval: 30, logger: nil, priority: nil
    )
      @logger = logger
      @pattern = pattern
      @profile_counter = 0
      @block_flusher = BlockFlusher.new(
        collector, interval: interval, logger: logger, priority: priority, on_flush: method(:on_flush)
      )
    end

    def_delegators :@block_flusher, :start!, :stop!, :run
    attr_accessor :pattern

    private

    def on_flush(profile_data)
      fname = template_string(@pattern)
      dirname = File.dirname(fname)
      FileUtils.mkdir_p dirname
      # Need to explicitly specify the encoding, because some applications might do exotic
      # things to File#default_external/#default_internal that would attempt to convert
      # our protobuf to UTF-8.
      File.write(template_string(@pattern), profile_data.pprof_data, encoding: 'ASCII-8BIT')
      @profile_counter += 1
    rescue => e
      @logger&.error("FileFlusher: failed to flush profiling data: #{e.inspect}")
    end

    def template_string(tmpl)
      vars = {
        pid: Process.pid,
        isotime: Time.now.iso8601,
        unixtime: Time.now.to_i,
        index: @profile_counter,
      }
      sprintf(tmpl, vars)
    end
  end
end
