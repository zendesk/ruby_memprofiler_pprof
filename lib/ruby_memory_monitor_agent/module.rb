module RubyMemoryMonitor
  def self.configure
    raise "Profiling already started! Disable before reconfiguring" if @agent&.running?
    @agent ||= Agent.new

    yield @agent if block_given?

    atfork_in_child &@agent.method(:atfork_in_child)
  end

  def self.enable_profiling!
    @agent.enable_profiling!
  end

  def self.disable_profiling!
    @agent.disable_profiling!
  end
end
