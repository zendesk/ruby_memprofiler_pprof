#ifndef __RUBY_MEMPROFILER_PPROF_H
#define __RUBY_MEMPROFILER_PPROF_H

#include "extconf.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <backtracie.h>
#include <ruby.h>

// UPB header files trip up a BUNCH of -Wshorten-64-to-32
// Also ignore -Wpragmas so that if -Wshorten-64-to-32 isn't present
// (it's a clang only thing), GCC doesn't warn about the unknown warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#include "pprof.upb.h"
#include <upb/upb.h>
#pragma GCC diagnostic pop

// ======== COMPAT DECLARATIONS ========

// For handling differences in ruby versions
#ifndef HAVE_RB_GC_MARK_MOVABLE
#define rb_gc_mark_movable(v) rb_gc_mark(v)
#endif

#ifndef RB_PASS_KEYWORDS
#define RB_SCAN_ARGS_LAST_HASH_KEYWORDS 3
#define rb_scan_args_kw(kw, c, v, s, ...) rb_scan_args(c, v, s, __VA_ARGS__)
#endif

#ifndef HAVE_RB_EXT_RACTOR_SAFE
#define rb_ext_ractor_safe(x)                                                                                          \
  do {                                                                                                                 \
  } while (0)
#endif

// Apparently "I just want a random number, without thinking about whether it's
// threadsafe, without thinking about whether some other part of the process needs
// the global seed to be set to some deterministic value, and without calling into
// the kernel every time" is... too much to ask for.
// BSD has arc4random(3) for this, but for glibc we have to use one of the threadsafe
// RNG's and seed a global instance of it, guarded by a mutex....
// These methods wrap all that rubbish up.
uint32_t mpp_rand();
void mpp_rand_init();

// Wrapper to get monotonic time. Pre-sierra MacOS doesn't have clock_gettime, so we need a wrapper for this.
// (n.b. - I haven't actually _implemented_ a fallback for pre-Sierra, but this is where we'd do it)
// Returns time in nanoseconds.
struct timespec mpp_gettime_monotonic();
int64_t mpp_time_delta_nsec(struct timespec t1, struct timespec t2);

// These declarations just wrap some things from the standard library that should "always succeed", but call
// our assertion macro if they fail to abort the program.
void *mpp_xmalloc(size_t sz);
void *mpp_realloc(void *mem, size_t newsz);
void mpp_free(void *mem);
void mpp_pthread_mutex_lock(pthread_mutex_t *m);
void mpp_pthread_mutex_unlock(pthread_mutex_t *m);
int mpp_pthread_mutex_trylock(pthread_mutex_t *m);
void mpp_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
void mpp_pthread_mutex_destroy(pthread_mutex_t *m);
void mpp_pthread_mutexattr_init(pthread_mutexattr_t *a);
void mpp_pthread_mutexattr_destroy(pthread_mutexattr_t *a);
void mpp_pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
void mpp_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

// Need a handy assertion macro. It would be nice to re-use rb_bug for some of this, but that actually
// requires the GVL (it walks the Ruby stack frames, for one) and we (want to) run some code outside
// the GVL, so this assertion macro has to be threadsafe. So we just implement it in pretty much a
// similar way to how stdlib's assert() works (plus some stuff to prefix the gem name to the abort message
// so users know who is at fault).
#define MPP_ASSERT_STRINGIFY1(x) #x
#define MPP_ASSERT_STRINGIFY2(x) MPP_ASSERT_STRINGIFY1(x)
#define MPP_ASSERT__LINE MPP_ASSERT_STRINGIFY2(__LINE__)
__attribute__((noreturn)) void mpp_assert_fail(const char *msg, const char *assertion, const char *file,
                                               const char *line, const char *fn);
#define MPP_ASSERT_MSG(expr, msg)                                                                                      \
  do {                                                                                                                 \
    if ((expr) == 0) {                                                                                                 \
      mpp_assert_fail((msg), #expr, __FILE__, MPP_ASSERT__LINE, __func__);                                             \
    }                                                                                                                  \
  } while (0)
#define MPP_ASSERT_FAIL(msg) MPP_ASSERT_MSG(0, msg)

// Log a debug message to "somewhere". This could be smarter in future, but for now, this'll do.
// The implementation here does not depend on holding the GVL.
// Will automatically add a trailing newline.
void mpp_log_debug(const char *pattern, ...);

// ======== RUBY HACKS DECLARATIONS ========

// An implementation of rb_gc_disable_no_rest; disables the GC without itself triggering
// the finalisation of the current sweep phase of the GC.
VALUE mpp_rb_gc_disable_no_rest();
// An implementation of rb_obj_memsize_of; tells us how big an object is.
VALUE mpp_rb_obj_memsize_of(VALUE obj);
// Tells us whether the given VALUE is valid enough still for rb_obj_memsize_of to
// work on it.
bool mpp_is_value_still_validish(VALUE obj);
// Is some other thread blocked waiting for the GVL?
bool mpp_is_someone_else_waiting_for_gvl();
// Like rb_ivar_set, but ignore frozen status.
VALUE mpp_rb_ivar_set_ignore_frozen(VALUE obj, ID key, VALUE value);

// ======== SAMPLE DECLARATIONS ========

// The struct mpp_sample is the core type for the data collected by ruby_memprofiler_pprof.

struct mpp_sample {
  // VALUE of the sampled object that was allocated, or Qundef it it's freed.
  VALUE allocated_value_weak;
  size_t allocated_value_objsize;
  size_t frames_count;
  size_t frames_capacity;
  unsigned int flush_epoch;
  raw_location frames[];
};

// Captures a backtrace for a sample using Backtracie. The resulting sample contains VALUES inside
// the raw_location struct whcih need to be marked.
struct mpp_sample *mpp_sample_capture(VALUE allocated_value_weak);
// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample);
// free the sample
void mpp_sample_free(struct mpp_sample *sample);
// Fill in a provided buffer with the name of a frame.
size_t mpp_sample_frame_function_name(struct mpp_sample *sample, int frame_index, char *outbuf, size_t outbuf_len);
// Fill in a provided buffer with the filename of a frame
size_t mpp_sample_frame_file_name(struct mpp_sample *sample, int frame_index, char *outbuf, size_t outbuf_len);
// Get the line number of a frame.
int mpp_sample_frame_line_number(struct mpp_sample *sample, int frame_index);

// ======== PROTO SERIALIZATION ROUTINES ========
struct mpp_pprof_serctx {
  // Defines the allocation routine & memory arena used by this serialisation context. When the ctx
  // is destroyed, we free the entire arena, so no other (protobuf) memory needs to be individually
  // freed.
  upb_alloc allocator;
  upb_Arena *arena;
  // Map of function ID -> function protobuf
  st_table *function_pbs;
  // Map of (function ID, line number) -> location protobufs
  st_table *location_pbs;
  // Map of (function name string ID, file name string ID) -> function ID
  st_table *function_ids;
  // Counter for assigning location IDs
  uint64_t loc_counter;
  // Counter for assigning function IDs
  uint64_t function_id_counter;
  // Map of (string, len) -> string table index
  st_table *strings;
  // Counter for assigning string table indexes.
  int strings_counter;
  // The protobuf representation we are building up.
  perftools_profiles_Profile *profile_proto;

  // A buffer which, if non-NULL, points into the upb arena and can be stolen for interning strings.
  char *scratch_buffer;
  size_t scratch_buffer_strlen;
  size_t scratch_buffer_capa;

  // Toggle to interrupt (toggled from Ruby's GVL unblocking function)
  uint8_t interrupt;
};

struct mpp_pprof_serctx *mpp_pprof_serctx_new(char *errbuf, size_t errbuflen);
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx);
int mpp_pprof_serctx_add_sample(struct mpp_pprof_serctx *ctx, struct mpp_sample *sample, char *errbuf,
                                size_t errbuflen);
int mpp_pprof_serctx_serialize(struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf,
                               size_t errbuflen);

// ======== COLLECTOR RUBY CLASS ========
void mpp_setup_collector_class();

#endif
