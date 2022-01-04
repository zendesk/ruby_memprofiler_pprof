#include <ruby.h>
#include "ruby_memprofiler_pprof.h"

VALUE mMemprofilerPprof;

// This should be the only symbol actually visible to Ruby
__attribute__(( visibility("default" )))
void Init_ruby_memprofiler_pprof_ext() {
    mpp_rand_init();
    rb_ext_ractor_safe(true);
    mMemprofilerPprof = rb_define_module("MemprofilerPprof");
    mpp_setup_collector_class();
}
