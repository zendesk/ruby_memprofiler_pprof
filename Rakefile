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
Rake::TestTask.new(:test) {}

task default: [:compile]

# These Rake tasks run the protobuf compiler to generate the .upb.c code. These are not run as part of
# the gem install; the generated protobufs are actually checked in.
PROTOC_GEN_UPB = 'ext/ruby_memprofiler_pprof/upb/bazel-bin/upbc/protoc-gen-upb'
PROTO_IN = Rake::FileList[
  "proto/pprof.proto"
]
PROTO_OUT = Rake::FileList[
  'ext/ruby_memprofiler_pprof/pprof.upb.c',
  'ext/ruby_memprofiler_pprof/pprof.upb.h',
  'lib/ruby_memprofiler_pprof/pb/pprof_pb.rb',
]
file PROTOC_GEN_UPB do
  cd 'ext/ruby_memprofiler_pprof/upb/upbc' do
    sh 'bazel', 'build', 'protoc-gen-upb'
  end
end

PROTO_OUT.each do |f|
  if File.extname(f) == ".h"
    file f => (File.dirname(f) + "/" + File.basename(f).gsub(/\.h$/, '.c'))
  elsif File.extname(f) == ".c"
    proto_file = "proto/" + File.basename(f).gsub(/\.upb\.c$/, ".proto")
    file f => [PROTOC_GEN_UPB, proto_file] do
      sh 'protoc', '--proto_path=proto', "--plugin=#{PROTOC_GEN_UPB}",
        "--upb_out=ext/ruby_memprofiler_pprof", proto_file
    end
  elsif File.extname(f) == ".rb"
    proto_file = "proto/" + File.basename(f).gsub(/_pb\.rb$/, ".proto")
    file f => [proto_file] do
      sh 'protoc', '--proto_path=proto', '--ruby_out=lib/ruby_memprofiler_pprof/pb', proto_file
    end
  end
end

task :protoc => PROTO_OUT

task :clean_protoc do
  PROTO_OUT.each do |f|
    rm_f f
  end
end
