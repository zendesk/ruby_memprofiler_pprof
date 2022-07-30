require "bundler/gem_tasks"
require "rake/extensiontask"
require "rake/testtask"
require "bump/tasks"

# Compile verbosely if specified.
ENV["MAKE"] = "make V=1" if ENV["VERBOSE"] == "true"

Rake::ExtensionTask.new("ruby_memprofiler_pprof_ext")

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
  upb_dir = "ext/ruby_memprofiler_pprof/vendor/upb"
  # Build protoc-gen-upb
  cd "#{upb_dir}/upbc" do
    sh "bazel", "build", "protoc-gen-upb"
  end

  # Delete & recompile the protobufs
  Dir["ext/ruby_memprofiler_pprof/*.upb.{c,h}"].each { |f| rm_rf f }
  Dir["lib/ruby_memprofiler_pprof/pb/*_pb.rb"].each { |f| rm_rf f }

  protoc_gen_upb = "#{upb_dir}/bazel-bin/upbc/protoc-gen-upb"
  sh "protoc", "--proto_path=proto", "--plugin=#{protoc_gen_upb}",
    "--upb_out=ext/ruby_memprofiler_pprof", "--ruby_out=test",
    *Dir["proto/*.proto"]
end
