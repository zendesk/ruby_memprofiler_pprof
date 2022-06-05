// External ruby headers
#include <ruby.h>
#include <ruby/debug.h>

#include "ruby_memprofiler_pprof.h"

void mpp_rb_backtrace_capture(struct mpp_rb_backtrace **bt_out) {
    int frames_capacity = backtracie_rb_profile_frames_count();
    *bt_out = mpp_xmalloc(sizeof(struct mpp_rb_backtrace) + sizeof(struct mpp_rb_backtrace_frame) * frames_capacity);
    (*bt_out)->frames_capacity = frames_capacity;
    backtracie_raw_location *frame_temp_buffer = mpp_xmalloc(sizeof(backtracie_raw_location) * frames_capacity);
    (*bt_out)->frames_count = backtracie_rb_profile_frames(frames_capacity, frame_temp_buffer);
    for (int i = 0; i < (*bt_out)->frames_count; i++) {
        (*bt_out)->frames[i].raw = frame_temp_buffer[i];
        (*bt_out)->frames[i].function_id = rb_obj_id(backtracie_frame_from_location(&frame_temp_buffer[i]));
    }
}

void mpp_rb_backtrace_destroy(struct mpp_rb_backtrace *bt) {
    mpp_free(bt);
}

size_t mpp_rb_backtrace_memsize(struct mpp_rb_backtrace *bt) {
    return sizeof(struct mpp_rb_backtrace) + sizeof(struct mpp_rb_backtrace_frame) * bt->frames_capacity;
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

struct mpp_functab_update_loc_ctx {
    struct mpp_functab *functab;
    struct mpp_rb_backtrace_frame *loc;
};

static int mpp_functab_update_add_loc(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
    if (*value) {
        struct mpp_functab_func *existing_func = (struct mpp_functab_func *)*value;
        existing_func->refcount++;
    } else {
        struct mpp_functab_update_loc_ctx *ctx = (struct mpp_functab_update_loc_ctx *)arg;
        struct mpp_functab_func *new_func = mpp_xmalloc(sizeof(struct mpp_functab_func));
        *value = (st_data_t)new_func;

        new_func->refcount = 1;
        VALUE frame = backtracie_frame_from_location(&ctx->loc->raw);
        VALUE method_name = backtracie_qualified_method_name_for_location(&ctx->loc->raw);
        mpp_strtab_intern_rbstr(ctx->functab->strtab, method_name, &new_func->function_name, &new_func->function_name_len);
        VALUE path = rb_profile_frame_path(frame);
        mpp_strtab_intern_rbstr(ctx->functab->strtab, path, &new_func->file_name, &new_func->file_name_len);
        VALUE lineno_value = rb_profile_frame_first_lineno(frame);
        if (RTEST(lineno_value)) {
            new_func->line_number = NUM2LONG(lineno_value);
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
        struct mpp_functab_update_loc_ctx *ctx = (struct mpp_functab_update_loc_ctx *)arg;
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

uint64_t mpp_functab_add_frame(struct mpp_functab *functab, struct mpp_rb_backtrace_frame *loc) {
    VALUE frame = backtracie_frame_from_location(&loc->raw);
    uint64_t frame_key = rb_obj_id(frame);
    struct mpp_functab_update_loc_ctx args;
    args.functab = functab;
    args.loc = loc;
    st_update(functab->funcs, frame_key, mpp_functab_update_add_loc, (st_data_t)&args);
    return frame_key;
}

uint64_t mpp_functab_del_frame(struct mpp_functab *functab, struct mpp_rb_backtrace_frame *loc) {
    VALUE frame = backtracie_frame_from_location(&loc->raw);
    uint64_t frame_key = rb_obj_id(frame);
    struct mpp_functab_update_loc_ctx args;
    args.functab = functab;
    args.loc = loc;
    st_update(functab->funcs, frame_key, mpp_functab_update_del_loc, (st_data_t)&args);
    return frame_key;
}

struct mpp_functab_func *mpp_functab_lookup_frame(struct mpp_functab *functab, uint64_t id) {
    struct mpp_functab_func *ret = NULL;
    st_lookup(functab->funcs, id, (st_data_t *)&ret);
    return ret;
}

void mpp_backtrace_gc_mark(struct mpp_rb_backtrace *bt) {
    for (int i = 0; i < bt->frames_count; i++) {
        rb_gc_mark_movable(bt->frames[i].raw.iseq);
        rb_gc_mark_movable(bt->frames[i].raw.callable_method_entry);
        rb_gc_mark_movable(bt->frames[i].raw.original_id);
        rb_gc_mark_movable(bt->frames[i].raw.self);
    }
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
void mpp_backtrace_gc_compact(struct mpp_rb_backtrace * bt) {
    for (int i = 0; i < bt->frames_count; i++) {
        bt->frames[i].raw.iseq = rb_gc_location(bt->frames[i].raw.iseq);
        bt->frames[i].raw.callable_method_entry = rb_gc_location(bt->frames[i].raw.callable_method_entry);
        bt->frames[i].raw.original_id = rb_gc_location(bt->frames[i].raw.original_id);
        bt->frames[i].raw.self = rb_gc_location(bt->frames[i].raw.self);
    }
}
#endif
