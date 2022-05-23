# frozen_string_literal: true

module MemprofilerPprof
  class BlockFlusher
    attr_reader :collector

    def initialize(collector, interval: 30, logger: nil, on_flush: nil)
      @collector = collector
      @interval = interval
      @logger = logger
      @thread = nil
      @on_flush = on_flush
    end

    def start!
      stop!
      @thread = Thread.new { flusher_thread }
      @atfork_handler = MemprofilerPprof::Atfork.at_fork(:child, &method(:at_fork_in_child))
    end

    def stop!
      @thread&.kill
      @thread&.join
      @thread = nil
      @atfork_handler&.remove!
      @atfork_handler = nil
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
          @on_flush&.call(profile_data)
        rescue => e
          @logger&.error("BaseFlusher: failed to flush profiling data: #{e.inspect}")
        ensure
          t2 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
          prev_flush_duration = (t2 - t1)
        end
      end
    end

    def at_fork_in_child
      start! if @thread && !@thread.alive?
    end
  end
end
