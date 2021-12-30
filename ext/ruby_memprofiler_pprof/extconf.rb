require "mkmf"

$CFLAGS += " -D_GNU_SOURCE -std=gnu11"
have_func("rb_gc_mark_movable", ["ruby.h"])
has_arc4random = have_func("arc4random", ["stdlib.h"])
has_mrand48_r = have_func("mrand48_r", ["stdlib.h"])
has_getentropy = have_func("getentropy", ["sys/random.h"])
if !has_arc4random && !(has_mrand48_r && has_getentropy)
    abort "Need either arc4random (BSD) or mrand48_r/getentropy (GNU)"
end
create_makefile("ruby_memprofiler_pprof_ext")
