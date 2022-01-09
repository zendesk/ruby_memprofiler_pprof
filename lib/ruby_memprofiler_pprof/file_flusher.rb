# frozen_string_literal: true

module MemprofilerPprof
  class FileFlusher

    attr_reader :collector

    def initialize(collector, pattern: 'tmp/profiles/mem-%{pid}-%{isotime}.pprof', interval: 30, logger: nil)
      @collector = collector
      @pattern = pattern
      @interval = interval
      @logger = logger
      @thread = nil
      @profile_counter = 0
    end

    def start!
      @thread = Thread.new { flusher_thread }
    end

    def stop!
      @thread.kill
      @thread.join
    end

    def run
      start!
      begin
        yield
      ensure
        stop!
      end
    end

    private

    def flusher_thread
      prev_flush_duration = 0
      loop do
        sleep([0, @interval - prev_flush_duration].max)
        t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        begin
          profile_data = @collector.flush
          File.write(template_string(@pattern), profile_data.pprof_data)
        rescue => e
          @logger&.error("FileFlusher: failed to flush profiling data: #{e.inspect}")
        ensure
          t2 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
          prev_flush_duration = (t2 - t1)
          @profile_counter += 1
        end
      end
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
