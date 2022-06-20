#include <ruby.h>
#include "ruby_memprofiler_pprof.h"

static int memoizer_update_add(st_data_t *key, st_data_t *value, st_data_t ctxarg, int exists);
static int memoizer_update_delete(st_data_t *key, st_data_t *value, st_data_t ctxarg, int exists);
static int memoizer_each_mark(st_data_t key, st_data_t value, st_data_t ctxarg);
#ifdef HAVE_RB_GC_MARK_MOVABLE
static int memoizer_each_compact(st_data_t key, st_data_t value, st_data_t ctxarg);
#endif

struct mpp_mark_memoizer *mpp_mark_memoizer_new() {
    struct mpp_mark_memoizer *memo = mpp_xmalloc(sizeof(struct mpp_mark_memoizer));
    memo->table = st_init_numtable();
    return memo;
}

void mpp_mark_memoizer_destroy(struct mpp_mark_memoizer *memo) {
    st_free_table(memo->table);
    mpp_free(memo);
}

unsigned int mpp_mark_memoizer_add(struct mpp_mark_memoizer *memo, VALUE value) {
    unsigned int current_refcount;
    st_update(memo->table, (st_data_t)value, memoizer_update_add, (st_data_t)&current_refcount);
    return current_refcount;
}

static int memoizer_update_add(st_data_t *key, st_data_t *value, st_data_t ctxarg, int exists) {
    if (!exists) {
        *value = 0;
    }
    (*value)++;
    *((unsigned int *)ctxarg) = (unsigned int)*value;
    return ST_CONTINUE;
}

unsigned int mpp_mark_memoizer_delete(struct mpp_mark_memoizer *memo, VALUE value) {
    unsigned int current_refcount;
    st_update(memo->table, (st_data_t)value, memoizer_update_delete, (st_data_t)&current_refcount);
    return current_refcount;
}

static int memoizer_update_delete(st_data_t *key, st_data_t *value, st_data_t ctxarg, int exists) {
    MPP_ASSERT_MSG(exists, "memoizer_update_delete: attempted to decrement refcount of non-contained VALUE");
    (*value)--;
    *((unsigned int *)ctxarg) = (unsigned int)*value;
    return *value == 0 ? ST_DELETE : ST_CONTINUE;
}

void mpp_mark_memoizer_mark(struct mpp_mark_memoizer *memo) {
    st_foreach(memo->table, memoizer_each_mark, (st_data_t)memo);
}

static int memoizer_each_mark(st_data_t key, st_data_t value, st_data_t ctxarg) {
    rb_gc_mark_movable((VALUE)key);
    return ST_CONTINUE;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
void mpp_mark_memoizer_compact(struct mpp_mark_memoizer *memo) {
    st_foreach(memo->table, memoizer_each_compact, (st_data_t)memo);
}

static int memoizer_each_compact(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct mpp_mark_memoizer *memo = (struct mpp_mark_memoizer *)ctxarg;
    VALUE old_location = (VALUE)key;
    VALUE new_location = rb_gc_location(old_location);

    if (old_location != new_location) {
        st_insert(memo->table, new_location, value);
        return ST_DELETE;
    } else {
        return ST_CONTINUE;
    }
}
#endif

size_t mpp_mark_memoizer_memsize(struct mpp_mark_memoizer *memo) {
    return sizeof(struct mpp_mark_memoizer) + st_memsize(memo->table);
}
