# We need to patch fork to re-create the profile submission thread after fork.
# In newer rubies, this is easy (see https://bugs.ruby-lang.org/issues/17795), but in older rubies, it's a bit
# trickier (see https://github.com/DataDog/dd-trace-rb/blob/793946146b4709289cfd459f3b68e8227a9f5fa7/lib/ddtrace/profiling/ext/forking.rb)

module RubyMemoryMonitor
  def self.atfork_in_child(&blk)
    if ::Process.respond_to?(:_fork)
      # New way
      m = Module.new do
        define_singleton_method(:_fork) do
          pid = super()
          blk.call if pid == 0
          pid
        end
      end
      ::Process.prepend(m)
    else
      # Old way
      m = Module.new do
        define_singleton_method(:fork) do |*args, **kwargs, &fork_blk|
          pid = super(*args, **kwargs, &fork_blk)
          blk.call if pid == 0
          pid
        end
      end
      ::Process.prepend(m)
      ::Kernel.prepend(m)
      TOPLEVEL_BINDING.receiver.class.prepend(m)
    end
  end
end
