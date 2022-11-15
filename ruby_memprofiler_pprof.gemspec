# frozen_string_literal: true

require_relative "./lib/ruby_memprofiler_pprof/version"

Gem::Specification.new do |spec|
  spec.name = "ruby_memprofiler_pprof"
  spec.version = MemprofilerPprof::VERSION
  spec.authors = ["KJ Tsanaktsidis"]
  spec.email = ["ktsanaktsidis@zendesk.com"]
  spec.summary = "Ruby pprof memproy profiler"
  spec.description = "Generates pprof profiles of ruby memory usage"
  spec.homepage = "https://github.com/zendesk/ruby_memprofiler_pprof"
  spec.license = "Apache-2.0"

  spec.files = Dir.glob("{ext,lib,libexec}/**/*").reject { |f| %w[.so .bundle].include? File.extname(f) }
  spec.extensions = ["ext/ruby_memprofiler_pprof_ext/extconf.rb"]
  spec.bindir = "libexec"
  spec.executables = %w[ruby_memprofiler_pprof_profile]

  spec.required_ruby_version = ">= 2.6.0"

  # This incredibly tight pin on backtracie is required, because we're calling its C internals via
  # a header file we vendored. Upgrading this requires copoying a new version of the header file.
  spec.add_dependency "backtracie", "= 1.0.0"
end
