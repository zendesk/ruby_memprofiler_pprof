require "mkmf"

append_cflags([
  '-g', '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra',
  '-fvisibility=hidden', # Make sure our upb symbols don't clobber anybody elses
  '-Wno-unused-parameter', # Is generally annoying and the Ruby headers do it a bunch anyway
  '-Wno-unknown-warning-option', # A bit tautalogical
  '-Wno-declaration-after-statement', # Somehow, this is in CFLAGS somewhere? Who needs it?
  '-Wno-suggest-attribute=noreturn', # I will survive without this too.
])

if ENV['WERROR'] == 'true'
  append_cflags(['-Werror'])
end

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

# Peek into internal Ruby headers
require 'debase/ruby_core_source'
internal_headers = proc {
  have_header("vm_core.h") and
    have_header("iseq.h", ["vm_core.h"]) and
    have_header("version.h")
}

dir_config('ruby')
extname = "ruby_memprofiler_pprof/ruby_memprofiler_pprof_ext"
unless Debase::RubyCoreSource.create_makefile_with_core(internal_headers, extname)
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
# So, the safest way to deal with this lack of API compatibility is to just embed it. This part of the
# script below hacks at the generated makefile that extconf.rb generated, to add targets to build libupb.a
# (which would _normally_ be built by Bazel, but we don't want to make installers of our gem have that
# either; hence, these hand-written Makefile rules below)
#
# This strategy of simply embedding the upb source is endorsed by the maintainers here:
# https://github.com/protocolbuffers/upb/pull/480
File.open('Makefile', 'a') do |f|
  f.puts <<~MAKEFILE
    UPB_OBJS += $(addprefix vendor/upb/upb/,decode.o encode.o msg.o table.o upb.o)
    UPB_OBJS += vendor/upb/third_party/utf8_range/utf8_range.o
    UPB_HDRS += $(wildcard $(srcdir)/vendor/upb/upb/*.h)
    UPB_HDRS += $(wildcard $(srcdir)/vendor/upb/third_party/utf8_range/*.h)
    UPB_LIB := vendor/upb/libupb.a
    UPB_CFLAGS += -Wno-shorten-64-to-32 -Wno-sign-compare -Wno-implicit-fallthrough
    UPB_CFLAGS += -Wno-clobbered -Wno-maybe-uninitialized

    $(UPB_OBJS): CFLAGS += $(UPB_CFLAGS)
    $(UPB_OBJS): $(UPB_HDRS)
    $(UPB_OBJS): %.o : $(srcdir)/%.c
    \t$(ECHO) compiling $(<)
    \t$(Q) $(MAKEDIRS) $(@D)
    \t$(Q) $(CC) $(INCFLAGS) $(CPPFLAGS) $(CFLAGS) $(COUTFLAG)$@ -c $(CSRCFLAG)$<
    $(UPB_LIB): $(UPB_OBJS)
    \t$(ECHO) linking static library $@
    \t$(Q) $(AR) rcs $@ $^

    $(TARGET_SO): $(UPB_LIB)
    $(TARGET_SO): LOCAL_LIBS += $(UPB_LIB)
    CFLAGS += -I$(srcdir)/vendor/upb
    %.upb.o: CFLAGS += $(UPB_CFLAGS)
  MAKEFILE

  # Make Make automatically verbose if specified
  if ENV['VERBOSE'] == 'true'
    f.puts "V = 1"
  end
end
