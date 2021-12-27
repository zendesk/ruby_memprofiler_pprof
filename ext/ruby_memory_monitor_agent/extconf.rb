require "mkmf"

$CFLAGS += " -D_GNU_SOURCE -std=gnu11"
have_func("rb_gc_mark_movable", ["ruby.h"])
create_makefile("ruby_memory_monitor_agent_ext")
