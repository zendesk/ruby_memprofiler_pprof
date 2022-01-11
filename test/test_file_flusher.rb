# frozen_string_literal: true

require_relative 'test_helper'
require 'fileutils'
require 'tmpdir'

describe MemprofilerPprof::FileFlusher do
  before do
    @dir = Dir.mktmpdir
    Timecop.freeze(Time.now)
  end

  after do
    Timecop.return
    FileUtils.remove_entry @dir
  end

  it 'writes profiles to the directory' do
    def alloc_method_1
      SecureRandom.hex(10)
    end
    def alloc_method_2
      SecureRandom.hex(10)
    end


    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    flusher = MemprofilerPprof::FileFlusher.new(c, pattern: "#{@dir}/%{index}.pprof", interval: 15)

    # Stub Kernel#sleep within the Flusher to flush on our command.
    flusher_waker = Queue.new
    flusher.instance_variable_set(:@__flusher_waker, flusher_waker)
    flusher.singleton_class.prepend(Module.new do
      def sleep(*)
        @__flusher_waker.pop
        nil
      end
    end)

    def wait_for_file_exist(file)
      50.times do
        return if File.exist?(file)
        sleep 0.1
      end
      raise "File #{file} did not get created"
    end

    flusher.run do
      c.profile do
        alloc_method_1
        flusher_waker << :wake
        wait_for_file_exist "#{@dir}/0.pprof"
        alloc_method_2
        flusher_waker << :wake
        wait_for_file_exist "#{@dir}/1.pprof"
      end
    end

    assert File.exist?("#{@dir}/0.pprof")
    assert File.exist?("#{@dir}/1.pprof")

    profile_1 = DecodedProfileData.new(File.read("#{@dir}/0.pprof"))
    profile_2 = DecodedProfileData.new(File.read("#{@dir}/1.pprof"))

    assert profile_1.allocation_samples_including_stack(['alloc_method_1']).size > 0
    assert profile_1.allocation_samples_including_stack(['alloc_method_2']).empty?
    assert profile_2.allocation_samples_including_stack(['alloc_method_2']).size > 0
    assert profile_2.allocation_samples_including_stack(['alloc_method_1']).empty?
  end
end
