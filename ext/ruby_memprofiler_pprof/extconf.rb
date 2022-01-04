require "mkmf"

# Need -Wno-shorten-64-to-32 -Wno-sign-compare because the ubp generated protos warn on thse
$CFLAGS << " -g -D_GNU_SOURCE -DNDEBUG -std=gnu11 -Wno-shorten-64-to-32 -Wno-sign-compare"
# Support GC.compact on Ruby >=- 2.7
have_func("rb_gc_mark_movable", ["ruby.h"])

# Find a random number generator API that doesn't have global state.
has_arc4random = have_func("arc4random", ["stdlib.h"])
has_mrand48_r = have_func("mrand48_r", ["stdlib.h"])
has_getentropy = have_func("getentropy", ["sys/random.h"])
if !has_arc4random && !(has_mrand48_r && has_getentropy)
  abort "Need either arc4random (BSD) or mrand48_r/getentropy (GNU)"
end

# Need protobuf
has_pb_lib = have_library("protobuf")
raise "Protobuf headers & library are required!" unless has_pb_lib

# Need zlib
has_zlib_headers = have_header("zlib.h")
has_zlib_lib = have_library("z")
raise "Zlib headers & library are required!" unless has_zlib_headers && has_zlib_lib

# Peek into internal Ruby headers
require 'debase/ruby_core_source'
internal_headers = proc {
  have_header("vm_core.h") and have_header("iseq.h") and have_header("version.h")
}

# Link against UPB in our source tree
$LDFLAGS << ' upb/libupb.a'
$CFLAGS << ' -I$(srcdir)/upb'

dir_config('ruby')
unless Debase::RubyCoreSource.create_makefile_with_core(internal_headers, "ruby_memprofiler_pprof_ext")
  STDERR.print("Makefile creation failed\n")
  STDERR.print("*************************************************************\n\n")
  STDERR.print("  NOTE: If your headers were not found, try passing\n")
  STDERR.print("        --with-ruby-include=PATH_TO_HEADERS      \n\n")
  STDERR.print("*************************************************************\n\n")
  exit(1)
end

# We embed an entire copy of upb (https://github.com/protocolbuffers/upb) in our source tree, to compile
# it with us and avoid requiring our users to have protoc/the runtime library to install the gem. This is
# important because there is no release or anything like that of UPB; in fact, it contains this warning:
#
#     While upb offers a C API, the C API & ABI are not stable. For this reason, upb is not generally
#     offered as a C library for direct consumption, and there are no releases.
#
# So, the safest way to deal with this lack of API comaptability is to just embed it. This part of the
# script below hacks at the generated makefile that extconf.rb generated, to add targets to build libupb.a
# (which would _normally_ be built by Bazel, but we don't want to make installers of our gem have that
# either; hence, these hand-written Makefile rules below)
File.open('Makefile', 'a') do |f|
  # Hack at the generated makefile to compile the third party upb library too.
  f.puts <<~MAKEFILE
    UPBCFLAGS = $(INCFLAGS) $(CPPFLAGS) $(CFLAGS)
  MAKEFILE
  %w[decode encode msg table upb].each do |m|
    f.puts <<~MAKEFILE
      upb/#{m}.o:
      \t$(ECHO) compiling $(srcdir)/upb/upb/#{m}.c
      \t$(Q) $(MAKEDIRS) upb
      \t$(Q) $(CC) $(UPBCFLAGS) -c -o upb/#{m}.o $(srcdir)/upb/upb/#{m}.c
    MAKEFILE
  end
  # The utf8 library used by upb is in a different directory
  f.puts <<~MAKEFILE
    upb/utf8_range.o:
    \t$(ECHO) compiling $(srcdir)/upb/third_party/utf8_range/utf8_range.c
    \t$(Q) $(MAKEDIRS) upb
    \t$(Q) $(CC) $(UPBCFLAGS) -c -o upb/utf8_range.o $(srcdir)/upb/third_party/utf8_range/utf8_range.c
  MAKEFILE
  # Compile a static library
  f.puts <<~MAKEFILE
    upb/libupb.a: upb/decode.o upb/encode.o upb/msg.o upb/table.o upb/upb.o upb/utf8_range.o
    \t$(ECHO) making shared library upb/upb.a
    \t$(Q) $(AR) rcs upb/libupb.a upb/decode.o upb/encode.o upb/msg.o upb/table.o upb/upb.o upb/utf8_range.o
  MAKEFILE
  #  Make it a dependency of our main target
  f.puts <<~MAKEFILE
    $(TARGET_SO): upb/libupb.a
  MAKEFILE
  # Also handle cleaning
  f.puts <<~MAKEFILE
    .PHONY: upb_clean
    upb_clean:
    \t$(Q) $(RM) -Rf upb/
    clean: upb_clean
  MAKEFILE
end
