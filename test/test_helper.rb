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
        stack_segment.reverse.zip(backtrace[i...(i + stack_segment.size)]).all? do |test_frame, pb_frame|
          pb_frame.include? test_frame
        end
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
      s.retained_objects = sample_proto.value[0]
      s.retained_size = sample_proto.value[1]
      s
    end
  end

  def samples_including_stack(stack_segment)
    @samples.select { |s| s.backtrace_contains? stack_segment }
  end

  def heap_samples_including_stack(stack_segment)
    @samples.select { |s| s.backtrace_contains?(stack_segment) && s.retained_objects > 0 }
  end

  def total_retained_objects
    @samples.reduce(0) { |acc, s| acc + s.retained_objects }
  end

  def total_retained_size(under: nil)
    @samples.reduce(0) do |acc, s|
      if under
        next acc unless s.backtrace_contains? [under]
      end
      acc + s.retained_size
    end
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
