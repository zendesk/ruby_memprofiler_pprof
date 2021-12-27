#ifndef __RMMA_COMPAT_H
#define __RMMA_COMPAT_H

#include <ruby.h>

#ifndef HAVE_RB_GC_MARK_MOVABLE
#define rb_gc_mark_moveable(v) rb_gc_mark(v)
#endif

#ifndef RB_PASS_KEYWORDS
#define rb_scan_args_kw(kw, c, v, s, ...) rb_scan_args(c, v, s, __VA_ARGS__)
#endif

#ifndef HAVE_RB_EXT_RACTOR_SAFE
#define rb_ext_ractor_safe(x) do {} while (0)
#endif

#endif
