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

# Peek into internal Ruby headers
require 'debase/ruby_core_source'
internal_headers = proc {
  have_header("vm_core.h") and
    have_header("iseq.h", ["vm_core.h"]) and
    have_header("version.h")
}

# Set our cflags up _only after_ we have run all the existence checks above; otherwise
# stuff like -Werror can break the test programs.
append_cflags([
  '-g', # Compile with debug info
  '-D_GNU_SOURCE', '-std=gnu11', # Use GNU C extensions (e.g. we use this for atomics)
  '-fvisibility=hidden', # Make sure our upb symbols don't clobber any others from other exts
])
append_cflags(['-Wall, -Wextra']) # Enable all the warnings
if ENV['WERROR'] == 'true'
  append_cflags(['-Werror']) # Enable werror on CI
end
# These diagnostics are not very interesting at all, just disable them.
append_cflags([
  '-Wno-unused-parameter',
  '-Wno-declaration-after-statement',
  '-Wno-suggest-attribute=noreturn',
])
# These diagnostics are interesting, but we can't -Werror them because upb commits some violations.
# Keep the warnings on though so we don't add them to our own code.
append_cflags([
  '-Wno-error=sign-compare',
  '-Wno-error=clobbered',
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
  "third_party/utf8_range/range2-sse.c",
].map { |f| File.join($srcdir, "vendor/upb", f) }
$VPATH << "$(srcdir)/vendor/upb/upb"
$VPATH << "$(srcdir)/vendor/upb/third_party/utf8_range"
$INCFLAGS << " -I#{File.join($srcdir, "vendor/upb")}"

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
