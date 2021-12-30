module MemprofilerPprof
  # Collector is the class responsible for actually installing the tracepoint probes used to
  # instrument memory allocations. The intended way to use it is:
  #
  #   * Instantiate and configure a Collector object
  #   * Call something like "c.global_tags.merge!({phase: 'initializing'})". This will mark the first allocations
  #     as being part of the "initializing" phase.
  #   * Call #start_profiling!, early in program lifetime (i.e. before Rails boots)
  #   * Once your app/rails has finished booting, modify #global_tags again to set phase: "running" or such.
  #   * In middleware, call "c.threadlocal_tags.merge!" at the beginning & end of requests to tag allocations within
  #     that request as having come from that request. You might e.g. tag it with a distributed tracing span ID, for
  #     example.
  #   * Periodically, call #rotate_profile!. This will return a binary string representing a pprof-protobuf-formatted
  #     profile, and will clear the Collector's internal buffers of previous allocation events. Save the returned string
  #     somewhere where it's useful to you. This is safe to do on another thread.
  class Collector

    # Instantiates a new Collector. Should ideally only be called once in the lifetime of a program.
    #
    # @param global_tags [Hash] Default tags to apply to all samples collected by this collector
    # @param u32_sample_rate [Float] Fraction of Ruby memory allocations that should be sampled
    # @param max_samples [Integer] If more than this number of samples are collected before a call to
    #     #rotate_profile!, then collection will stop and #missed_samples will be incremented.
    # @param logger [Logger] Logger instance to use for diagnostic info.
    def initialize(global_tags: {}, sample_rate: 0.01, max_samples: 10_000, logger: nil)
      @global_tags = global_tags
      self.sample_rate = sample_rate
      @max_samples = max_samples
      @threadlocal_tags = Concurrent::ThreadLocalVar.new({})
      @global_tags = {}
      @missed_samples = 0
      @logger = logger
      @running = false

      @sample_buffer = []
      @sample_buffer_next_ix = 0
    end

    # Tags that will be applied to all samples collected by this collector
    # @return [Hash] The tags as a hash of key -> value pairs
    attr_accessor :global_tags

    # @!attribute [rw]
    # Tags that will be applied only to samples collected from the thread that this attribute is accessed from.
    # @return [Hash] The set of tags for this thread.
    def threadlocal_tags
      @threadlocal_tags.value
    end

    def threadlocal_tags=(newval)
      @threadlocal_tags.value = newval
    end

    # @!attribute [rw]
    # Fraction of Ruby memory allocations that will be sampled. Reduce this value to decrease the performance
    # impact of this gem.
    # @return [Float] The sample rate
    def sample_rate
      @sample_rate
    end

    def sample_rate=(newval)
      # The validation here is needed because if garbage is passed here, we could wind up longjump'ing out of
      # the newobj hook in C land, which seems like a bad idea.
      raise "Not a number!" unless newval.is_a? Numeric
      raise "Not between 0 and 1!" unless newval >= 0 && newval <= 1
      @sample_rate = newval
      set_sample_rate_cimpl
      @sample_rate
    end

    # @!attribute [rw]
    # The maximum number of samples allowed per profile. If more samples than this are collected before a call
    # to #rotate_profile! is made, the4n samples will no longer be collected and instead #missed_samples will
    # be incremented.
    # Note that to minimise the performance impact of this gem, the full size of max_samples
    # will be pre-allocated.
    # Also note that any change to max_samples will not take effect until a call to either #start_profiling!
    # or #rotate_profile!.
    # @return [Integer] The current max samples per profile
    attr_accessor :max_samples

    # The total number of samples that have been missed because the sample buffer was full. If this is nonzero,
    # consider increasing #max_samples
    # @return [Integer] Total number of missed samples
    attr_reader :missed_samples

    # Logger used to send diagnostic information about the profiler. Not really intended to be on in production.
    # @return [Logger] The logger instance
    attr_accessor :logger

    # Returns whether or not we are currently profiling
    # @return [Boolean] Whether or not we are currently profiling
    attr_reader :running

    # Begins collecting samples and appending them to the internal sample buffer
    def start_profiling!
      raise "Already profiling!" if @running

      swap_sample_buffer

      # Attach tracepoint hooks
      enable_profiling_cimpl

      @running = true
      @profiling_started_at = Time.now
    end

    # Pauses the collection of samples, and disconnects from the Tracepoint API.
    def stop_profiling!
      raise "Not currently profiling!" unless @running

      disable_profiling_cimpl
      @running = false
      @sample_buffer_next_ix = 0
      @sample_buffer = []
    end

    # Rotates the sample collection buffer. The sample buffer is formatted as a protobuf-encoded, pprof-compatible
    # string to be returned by this method. The sample buffer is then emptied so that further samples can be
    # collected. This method is safe to call from any thread.
    # @ return [String] A protobuf-encoded pprof profile
    def rotate_profile!
      existing_samples, samples_start_at = swap_sample_buffer
      samples_end_at = Time.now

      samples_start_at_nanos = samples_start_at.to_i * (10 ** 9) + samples_start_at.nsec
      samples_end_at_nanos = samples_end_at.to_i * (10 ** 9) + samples_end_at.nsec

      st = StringTable.new
      loc_table = LocationTable.new(st)
      pprof_profile = Perftools::Profiles::Profile.new
      pprof_profile.sample_type << Perftools::Profiles::ValueType.new(
        type: st.index("allocations"), unit: st.index("count")
      )
      existing_samples.each do |sample|
        next unless sample.filled

        sample_proto = Perftools::Profiles::Sample.new
        sample.allocation_stack.each do |frame|
          sample_proto.location_id << loc_table.location_id(frame)
        end
        sample_proto.value = Google::Protobuf::RepeatedField.new(:int64, [1]) # allocation count
        pprof_profile.sample << sample_proto
      end
      pprof_profile.location = loc_table.locations
      pprof_profile.function = loc_table.functions
      pprof_profile.string_table = st.string_table
      pprof_profile.time_nanos = samples_start_at_nanos
      pprof_profile.duration_nanos = samples_end_at_nanos - samples_start_at_nanos

      Zlib.gzip pprof_profile.class.encode(pprof_profile)
    end

    def inspect
      "#<#{self.class.name}:#{self.object_id.to_s 16}>"
    end

    private

    def swap_sample_buffer
      old_started_at = @samples_started_at
      existing_samples = swap_sample_buffer_cimpl
      @samples_started_at = Time.now
      [existing_samples, old_started_at]
    end
  end

  Sample = Struct.new(:filled, :allocation_stack, :allocation_klass, :allocation_size)
end
