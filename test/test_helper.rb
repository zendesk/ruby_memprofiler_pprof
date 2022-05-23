# frozen_string_literal: true

require 'bundler/setup'
require 'ruby_memprofiler_pprof'
require_relative 'pprof_pb'
require "minitest/autorun"
require 'securerandom'
require 'zlib'
require 'timecop'

class DecodedProfileData
  class Sample
    attr_accessor :backtrace, :line_backtrace, :allocations, :allocation_size, :retained_objects, :retained_size

    def backtrace_contains?(stack_segment)
      return false if stack_segment.size > backtrace.size
      (0..(backtrace.size - stack_segment.size)).any? do |i|
        stack_segment.reverse == backtrace[i...(i + stack_segment.size)]
      end
    end
  end

  attr_reader :samples, :pprof

  def initialize(profile_data)
    if profile_data.is_a?(MemprofilerPprof::ProfileData)
      @profile_data = profile_data
      pprof_data = @profile_data.pprof_data
    else
      @profile_data = nil
      pprof_data = profile_data
    end
    @pprof = Perftools::Profiles::Profile.decode Zlib.gunzip(pprof_data)

    @fn_map = @pprof.function.to_h { |fn| [fn.id, fn] }
    @loc_map = @pprof.location.to_h { |loc| [loc.id, loc] }

    @samples = @pprof.sample.map do |sample_proto|
      s = Sample.new
      s.line_backtrace = []
      s.backtrace = []
      sample_proto.location_id.map do |loc_id|
        fn = @fn_map[@loc_map[loc_id].line[0].function_id]
        fn_name = @pprof.string_table[fn.name]
        filename = @pprof.string_table[fn.filename]
        line_no = @loc_map[loc_id].line[0].line

        s.backtrace << fn_name
        s.line_backtrace << "#{filename}:#{line_no} in #{fn_name}"
      end
      s.allocations = sample_proto.value[0]
      s.allocation_size = sample_proto.value[1]
      s.retained_objects = sample_proto.value[2]
      s.retained_size = sample_proto.value[3]
      s
    end
  end

  def total_allocations
    @samples.reduce(0) { |acc, s| acc += s.allocations }
  end

  def total_allocation_size(under: nil)
    @samples.reduce(0) do |acc, s|
      if under
        next acc unless s.backtrace_contains? [under]
      end
      acc += s.allocation_size
    end
  end

  def samples_including_stack(stack_segment)
    @samples.select { |s| s.backtrace_contains? stack_segment }
  end

  def allocation_samples_including_stack(stack_segment)
    @samples.select { |s| s.backtrace_contains?(stack_segment) && s.allocations > 0 }
  end

  def heap_samples_including_stack(stack_segment)
    @samples.select { |s| s.backtrace_contains?(stack_segment) && s.retained_objects > 0 }
  end


  def allocation_samples
    @samples.select { |s| s.allocations > 0 }
  end

  if Gem::Version.new(RUBY_VERSION) < Gem::Version.new("2.7")
    def method_missing(m, *args, &blk)
      @profile_data.send(m, *args, &blk)
    end
  else
    def method_missing(m, *args, **kwargs, &blk)
      @profile_data.send(m, *args, **kwargs, &blk)
    end
  end
end

module FakeSleepSupport
  def stub_sleep
    @@sleep_mutex = Mutex.new
    @@sleep_cvar = ConditionVariable.new
    @@sleep_counter = 0

    define_method :sleep do |n|
      @@sleep_mutex.synchronize do
        started = @@sleep_counter
        while @@sleep_counter < (started + n)
          @@sleep_cvar.wait(@@sleep_mutex)
        end
      end
      n
    end
  end

  def unstub_sleep
    undef_method :sleep
  end

  def advance_sleep(n)
    @@sleep_mutex.synchronize do
      @@sleep_counter += n
      @@sleep_cvar.broadcast
    end
  end
end

def setup_fake_sleep(clazz)
  clazz.singleton_class.prepend(FakeSleepSupport) unless clazz.ancestors.include?(FakeSleepSupport)
  clazz.stub_sleep
end
