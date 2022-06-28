# frozen_string_literal: true

module MemprofilerPprof
  class BlockFlusher
    attr_reader :collector

    def initialize(
      collector, interval: 30, logger: nil, on_flush: nil, priority: nil,
      yield_gvl: false, proactively_yield_gvl: false
    )
      @collector = collector
      @interval = interval
      @logger = logger
      @thread = nil
      @on_flush = on_flush
      @status_mutex = Mutex.new
      @status_cvar = ConditionVariable.new
      @status = :not_started
      @priority = priority
      @yield_gvl = yield_gvl
      @proactively_yield_gvl = proactively_yield_gvl
    end

    def start!
      stop!
      @status = :running
      @is_paused = false
      @thread = Thread.new { flusher_thread }
      if !@priority.nil?
        @thread.priority = @priority
      end
      @atfork_handler = MemprofilerPprof::Atfork.at_fork(:child, &method(:at_fork_in_child))
    end

    def stop!
      @status_mutex.synchronize do
        @status = :stop
        @status_cvar.broadcast
      end
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

    def pause
      @status_mutex.synchronize do
        @is_paused = true
        @status_cvar.broadcast
      end
    end


    def unpause
      @status_mutex.synchronize do
        @is_paused = false
        @status_cvar.broadcast
      end
    end

    private

    def flusher_thread
      prev_flush_duration = 0
      loop do
        now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        wait_until = now + [0, @interval - prev_flush_duration].max

        @status_mutex.synchronize do
          loop do
            return if @status != :running
            now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
            break if now >= wait_until && !@is_paused
            @status_cvar.wait(@status_mutex, wait_until - now)
          end
        end

        t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        begin
          profile_data = @collector.flush(
            yield_gvl: @yield_gvl, proactively_yield_gvl: @proactively_yield_gvl
          )
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
