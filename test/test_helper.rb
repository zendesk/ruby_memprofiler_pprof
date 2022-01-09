require 'bundler/setup'
require 'ruby_memprofiler_pprof'
require_relative 'pprof_pb'
require "minitest/autorun"
require 'securerandom'
require 'zlib'

# Some dummy functions that call each other
class DecodedProfileData
  class Sample
    attr_accessor :backtrace, :line_backtrace, :allocations, :allocation_size

    def backtrace_contains?(stack_segment)
      return false if stack_segment.size > backtrace.size
      (0..(backtrace.size - stack_segment.size)).any? do |i|
        stack_segment.reverse == backtrace[i...(i + stack_segment.size)]
      end
    end
  end

  attr_reader :samples

  def initialize(profile_data)
    @profile_data = profile_data
    @pprof = Perftools::Profiles::Profile.decode Zlib.gunzip(@profile_data.pprof_data)

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
