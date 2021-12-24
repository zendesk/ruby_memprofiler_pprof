require "mkmf"

$CFLAGS += " -D_GNU_SOURCE -std=gnu11"
create_makefile("ruby_memory_monitor_agent_ext")
