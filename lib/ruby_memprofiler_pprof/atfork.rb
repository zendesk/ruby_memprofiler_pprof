# frozen_string_literal: true

# A mechanism for adding atfork handlers, so that background profile flushing threads
# can re-create themselves in forked children.
# Mostly ripped off from the Datadog one here:
# https://github.com/DataDog/dd-trace-rb/blob/master/lib/datadog/profiling/ext/forking.rb

module MemprofilerPprof
  module Atfork
    class Handler
      attr_accessor :block
      attr_accessor :stage

      def call
        block.call
      end

      def remove!
        Atfork.at_fork_handlers.reject! { |h| h === self }
      end
    end

    module_function

    def at_fork_handlers
      @@at_fork_handlers ||= []
    end

    def at_fork(stage = :prepare, &block)
      handler = Handler.new.tap do |h|
        h.block = block
        h.stage = stage
      end
      at_fork_handlers << handler
      handler
    end

    def fork
      # If a block is provided, it must be wrapped to trigger callbacks.
      child_block = if block_given?
        proc do
          # Trigger :child callback
          at_fork_handlers.select { |h| h.stage == :child }.each(&:call)

          # Invoke original block
          yield
        end
      end

      # Trigger :prepare callback
      at_fork_handlers.select { |h| h.stage == :prepare }.each(&:call)

      # Start fork
      # If a block is provided, use the wrapped version.
      result = child_block.nil? ? super : super(&child_block)

      # Trigger correct callbacks depending on whether we're in the parent or child.
      # If we're in the fork, result = nil: trigger child callbacks.
      # If we're in the parent, result = fork PID: trigger parent callbacks.
      if result.nil?
        # Trigger :child callback
        at_fork_handlers.select { |h| h.stage == :child }.each(&:call)
      else
        # Trigger :parent callback
        at_fork_handlers.select { |h| h.stage == :parent }.each(&:call)
      end
      # rubocop:enable Style/IfInsideElse

      # Return PID from #fork
      result
    end
  end
end

::Process.singleton_class.prepend(MemprofilerPprof::Atfork)
::Kernel.singleton_class.prepend(MemprofilerPprof::Atfork)
TOPLEVEL_BINDING.receiver.class.prepend(MemprofilerPprof::Atfork)
