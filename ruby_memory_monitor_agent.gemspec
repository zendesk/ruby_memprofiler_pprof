require "./lib/ruby_memory_monitor_agent/version"

Gem::Specification.new do |spec|
  spec.name = "ruby_memory_monitor_agent"
  spec.version = RubyMemoryMonitorAgent::VERSION
  spec.authors = ["KJ Tsanaktsidis"]
  spec.email = ["ktsanaktsidis@zendesk.com"]
  spec.summary = "Ruby memory monitor agent"
  spec.description = "Submits metrics to ruby_memory_monitor"
  spec.homepage = "https://github.com/zendesk/ruby_memory_monitor_agent"
  spec.license = "Copyright Zendesk. All Rights Reserved"

  spec.metadata["allowed_push_host"] = "https://zdrepo.jfrog.io/zdrepo/api/gems/gems-local/"

  spec.files = Dir.glob("{ext,lib}/**/*")
  spec.extensions = ["ext/ruby_memory_monitor_agent/extconf.rb"]

  spec.required_ruby_version = ">= 2.6.8"
end
