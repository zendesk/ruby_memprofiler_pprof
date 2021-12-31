#ifndef __RMPP_COMPAT_H
#define __RMPP_COMPAT_H

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

// Implemented in compat.c
uint32_t rmm_pprof_rand();
void rmm_pprof_rand_init();
void *rmmp_xcalloc(size_t sz);
void rmmp_free(void *mem);

void rmmp_pthread_mutex_lock(pthread_mutex_t *m);
void rmmp_pthread_mutex_unlock(pthread_mutex_t *m);
int rmmp_pthread_mutex_trylock(pthread_mutex_t *m);
void rmmp_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
void rmmp_pthread_mutex_destroy(pthread_mutex_t *m);

#endif
