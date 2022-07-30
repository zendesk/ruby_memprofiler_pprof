# frozen_string_literal: true

require "mkmf"

# Support GC.compact on Ruby >=- 2.7
have_func("rb_gc_mark_movable", ["ruby.h"])
# Handle Ractors
have_func("rb_ext_ractor_safe", ["ruby.h"])

# Find a random number generator API that doesn't have global state.
has_arc4random = have_func("arc4random", ["stdlib.h"])
has_mrand48_r = have_func("mrand48_r", ["stdlib.h"])
has_getentropy = have_func("getentropy", ["sys/random.h"])
if !has_arc4random && !(has_mrand48_r && has_getentropy)
  abort "Need either arc4random (BSD) or mrand48_r/getentropy (GNU)"
end

# Need zlib
has_zlib_headers = have_header("zlib.h")
has_zlib_lib = have_library("z")
raise "Zlib headers & library are required!" unless has_zlib_headers && has_zlib_lib

# Need to actually link pthreads properly
have_library("pthread") or raise "missing pthread library"
have_func("clock_gettime", ["time.h"]) or raise "missing clock_gettime"

ruby_version = Gem::Version.new RUBY_VERSION

# Ruby >= 3.1 has deprecated/no-opp'd rb_gc_force_recycle, which is good for us, because
# objects freed with that method do not get the freeobj tracepoint called on them.
if Gem::Requirement.new("< 3.1").satisfied_by?(ruby_version)
  $defs << "-DHAVE_WORKING_RB_GC_FORCE_RECYCLE"
end

if Gem::Requirement.new("~> 2.6.0").satisfied_by?(ruby_version)
  $VPATH << "$(srcdir)/ruby_private/ruby26"
  $INCFLAGS << " -I#{File.join($srcdir, "ruby_private/ruby26")}"
elsif Gem::Requirement.new("~> 2.7.0").satisfied_by?(ruby_version)
  $VPATH << "$(srcdir)/ruby_private/ruby27"
  $INCFLAGS << " -I#{File.join($srcdir, "ruby_private/ruby27")}"
elsif Gem::Requirement.new("~> 3.0.0").satisfied_by?(ruby_version)
  $VPATH << "$(srcdir)/ruby_private/ruby30"
  $INCFLAGS << " -I#{File.join($srcdir, "ruby_private/ruby30")}"
elsif Gem::Requirement.new("~> 3.1.0").satisfied_by?(ruby_version)
  $VPATH << "$(srcdir)/ruby_private/ruby31"
  $INCFLAGS << " -I#{File.join($srcdir, "ruby_private/ruby31")}"
else
  raise "Not compatible with Ruby #{ruby_version}"
end

# Ask the ruby interpreter at runtime if these features are enabled or not
# The values detected this way will be used if they are not present in
# the mjit header.
gc_opt_keys = %w[
  GC_DEBUG
  USE_RGENGC
  RGENGC_DEBUG
  RGENGC_CHECK_MODE
  RGENGC_PROFILE
  RGENGC_ESTIMATE_OLDMALLOC
  GC_PROFILE_MORE_DETAIL
  GC_ENABLE_LAZY_SWEEP
  CALC_EXACT_MALLOC_SIZE
  MALLOC_ALLOCATED_SIZE
  MALLOC_ALLOCATED_SIZE_CHECK
  GC_PROFILE_DETAIL_MEMORY
]
gc_opt_keys.each do |key|
  $defs << if GC::OPTS.include?(key)
    "-D#{key}=1"
  else
    "-D#{key}=0"
  end
end

# When including the mjit header, it needs to come _before_ ruby.h otherwise
# the C compiler will spew out multiple definition errors. So we pass a block
# to the various have_* methods to strip that out (just the first one).
STRIP_RUBY_INCLUDE = proc do |src|
  src.sub(/#\s*include\s+["<]ruby\.h[">]/, "")
end

# Record where to find the mjit header
RUBY_MJIT_HEADER = "rb_mjit_min_header-#{RUBY_VERSION}.h"
$defs << "-DRUBY_MJIT_HEADER=\\\"#{RUBY_MJIT_HEADER}\\\""
have_header(RUBY_MJIT_HEADER, &STRIP_RUBY_INCLUDE) or raise "mjit header #{RUBY_MJIT_HEADER} not found"

# Whether or not we have variable-sized slots in Ruby 3.1+
checking_for checking_message("variable sized heap slots") do
  variable_slots_src = <<~CLANG
    #include <#{RUBY_MJIT_HEADER}>
    #include <ruby.h>
    #include <ruby/re.h>
    #include "gc_private.h"
  
    int s = (char *)&((struct heap_page *)0)->slot_size - (char *)0;
    int main(int argc, char **argv) {
      return !!argv[argc];  
    }
  CLANG
  if try_compile(variable_slots_src, &STRIP_RUBY_INCLUDE)
    $defs << "-DHAVE_VARIABLE_SLOT_SIZE"
    true
  else
    false
  end
end

# Set our cflags up _only after_ we have run all the existence checks above; otherwise
# stuff like -Werror can break the test programs.
append_cflags([
  "-g", # Compile with debug info
  "-D_GNU_SOURCE", "-std=gnu11", # Use GNU C extensions (e.g. we use this for atomics)
  "-fvisibility=hidden", # Make sure our upb symbols don't clobber any others from other exts
  "-fno-optimize-sibling-calls"
])
append_cflags(["-Wall", "-Wextra"]) # Enable all the warnings
# These diagnostics are not very interesting at all, just disable them.
append_cflags([
  "-Wno-unused-parameter",
  "-Wno-declaration-after-statement",
  "-Wno-suggest-attribute=noreturn",
  "-Wno-suggest-attribute=format"
])

# Compile the upb objects into our extension as well.
$srcs = Dir.glob(File.join($srcdir, "*.c"))
$srcs += [
  "upb/decode.c",
  "upb/encode.c",
  "upb/msg.c",
  "upb/table.c",
  "upb/upb.c",
  "third_party/utf8_range/naive.c",
  "third_party/utf8_range/range2-neon.c",
  "third_party/utf8_range/range2-sse.c"
].map { |f| File.join($srcdir, "vendor/upb", f) }
$VPATH << "$(srcdir)/vendor/upb/upb"
$VPATH << "$(srcdir)/vendor/upb/third_party/utf8_range"
$INCFLAGS << " -I#{File.join($srcdir, "vendor/upb")}"

# Include the vendored Backtracie header too
$INCFLAGS << " -I#{File.join($srcdir, 'vendor/backtracie')}"

dir_config("ruby")

create_header
create_makefile "ruby_memprofiler_pprof_ext"
