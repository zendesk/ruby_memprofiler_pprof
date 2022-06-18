#include <ruby.h>
#include <stdlib.h>
#include "ruby_memprofiler_pprof.h"

static int memoizer_bsearch_compare(const void *e1void, const void *e2void);
static void memoizer_do_sort(struct mpp_mark_memoizer *memo);

struct mpp_mark_memoizer *mpp_mark_memoizer_new() {
    struct mpp_mark_memoizer *memo = mpp_xmalloc(sizeof(struct mpp_mark_memoizer));
    memo->sorted_list = NULL;
    memo->sorted_list_capacity = 0;
    memo->sorted_list_current_size = 0;
    memo->unsorted_list_capacity = 1024;
    memo->unsorted_list = ruby_xmalloc(memo->unsorted_list_capacity * sizeof(struct mpp_mark_memoizer_el));
    memo->unsorted_list_current_size = 0;
    return memo;
}

void mpp_mark_memoizer_destroy(struct mpp_mark_memoizer *memo) {
    if (memo->sorted_list) {
        mpp_free(memo->sorted_list);
    }
    if (memo->unsorted_list) {
        mpp_free(memo->unsorted_list);
    }
    mpp_free(memo);
}

unsigned int mpp_mark_memoizer_add(struct mpp_mark_memoizer *memo, VALUE value) {
    // See if it's in the sorted list.
    if (memo->sorted_list) {
        struct mpp_mark_memoizer_el key = { .value = value, .refcount = 0 };
        struct mpp_mark_memoizer_el *existing_el = bsearch(
            &key, memo->sorted_list, memo->sorted_list_current_size,
            sizeof(struct mpp_mark_memoizer_el), memoizer_bsearch_compare
        );
        if (existing_el) {
            existing_el->refcount++;
            return existing_el->refcount;
        }
    }

    // See if it's in the unsorted list
    for (size_t i = 0; i < memo->unsorted_list_current_size; i++) {
        if (memo->unsorted_list[i].value == value) {
            // Found it.
            memo->unsorted_list[i].refcount++;
            return memo->unsorted_list[i].refcount;
        }
    }

    // Nope, is there space in the unsorted list?
    if (memo->unsorted_list_current_size >= memo->unsorted_list_capacity) {
        memoizer_do_sort(memo);
    }

    // Insert to unsorted list.
    MPP_ASSERT_MSG(memo->unsorted_list_current_size < memo->unsorted_list_capacity, "no space freed in unsorted list?");
    memo->unsorted_list[memo->unsorted_list_current_size].value = value;
    memo->unsorted_list[memo->unsorted_list_current_size].refcount = 1;
    memo->unsorted_list_current_size++;
    return 1;
}


unsigned int mpp_mark_memoizer_delete(struct mpp_mark_memoizer *memo, VALUE value) {
    // See if it's in the sorted list.
    if (memo->sorted_list) {
        struct mpp_mark_memoizer_el key = { .value = value, .refcount = 0 };
        struct mpp_mark_memoizer_el *existing_el = bsearch(
            &key, memo->sorted_list, memo->sorted_list_current_size,
            sizeof(struct mpp_mark_memoizer_el), memoizer_bsearch_compare
        );
        if (existing_el) {
            MPP_ASSERT_MSG(existing_el->refcount > 0, "decrementing refcount to < 0 (sorted)");
            existing_el->refcount--;
            return existing_el->refcount;
        }
    }

    // See if it's in the unsorted list
    for (size_t i = 0; i < memo->unsorted_list_current_size; i++) {
        if (memo->unsorted_list[i].value == value) {
            // Found it.
            MPP_ASSERT_MSG(memo->unsorted_list[i].refcount > 0, "decrementing refcount to < 0 (unsorted)");
            memo->unsorted_list[i].refcount--;
            return memo->unsorted_list[i].refcount;
        }
    }

    MPP_ASSERT_FAIL("item to decrement refcount not found");
}

static void memoizer_do_sort(struct mpp_mark_memoizer *memo) {
    while (memo->sorted_list_capacity < memo->sorted_list_current_size + memo->unsorted_list_current_size) {
        if (memo->sorted_list_capacity == 0) {
            memo->sorted_list_capacity = 2048;
        } else {
            memo->sorted_list_capacity = memo->sorted_list_capacity * 2;
        }
        memo->sorted_list = mpp_realloc(memo->sorted_list, memo->sorted_list_capacity * sizeof(struct mpp_mark_memoizer_el));
    }
    memcpy(
        memo->sorted_list + memo->sorted_list_current_size,
        memo->unsorted_list,
        memo->unsorted_list_current_size * sizeof(struct mpp_mark_memoizer_el)
    );
    memo->sorted_list_current_size += memo->unsorted_list_current_size;
    memo->unsorted_list_current_size = 0;
    for (size_t i = 0; i < memo->sorted_list_current_size; i++) {
        if (memo->sorted_list[i].refcount == 0) {
            memo->sorted_list[i].value = Qundef;
        }
    }
    qsort(memo->sorted_list, memo->sorted_list_current_size, sizeof(struct mpp_mark_memoizer_el), memoizer_bsearch_compare);
    while (memo->sorted_list_current_size > 0 && memo->sorted_list[memo->sorted_list_current_size - 1].value == Qundef) {
        memo->sorted_list_current_size--;
    }
}

static int memoizer_bsearch_compare(const void *e1void, const void *e2void) {
    struct mpp_mark_memoizer_el *e1 = (struct mpp_mark_memoizer_el *)e1void;
    struct mpp_mark_memoizer_el *e2 = (struct mpp_mark_memoizer_el *)e2void;

    // objects with value Qundef need to be biggest
    if (e1->value == Qundef && e2->value != Qundef) {
        return 1;
    }
    if (e2->value == Qundef && e1->value != Qundef) {
        return -1;
    }

    // Otherwise sort by value
    if (e1->value < e2->value) {
        return -1;
    } else if (e1->value == e2->value) {
        return 0;
    } else {
        return 1;
    }
}

void mpp_mark_memoizer_mark(struct mpp_mark_memoizer *memo) {
    memoizer_do_sort(memo);
    for (size_t i = 0; i < memo->sorted_list_current_size; i++) {
        rb_gc_mark_movable(memo->sorted_list[i].value);
    }
}

void mpp_mark_memoizer_compact(struct mpp_mark_memoizer *memo) {
    memoizer_do_sort(memo);
    for (size_t i = 0; i < memo->sorted_list_current_size; i++) {
        struct mpp_mark_memoizer_el el = memo->sorted_list[i];
        if (el.refcount == 0) continue;
        el.value = rb_gc_location(el.value);
    }
    memoizer_do_sort(memo);
}

size_t mpp_mark_memoizer_memsize(struct mpp_mark_memoizer *memo) {
    return sizeof(struct mpp_mark_memoizer) +
        sizeof(struct mpp_mark_memoizer_el) * memo->sorted_list_capacity +
        sizeof(struct mpp_mark_memoizer_el) * memo->unsorted_list_capacity;
}
