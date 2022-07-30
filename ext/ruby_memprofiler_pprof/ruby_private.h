#ifndef __RUBY_PRIVATE_H
#define __RUBY_PRIVATE_H

#include "extconf.h"
#include RUBY_MJIT_HEADER

#include <ruby.h>
#include <ruby/re.h>

// Default this to zero unless the MJIT header already has a value
#ifndef GC_DEBUG_STRESS_TO_CLASS
#define GC_DEBUG_STRESS_TO_CLASS 0
#endif

// This is the correct default for GC_ENABLE_INCREMENTAL_MARK
#ifndef GC_ENABLE_INCREMENTAL_MARK
#define GC_ENABLE_INCREMENTAL_MARK USE_RINCGC
#endif

// Prototype for functions that are exposed with external linkage, but not in
// the public headers.
size_t rb_obj_memsize_of(VALUE obj);

// Bring in an appropriate version of the rb_objspace structs; this will include
// one of the files under ruby_private/ depending on version.
#include "gc_private.h"

#define CLASS_OR_MODULE_P(obj)                                                                                         \
  (!SPECIAL_CONST_P(obj) && (BUILTIN_TYPE(obj) == T_CLASS || BUILTIN_TYPE(obj) == T_MODULE))

#endif
