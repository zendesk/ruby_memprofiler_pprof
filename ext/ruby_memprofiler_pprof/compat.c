#include <ruby.h>
#include <pthread.h>
#include <stdlib.h>
#include "compat.h"


#if defined(HAVE_ARC4RANDOM)
uint32_t rmm_pprof_rand() {
    return arc4random();
}
void rmm_pprof_rand_init() {

}
#elif defined(HAVE_MRAND48_R) && defined(HAVE_GETENTROPY)
// Why? Why does glibc make it so darn difficult to just get random numbers in a way
// that isn't racy across different threads and doesn't depend on global state?
#include <sys/random.h>

static pthread_mutex_t rmm_pprof_rand_lock;
static struct drand48_data rmm_pprof_rand_buffer;

uint32_t rmm_pprof_rand() {
    pthread_mutex_lock(&rmm_pprof_rand_lock);
    long int result;
    int ret = mrand48_r(&rmm_pprof_rand_buffer, &result);
    if (ret < 0) {
        rb_bug("mrand48_r returned -1?");
    }
    pthread_mutex_unlock(&rmm_pprof_rand_lock);
    return (uint32_t)result;
}

void rmm_pprof_rand_init() {
    pthread_mutex_init(&rmm_pprof_rand_lock, NULL);
    memset(&rmm_pprof_rand_buffer, 0, sizeof(struct drand48_data));
    unsigned short int seedbuf[3];
    int ret;
    ret = getentropy(seedbuf, sizeof(seedbuf));
    if (ret == -1) {
        rb_bug("getentropy returned -1?");
    }
    ret = seed48_r(seedbuf, &rmm_pprof_rand_buffer);
    if (ret < 0) {
        rb_bug("seed48_r returned -1?");
    }
}
#else
#error "No suitable RNG implementation"
#endif
