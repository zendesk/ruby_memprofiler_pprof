# frozen_string_literal: true

require_relative 'test_helper'
require 'fileutils'
require 'tmpdir'
require 'logger'

class FakeSleepExtension

end

describe MemprofilerPprof::FileFlusher do
  before do
    @dir = Dir.mktmpdir
  end

  after do
    FileUtils.remove_entry @dir
  end

  def alloc_method_1
    SecureRandom.hex(10)
  end
  def alloc_method_2
    SecureRandom.hex(10)
  end
  def alloc_method_3
    SecureRandom.hex(10)
  end

  def wait_for_file_exist(file)
    5000.times do
      return if File.exist?(file)
      sleep 1
    end
    raise "File #{file} did not get created"
  end


    it 'writes profiles to the directory' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    flusher = MemprofilerPprof::FileFlusher.new(c,
      pattern: "#{@dir}/%{index}.pprof",
      interval: 2,
      logger: Logger.new(STDERR),
      yield_gvl: true,
      proactively_yield_gvl: true,
    )

    flusher.run do
      leak_list = []
      c.profile do
        leak_list << alloc_method_1
        wait_for_file_exist "#{@dir}/0.pprof"
        leak_list << alloc_method_2
        wait_for_file_exist "#{@dir}/1.pprof"
      end
    end

    assert File.exist?("#{@dir}/0.pprof")
    assert File.exist?("#{@dir}/1.pprof")

    profile_1 = DecodedProfileData.new(File.read("#{@dir}/0.pprof"))
    profile_2 = DecodedProfileData.new(File.read("#{@dir}/1.pprof"))

    assert profile_1.heap_samples_including_stack(['alloc_method_1']).size > 0
    assert profile_1.heap_samples_including_stack(['alloc_method_2']).empty?
    assert profile_2.heap_samples_including_stack(['alloc_method_2']).size > 0
  end

  it 'writes for both parent and child on fork' do
    c = MemprofilerPprof::Collector.new(sample_rate: 1.0)
    flusher = MemprofilerPprof::FileFlusher.new(c,
      pattern: "#{@dir}/%{pid}-%{index}.pprof",
      interval: 2,
      logger: Logger.new(STDERR),
    )

    flusher.start!
    c.start!

    leak_list = []
    leak_list << alloc_method_1
    parent_pid = Process.pid
    child_pid = fork do
      # Child process
      leak_list << alloc_method_2
      wait_for_file_exist "#{@dir}/#{Process.pid}-0.pprof"
      c.stop!
      flusher.stop!
      exit! 0
    end
    leak_list << alloc_method_3
    wait_for_file_exist "#{@dir}/#{Process.pid}-0.pprof"

    c.stop!
    flusher.stop!

    Process.waitpid child_pid
    profile_parent = DecodedProfileData.new(File.read("#{@dir}/#{parent_pid}-0.pprof"))
    profile_child = DecodedProfileData.new(File.read("#{@dir}/#{child_pid}-0.pprof"))

    assert profile_parent.heap_samples_including_stack(['alloc_method_1']).size > 0
    assert profile_parent.heap_samples_including_stack(['alloc_method_2']).empty?
    assert profile_parent.heap_samples_including_stack(['alloc_method_3']).size > 0

    assert profile_child.heap_samples_including_stack(['alloc_method_1']).size > 0
    assert profile_child.heap_samples_including_stack(['alloc_method_2']).size > 0
    assert profile_child.heap_samples_including_stack(['alloc_method_3']).empty?
  end
end
