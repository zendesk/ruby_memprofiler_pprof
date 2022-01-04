require "mkmf"

append_cflags([
  '-g', '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra',
  '-fvisibility=hidden', # Make sure our upb symbols don't clobber anybody elses
  '-Wno-unused-parameter', # Is generally annoying and the Ruby headers do it a bunch anyway
  '-Wno-unknown-warning-option', # A bit tautalogical
])

# Support GC.compact on Ruby >=- 2.7
have_func("rb_gc_mark_movable", ["ruby.h"])

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

# Peek into internal Ruby headers
require 'debase/ruby_core_source'
internal_headers = proc {
  have_header("vm_core.h") and have_header("iseq.h") and have_header("version.h")
}


dir_config('ruby')
unless Debase::RubyCoreSource.create_makefile_with_core(internal_headers, "ruby_memprofiler_pprof_ext")
  STDERR.print("Makefile creation failed\n")
  STDERR.print("*************************************************************\n\n")
  STDERR.print("  NOTE: If your headers were not found, try passing\n")
  STDERR.print("        --with-ruby-include=PATH_TO_HEADERS      \n\n")
  STDERR.print("*************************************************************\n\n")
  exit(1)
end

# Append some stuff to the Makefile, to set CFLAGS slightly differently for objects that
# we do not control (the upb stuff)
File.open('Makefile', 'a') do |f|
  upb_objs = Dir["#{$srcdir}/*.upb.c"].map { |f| File.basename(f, ".upb.c") + ".upb.o" }
  f.puts <<~MAKEFILE
    UPB_OBJS=upb.o #{upb_objs.join ' '}
    $(UPB_OBJS): CFLAGS += -Wno-shorten-64-to-32 -Wno-sign-compare -Wno-implicit-fallthrough -Wno-clobbered
  MAKEFILE
end
