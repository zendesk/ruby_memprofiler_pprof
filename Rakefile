require "bundler/gem_tasks"
require "rake/extensiontask"
require "rake/testtask"
require "bump/tasks"
require "private_gem/tasks"

gemspec = Gem::Specification.load("ruby_memprofiler_pprof.gemspec.gemspec")
Rake::ExtensionTask.new do |ext|
  ext.name = "ruby_memprofiler_pprof_ext"
  ext.source_pattern = "*.{c,h}"
  ext.ext_dir = "ext/ruby_memprofiler_pprof"
  ext.lib_dir = "lib/ruby_memprofiler_pprof"
  ext.gem_spec = gemspec
end
Rake::TestTask.new(:test) do |t|
  t.libs << "test"
  t.libs << "lib"
  t.test_files = FileList["test/**/test_*.rb"]
  t.verbose = true
  t.options = "--verbose"
  t.warning = false
end

task default: [:compile]

# These Rake tasks run the protobuf compiler to generate the .upb.c code. These are not run as part of
# the gem install; the generated protobufs are actually checked in. We also check in a complete copy
# of the upb library source and build it when the gem is installed too.

task :proto_compile do

  # Checkout the upb repo
  upb_version = ENV['UPB_VERSION']
  upb_dir = "tmp/#{RUBY_PLATFORM}/upb"
  amalgamate_patch = File.absolute_path "0001_upb_amalgamate.patch"
  mkdir_p upb_dir
  cd upb_dir do
    if File.exist?('.git')
      if !upb_version.nil?
        sh 'git', 'fetch'
        sh 'git', 'checkout', upb_version
        sh 'git', 'am', amalgamate_patch
      end
    else
      sh 'git', 'clone', 'https://github.com/protocolbuffers/upb.git', '.'
      if !upb_version.nil?
        sh 'git', 'checkout', upb_version
      end
      sh 'git', 'am', amalgamate_patch
    end
  end

  # Build protoc-gen-upb
  cd "#{upb_dir}/upbc" do
    sh 'bazel', 'build', 'protoc-gen-upb'
  end

  # Build the amalgamation, then copy it to our source tree
  cd upb_dir do
    sh 'bazel', 'build', 'gen_amalgamation'
  end
  Dir["#{upb_dir}/bazel-bin/upb.{c,h}"].each do |f|
    # For some stoopid reason, the files are created as read-only?
    chmod 0644, f
    # Copy them into place.
    cp f, "ext/ruby_memprofiler_pprof/"
  end

  # Delete & recompile the protobufs
  Dir["ext/ruby_memprofiler_pprof/*.upb.{c,h}"].each { |f| rm_rf f }
  Dir["lib/ruby_memprofiler_pprof/pb/*_pb.rb"].each { |f| rm_rf f }

  protoc_gen_upb = "#{upb_dir}/bazel-bin/upbc/protoc-gen-upb"
  sh 'protoc', '--proto_path=proto', "--plugin=#{protoc_gen_upb}",
    "--upb_out=ext/ruby_memprofiler_pprof", "--ruby_out=test",
    *Dir["proto/*.proto"]

  # We need to hack at the generated protobufs because they #include "upb/stuff.h", but all
  # of our headers are in a single "upb.h"; rewrite.
  # My gsub call here is pretty silly - it will #include "upb.h" a whole bunch of times. However,
  # that should still be fine because upb.h has an include guard.
  port_def = File.read("#{upb_dir}/upb/port_def.inc")
  Dir["ext/ruby_memprofiler_pprof/*.upb.{c,h}"].each do |f|
    old_content = File.read(f)
    old_content.gsub!(/^\s*#\s*include\s+["<]upb\/port_def\.inc.*$/, port_def)
    old_content.gsub!(/^\s*#\s*include\s+["<]upb\/.*$/, '#include "upb.h"')
    File.write(f, old_content)
  end
end
