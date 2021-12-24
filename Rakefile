require "bundler/gem_tasks"
require "rake/extensiontask"
require "rake/testtask"
require "bump/tasks"
require "private_gem/tasks"

gemspec = Gem::Specification.load("ruby_memory_monitor_agent.gemspec.gemspec")
Rake::ExtensionTask.new do |ext|
  ext.name = "ruby_memory_monitor_agent_ext"
  ext.source_pattern = "*.{c,h}"
  ext.ext_dir = "ext/ruby_memory_monitor_agent"
  ext.lib_dir = "lib/ruby_memory_monitor_agent"
  ext.gem_spec = gemspec
end
Rake::TestTask.new(:test) {}

task default: [:compile]
