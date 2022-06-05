// External ruby headers
#include <ruby.h>
#include <ruby/debug.h>

#include "ruby_memprofiler_pprof.h"

void mpp_rb_backtrace_capture(struct mpp_rb_backtrace **bt_out) {
    *bt_out = mpp_xmalloc(sizeof(struct mpp_rb_backtrace));
    (*bt_out)->frame_extras = NULL;
    (*bt_out)->backtracie = backtracie_bt_capture();
}

void mpp_rb_backtrace_destroy(struct mpp_rb_backtrace *bt) {
    backtracie_bt_free(bt->backtracie);
    if (bt->frame_extras) {
        mpp_free(bt->frame_extras);
    }
    mpp_free(bt);
}

size_t mpp_rb_backtrace_memsize(struct mpp_rb_backtrace *bt) {
    return sizeof(struct mpp_rb_backtrace) +
            backtracie_bt_memsize(bt->backtracie) +
            backtracie_bt_get_frames_count(bt->backtracie) * sizeof(uint64_t);
}

struct mpp_functab *mpp_functab_new(struct mpp_strtab *strtab) {
    struct mpp_functab *functab = mpp_xmalloc(sizeof(struct mpp_functab));
    functab->strtab = strtab;
    functab->funcs = st_init_numtable();
    functab->funcs_count = 0;
    return functab;
}

static int mpp_functab_dealloc_loc(st_data_t key, st_data_t value, st_data_t arg) {
    struct mpp_functab_func *existing_func = (struct mpp_functab_func *)value;
    struct mpp_functab *functab = (struct mpp_functab *)arg;
    mpp_strtab_release(functab->strtab, existing_func->file_name, existing_func->file_name_len);
    mpp_strtab_release(functab->strtab, existing_func->function_name, existing_func->function_name_len);
    return ST_CONTINUE;
}

void mpp_functab_destroy(struct mpp_functab *functab) {
    st_foreach(functab->funcs, mpp_functab_dealloc_loc, (st_data_t)functab);
    st_free_table(functab->funcs);
    free(functab);
}

size_t mpp_functab_memsize(struct mpp_functab *functab) {
    return st_memsize(functab->funcs) + functab->funcs_count * sizeof(struct mpp_functab_func);
}

struct mpp_functab_update_ctx {
    struct mpp_functab *functab;
    uint64_t id;
    VALUE name;
    VALUE file_name;
    VALUE line_number;
};

static int mpp_functab_update_add_loc(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
    if (*value) {
        struct mpp_functab_func *existing_func = (struct mpp_functab_func *)*value;
        existing_func->refcount++;
    } else {
        struct mpp_functab_update_ctx *ctx = (struct mpp_functab_update_ctx *)arg;
        struct mpp_functab_func *new_func = mpp_xmalloc(sizeof(struct mpp_functab_func));
        *value = (st_data_t)new_func;

        new_func->refcount = 1;
        if (RTEST(ctx->name)) {
            mpp_strtab_intern_rbstr(
                ctx->functab->strtab, ctx->name,
                &new_func->function_name, &new_func->function_name_len
            );
        } else {
            mpp_strtab_intern_cstr(
                ctx->functab->strtab, "(unknown)",
                &new_func->function_name, &new_func->function_name_len
            );
        }
        if (RTEST(ctx->file_name)) {
            mpp_strtab_intern_rbstr(
                ctx->functab->strtab, ctx->file_name,
                &new_func->file_name, &new_func->file_name_len
            );
        } else {
            mpp_strtab_intern_cstr(
                ctx->functab->strtab, "(unknown)",
                &new_func->file_name, &new_func->file_name_len
            );
        }
        if (RTEST(ctx->line_number)) {
            new_func->line_number = NUM2LONG(ctx->line_number);
        } else {
            new_func->line_number = 0;
        }
        new_func->id = *key;

        ctx->functab->funcs_count++;
    }

    return ST_CONTINUE;
}

static int mpp_functab_update_del_loc(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
    if (*value) {
        struct mpp_functab_update_ctx *ctx = (struct mpp_functab_update_ctx *)arg;
        struct mpp_functab_func *existing_func = (struct mpp_functab_func *)*value;
        existing_func->refcount--;
        if (existing_func->refcount == 0) {
            mpp_strtab_release(ctx->functab->strtab, existing_func->function_name, existing_func->function_name_len);
            mpp_strtab_release(ctx->functab->strtab, existing_func->file_name, existing_func->file_name_len);
            ctx->functab->funcs_count--;
            return ST_DELETE;
        } else {
            return ST_CONTINUE;
        }
    } else {
        return ST_DELETE;
    }
}

uint64_t mpp_functab_add(
    struct mpp_functab *functab, uint64_t id, VALUE name, VALUE file_name, VALUE line_number
) {
    struct mpp_functab_update_ctx args;
    args.functab = functab;
    args.id = id;
    args.name = name;
    args.file_name = file_name;
    args.line_number = line_number;
    st_update(functab->funcs, id, mpp_functab_update_add_loc, (st_data_t)&args);
    return id;
}

uint64_t mpp_functab_del(struct mpp_functab *functab, uint64_t id) {
    struct mpp_functab_update_ctx args;
    args.functab = functab;
    args.id = id;
    st_update(functab->funcs, id, mpp_functab_update_del_loc, (st_data_t)&args);
    return id;
}

void mpp_functab_add_all_frames(struct mpp_functab *functab, struct mpp_rb_backtrace *bt) {
    uint32_t frames_count = backtracie_bt_get_frames_count(bt->backtracie);
    bt->frame_extras = mpp_xmalloc(frames_count * sizeof(struct mpp_rb_backtrace_frame_extra));
    for (uint32_t i = 0; i < frames_count; i++) {
        VALUE frame = backtracie_bt_get_frame_value(bt->backtracie, i);
        uint64_t id = NUM2LONG(rb_obj_id(frame));
        VALUE name = backtracie_bt_get_frame_method_name(bt->backtracie, i);
        VALUE file_name = backtracie_bt_get_frame_file_name(bt->backtracie, i);
        VALUE line_number = backtracie_bt_get_frame_line_number(bt->backtracie, i);
        VALUE fn_line_number = rb_profile_frame_first_lineno(frame);

        mpp_functab_add(functab, id, name, file_name, fn_line_number);

        bt->frame_extras[i].function_id = id;
        bt->frame_extras[i].line_number = RTEST(line_number) ? NUM2LONG(line_number) : 0;
    }
}

void mpp_functab_del_all_frames(struct mpp_functab *functab, struct mpp_rb_backtrace *bt) {
    uint32_t frames_count = backtracie_bt_get_frames_count(bt->backtracie);
    for (uint32_t i = 0; i < frames_count; i++) {
        VALUE frame = backtracie_bt_get_frame_value(bt->backtracie, i);
        uint64_t id = NUM2LONG(rb_obj_id(frame));
        mpp_functab_del(functab, id);
    }
}


struct mpp_functab_func *mpp_functab_lookup_frame(struct mpp_functab *functab, uint64_t id) {
    struct mpp_functab_func *ret = NULL;
    st_lookup(functab->funcs, id, (st_data_t *)&ret);
    return ret;
}

void mpp_backtrace_gc_mark(struct mpp_rb_backtrace *bt) {
    backtracie_bt_gc_mark_moveable(bt->backtracie);
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
void mpp_backtrace_gc_compact(struct mpp_rb_backtrace * bt) {
    backtracie_bt_gc_move(bt->backtracie);
}
#endif
