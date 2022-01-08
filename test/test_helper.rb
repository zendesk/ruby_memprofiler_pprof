require 'bundler/setup'
require 'ruby_memprofiler_pprof'
require_relative 'pprof_pb'
require "minitest/autorun"
require 'securerandom'
require 'zlib'

# Some dummy functions that call each other
def dummy_fn1
  $dummy_fn_state = 0
  dummy_fn2
end

def dummy_fn2
  v1 = "a" * 1024
  v1 + dummy_fn3 + dummy_fn4
end

def dummy_fn3
  SecureRandom.hex(512) + dummy_fn4
end

def dummy_fn4
  # dummy_fn4 is called twice; make sure it returns a different result both times, otherwise
  # the string we allocated gets interned and it only allocates once.
  $dummy_fn_state += 1
  case $dummy_fn_state
  when 1
    return 'z' * 1024
  when 2
    return 'y' * 1024
  end
end

def decode_pprof(pprof_bytes)
  Perftools::Profiles::Profile.decode Zlib.gunzip(pprof_bytes)
end

def allocation_function_backtraces(pprof)
  fn_map = pprof.function.to_h { |fn| [fn.id, fn] }
  loc_map = pprof.location.to_h { |loc| [loc.id, loc] }

  pprof.sample.map do |sample|
    sample.location_id.map do |loc_id|
      fn = fn_map[loc_map[loc_id].line[0].function_id]
      pprof.string_table[fn.name]
    end
  end
end

def allocation_full_backtraces(pprof)
  fn_map = pprof.function.to_h { |fn| [fn.id, fn] }
  loc_map = pprof.location.to_h { |loc| [loc.id, loc] }

  pprof.sample.map do |sample|
    sample.location_id.map do |loc_id|
      fn = fn_map[loc_map[loc_id].line[0].function_id]
      fn_name = pprof.string_table[fn.name]
      filename = pprof.string_table[fn.filename]
      line_no = loc_map[loc_id].line[0].line
      "#{filename}:#{line_no} in #{fn_name}"
    end
  end
end

def stack_ends_with?(stack, segment)
  return false if segment.size > stack.size
  segment.reverse == stack[0...segment.size]
end

def stack_contains?(stack, segment)
  return false if segment.size > stack.size
  (0..(stack.size - segment.size)).any? do |i|
    segment.reverse == stack[i...(i + segment.size)]
  end
end

def total_allocations(pprof)
  pprof.sample.size
end
