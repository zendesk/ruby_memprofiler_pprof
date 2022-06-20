#include <ruby.h>
#include "ruby_memprofiler_pprof.h"

static int functab_each_destroy(st_data_t key, st_data_t value, st_data_t ctxarg);
static int functab_each_mark(st_data_t key, st_data_t value, st_data_t ctxarg);
static int functab_each_compact(st_data_t key, st_data_t value, st_data_t ctxarg);
struct functab_update_add_ctx {
    struct mpp_functab *functab;
    unsigned long id;
    VALUE cme_or_iseq;
    VALUE function_name;
    VALUE file_name;
};
static int functab_update_add(st_data_t *key, st_data_t *value, st_data_t ctxarg, int existing);
struct functab_update_deref_ctx {
    struct mpp_functab *functab;
    unsigned long id;
};
static int functab_update_deref(st_data_t *key, st_data_t *value, st_data_t ctxarg, int existing);

struct mpp_functab *mpp_functab_new(struct mpp_strtab *strtab) {
    struct mpp_functab *functab = mpp_xmalloc(sizeof(struct mpp_functab));
    functab->strtab = strtab;
    functab->function_map = st_init_numtable();
    functab->function_count = 0;
    return functab;
}


void mpp_functab_destroy(struct mpp_functab *functab) {
    // Free all resources held by any contained functions.
    st_foreach(functab->function_map, functab_each_destroy, (st_data_t)functab);
    st_free_table(functab->function_map);
    mpp_free(functab);
}


void mpp_functab_gc_mark(struct mpp_functab *functab) {
    st_foreach(functab->function_map, functab_each_mark, (st_data_t)functab);
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
// Move any owned Ruby VALUEs
void mpp_functab_gc_compact(struct mpp_functab *functab) {
    st_foreach(functab->function_map, functab_each_compact, (st_data_t)functab);
}
#endif


size_t mpp_functab_memsize(struct mpp_functab *functab) {
    return sizeof(struct mpp_functab) +
        functab->function_count * sizeof(struct mpp_functab_entry) +
        st_memsize(functab->function_map);
}


unsigned long mpp_functab_add_by_value(
    struct mpp_functab *functab, VALUE cme_or_iseq, VALUE function_name, VALUE file_name
) {
    unsigned long id = NUM2ULONG(rb_obj_id(cme_or_iseq));
    struct functab_update_add_ctx ctx = {
        .id = id,
        .functab = functab,
        .cme_or_iseq = cme_or_iseq,
        .function_name = function_name,
        .file_name = file_name,
    };
    st_update(functab->function_map, id, functab_update_add, (st_data_t)&ctx);
    return id;
}

void mpp_functab_deref(struct mpp_functab *functab, unsigned long id) {
    struct functab_update_deref_ctx ctx = {
        .id = id,
        .functab = functab,
    };
    st_update(functab->function_map, id, functab_update_deref, (st_data_t)&ctx);
}

struct mpp_functab_entry *mpp_functab_lookup(struct mpp_functab *functab, unsigned long id) {
    struct mpp_functab_entry *ret = NULL;
    st_lookup(functab->function_map, id, (st_data_t *)&ret);
    return ret;
}


static int functab_each_destroy(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct mpp_functab_entry *entry = (struct mpp_functab_entry *)value;
    struct mpp_functab *functab = (struct mpp_functab *)ctxarg;

    mpp_strtab_release(functab->strtab, entry->function_name, entry->function_name_len);
    mpp_strtab_release(functab->strtab, entry->file_name, entry->file_name_len);
    mpp_free(entry);

    return ST_CONTINUE;
}

static int functab_each_mark(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct mpp_functab_entry *entry = (struct mpp_functab_entry *)value;
    rb_gc_mark_movable(entry->cme_or_iseq);
    return ST_CONTINUE;
}

#ifdef RB_GHAVE_RB_GC_MARK_MOVABLE
static int functab_each_compact(st_data_t key, st_data_t value, st_data_t ctxarg) {
    struct mpp_functab_entry *entry = (struct mpp_functab_entry *)value;
    entry->cme_or_iseq = rb_gc_location(entry->cme_or_iseq);
    return ST_CONTINUE;
}
#endif

static int functab_update_add(st_data_t *key, st_data_t *value, st_data_t ctxarg, int existing) {
    if (*value) {
        // ID already present, update refcount
        struct mpp_functab_entry *entry = (struct mpp_functab_entry *)(*value);
        entry->refcount++;
        return ST_CONTINUE;
    } else {
        // ID not already present, construct a new one.
        struct functab_update_add_ctx *ctx = (struct functab_update_add_ctx *)ctxarg;

        // Intern strings before we allocate any memory, because these can raise Ruby exceptions.
        const char *function_name_cstr;
        size_t function_name_len;
        const char *file_name_cstr;
        size_t file_name_len;
        mpp_strtab_intern_rbstr(
            ctx->functab->strtab, ctx->function_name,
            &function_name_cstr, &function_name_len
        );
        mpp_strtab_intern_rbstr(
            ctx->functab->strtab, ctx->file_name,
            &file_name_cstr, &file_name_len
        );

        struct mpp_functab_entry *entry = mpp_xmalloc(sizeof(struct mpp_functab_entry));
        entry->id = ctx->id;
        entry->refcount = 1;
        entry->cme_or_iseq = ctx->cme_or_iseq;
        entry->function_name = function_name_cstr;
        entry->function_name_len = function_name_len;
        entry->file_name = file_name_cstr;
        entry->file_name_len = file_name_len;

        ctx->functab->function_count++;
        *value = (st_data_t)entry;
        return ST_CONTINUE;
    }
}

static int functab_update_deref(st_data_t *key, st_data_t *value, st_data_t ctxarg, int existing) {
    MPP_ASSERT_MSG(*value, "mpp_functab_deref called with non-existing function?");

    struct functab_update_deref_ctx *ctx = (struct functab_update_deref_ctx *)ctxarg;
    struct mpp_functab_entry *entry = (struct mpp_functab_entry *)(*value);

    MPP_ASSERT_MSG(entry->refcount, "mpp_functab_deref called with zero refcount!");
    entry->refcount--;
    if (!entry->refcount) {
        mpp_strtab_release(ctx->functab->strtab, entry->function_name, entry->function_name_len);
        mpp_strtab_release(ctx->functab->strtab, entry->file_name, entry->file_name_len);
        ctx->functab->function_count--;
        return ST_DELETE;
    } else {
        return ST_CONTINUE;
    };
}
