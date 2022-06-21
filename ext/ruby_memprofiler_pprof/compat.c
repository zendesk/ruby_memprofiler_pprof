#include <errno.h>
#include <ruby.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>

// for GET_VM
#include <vm_core.h>

#include "ruby_memprofiler_pprof.h"

static bool malloc_allocated_size_enabled;

void mpp_compat_init() {
    if (rb_respond_to(rb_mGC, rb_intern("malloc_allocated_size"))) {
        malloc_allocated_size_enabled = true;
    } else {
        malloc_allocated_size_enabled = false;
    }

    mpp_rand_init();
}

// We need to set the dont_gc flag directly on the objspace, WITHOUT running gc_rest().
// This is exactly what rb_gc_disable_no_rest() does, but that's not exported (it's in
// the internal header file, it's not static, but it's not exported with the right
// symbol visibility, so we can't call it).
// 
// We _can_ get a pointer to the rb_objspace struct (it's available on the vm struct),
// so to set the dont_gc flag, we "just" need to know what offset to poke it at.
//
// The "best" way I can think of to do that is to define a few versions of the rb_objspace
// struct here, copied out of gc.c from various versions, and switch between them. The flags
// are thankfully pretty close to the top of the struct, and thankfully dont_gc is the third
// flag, so this is actually identical for all versions of Ruby >= 2.6 <= 3.1. Later flags
// _do_ differ in position (and depending on various #define's too), but we don't need them.


struct rb_objspace_head_with_allocated_size {
    struct {
        size_t limit;
        size_t increase;
        size_t allocated_size;
        size_t allocations;
    } malloc_params;

    struct {
        unsigned int mode : 2;
        unsigned int immediate_sweep : 1;
        unsigned int dont_gc : 1;
        // Other flags follow, but we ignore them.
    } flags;

    // Other struct members follow, but we ignore them.
};

struct rb_objspace_head_without_allocated_size {
    struct {
        size_t limit;
        size_t increase;
    } malloc_params;

    struct {
        unsigned int mode : 2;
        unsigned int immediate_sweep : 1;
        unsigned int dont_gc : 1;
    } flags;

    // Other struct members follow, but we ignore them.
};

VALUE mpp_rb_gc_disable_no_rest() {
    void *objspace = (void *)(GET_VM()->objspace);
    int old_dont_gc;
    if (malloc_allocated_size_enabled) {
        old_dont_gc = ((struct rb_objspace_head_with_allocated_size *) objspace)->flags.dont_gc;
        ((struct rb_objspace_head_with_allocated_size *) objspace)->flags.dont_gc = 1;
    } else {
        old_dont_gc = ((struct rb_objspace_head_without_allocated_size *) objspace)->flags.dont_gc;
        ((struct rb_objspace_head_without_allocated_size *) objspace)->flags.dont_gc = 1;
    }

    return old_dont_gc ? Qtrue : Qfalse;
}

bool mpp_is_value_still_validish(VALUE obj) {
    int type = RB_BUILTIN_TYPE(obj);
    // do NOT return true for T_NODE; rb_obj_memsize_of() can't handle it.
    switch (type) {
    case T_MODULE:
    case T_CLASS:
    case T_ICLASS:
    case T_STRING:
    case T_ARRAY:
    case T_HASH:
    case T_REGEXP:
    case T_DATA:
    case T_MATCH:
    case T_FILE:
    case T_RATIONAL:
    case T_COMPLEX:
    case T_IMEMO:
    case T_FLOAT:
    case T_SYMBOL:
    case T_BIGNUM:
    case T_STRUCT:
        return true;
    }
    return false;
}

#if defined(HAVE_ARC4RANDOM)
uint32_t mpp_rand() {
    return arc4random();
}
void mpp_rand_init() {

}
#elif defined(HAVE_MRAND48_R) && defined(HAVE_GETENTROPY)
// Why? Why does glibc make it so darn difficult to just get random numbers in a way
// that isn't racy across different threads and doesn't depend on global state?
#include <sys/random.h>

static pthread_mutex_t mpp_rand_lock;
static struct drand48_data mpp_rand_buffer;

uint32_t mpp_rand() {
    pthread_mutex_lock(&mpp_rand_lock);
    long int result;
    int ret = mrand48_r(&mpp_rand_buffer, &result);
    if (ret < 0) {
        rb_bug("mrand48_r returned -1?");
    }
    pthread_mutex_unlock(&mpp_rand_lock);
    return (uint32_t)result;
}

void mpp_rand_init() {
    pthread_mutex_init(&mpp_rand_lock, NULL);
    memset(&mpp_rand_buffer, 0, sizeof(struct drand48_data));
    unsigned short int seedbuf[3];
    int ret;
    ret = getentropy(seedbuf, sizeof(seedbuf));
    if (ret == -1) {
        rb_bug("getentropy returned -1?");
    }
    ret = seed48_r(seedbuf, &mpp_rand_buffer);
    if (ret < 0) {
        rb_bug("seed48_r returned -1?");
    }
}
#else
#error "No suitable RNG implementation"
#endif

void *mpp_xcalloc(size_t sz) {
    void *mem = mpp_xmalloc(sz);
    memset(mem, 0, sz);
    return mem;
}

void *mpp_xmalloc(size_t sz) {
    void *mem = ruby_xmalloc(sz);
    if (!mem) {
        MPP_ASSERT_FAIL("failed to allocate memory in ruby_memprofiler_pprof gem");
    }
    return mem;
}

void mpp_free(void *mem) {
    ruby_xfree(mem);
}

void *mpp_realloc(void *mem, size_t newsz) {
    void *newmem = ruby_xrealloc(mem, newsz);
    if (!newmem) {
        MPP_ASSERT_FAIL("failed to allocate memory in ruby_memprofiler_pprof gem");
    }
    return newmem;
}

void mpp_pthread_mutex_lock(pthread_mutex_t *m) {
    if (pthread_mutex_lock(m) != 0) {
        MPP_ASSERT_FAIL("failed to lock mutex in ruby_memprofiler_pprof gem");
    }
}

void mpp_pthread_mutex_unlock(pthread_mutex_t *m) {
    if (pthread_mutex_unlock(m) != 0) {
        MPP_ASSERT_FAIL("failed to unlock mutex in ruby_memprofiler_pprof gem");
    }
}

int mpp_pthread_mutex_trylock(pthread_mutex_t *m) {
    int r = pthread_mutex_trylock(m);
    if (r != 0 && r != EBUSY) {
        MPP_ASSERT_FAIL("failed to trylock mutex in ruby_memprofiler_pprof gem");
    }
    return r;
}

void mpp_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    if (pthread_mutex_init(m, attr) != 0) {
        MPP_ASSERT_FAIL("failed to init mutex in ruby_memprofiler_pprof gem");
    }
}

void mpp_pthread_mutex_destroy(pthread_mutex_t *m) {
    if (pthread_mutex_destroy(m) != 0) {
        MPP_ASSERT_FAIL("failed to destroy mutex in ruby_memprofiler_pprof gem");
    }
}

void mpp_pthread_mutexattr_init(pthread_mutexattr_t *a) {
    if (pthread_mutexattr_init(a) != 0) {
        MPP_ASSERT_FAIL("failed to init pthread_mutexattr in ruby_memprofiler_pprof gem");
    }
}

void mpp_pthread_mutexattr_destroy(pthread_mutexattr_t *a) {
    if (pthread_mutexattr_destroy(a) != 0) {
        MPP_ASSERT_FAIL("failed to destroy pthread_mutexattr in ruby_memprofiler_pprof gem");
    }
}

void mpp_pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
    if (pthread_mutexattr_settype(a, type) != 0) {
        MPP_ASSERT_FAIL("failed to set type on pthread_mutexattr in ruby_memprofiler_pprof gem");
    }
}

void mpp_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    if (pthread_atfork(prepare, parent, child) != 0) {
        MPP_ASSERT_FAIL("pthread_atfork failed in ruby_memprofiler_pprof gem");
    }
}

__attribute__ ((noreturn))
void mpp_assert_fail(
    const char *msg, const char *assertion, const char *file, const char *line, const char *fn
) {
    char final_msg_buf[1024];
    int32_t final_msg_ix = 0;
    int32_t final_msg_remaining = sizeof(final_msg_buf) - 2; // -1 for null terminator, -1 for trailing newline.

#define FINAL_MSG_BUF_APPEND(str)                                                               \
    do {                                                                                        \
        int32_t seg_len = (int32_t)strlen(str);                                                 \
        int32_t copy_n = (seg_len > final_msg_remaining) ? final_msg_remaining : seg_len;       \
        memcpy(final_msg_buf + final_msg_ix, str, copy_n);                                      \
        final_msg_ix += seg_len;                                                                \
        final_msg_remaining -= seg_len;                                                         \
    } while (0)

    FINAL_MSG_BUF_APPEND("assertion failure in ruby_memprofiler_pprof gem: ");
    FINAL_MSG_BUF_APPEND(msg);
    FINAL_MSG_BUF_APPEND(" (");
    FINAL_MSG_BUF_APPEND(assertion);
    FINAL_MSG_BUF_APPEND("; at ");
    FINAL_MSG_BUF_APPEND(file);
    FINAL_MSG_BUF_APPEND(":");
    FINAL_MSG_BUF_APPEND(line);
    FINAL_MSG_BUF_APPEND(" ");
    FINAL_MSG_BUF_APPEND(fn);
    FINAL_MSG_BUF_APPEND(")");

    // We're guaranteed to have left enough space for this because we started final_msg_remaining off
    // with sizeof(buf) - 2.
    final_msg_remaining += 2;
    FINAL_MSG_BUF_APPEND("\n\0");

    __attribute__((unused)) size_t r =
        write(STDERR_FILENO, final_msg_buf, final_msg_ix + 1);
    abort();
}

void mpp_log_debug(const char *pattern, ...) {
    va_list args;
    va_start(args, pattern);
    fprintf(stderr, "ruby_memprofiler_pprof gem: ");
    vfprintf(stderr, pattern, args);
    fprintf(stderr, "\n");
    va_end(args);
}
