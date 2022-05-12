#include <ruby.h>
#include <zlib.h>

#include "ruby_memprofiler_pprof.h"

// allocator/free function for our upb_arena. Contract is:
// If "size" is 0 then the function acts like free(), otherwise it acts like
// realloc().  Only "oldsize" bytes from a previous allocation are preserved.
static void *mpp_pprof_upb_arena_malloc(upb_alloc *alloc, void *ptr, size_t oldsize, size_t size) {
    if (size == 0) {
        mpp_free(ptr);
        return NULL;
    } else {
        return mpp_realloc(ptr, size);
    }
}

// Initialize an already-allocated serialization context.
struct mpp_pprof_serctx *mpp_pprof_serctx_new() {
    struct mpp_pprof_serctx *ctx = mpp_xmalloc(sizeof(struct mpp_pprof_serctx));
    ctx->allocator.func = mpp_pprof_upb_arena_malloc;
    ctx->arena = upb_Arena_Init(NULL, 0, &ctx->allocator);
    ctx->profile_proto = perftools_profiles_Profile_new(ctx->arena);
    ctx->strindex = NULL;
    return ctx;
}

// Destroys the serialization context. After this call, any stringtab indexes it held
// are released, and any memory from its internal state is freed. *ctx itself is also
// freed and must not be dereferenced after this.
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx) {
    upb_Arena_Free(ctx->arena);
    if (ctx->strindex) {
        mpp_strtab_index_destroy(ctx->strindex);
    }
    mpp_free(ctx);
}


struct mpp_pprof_serctx_add_location_ctx {
    struct mpp_pprof_serctx *ctx;
    int is_error;
    char *errbuf;
    size_t errbuflen;
};

static int mpp_pprof_serctx_add_location(
    struct mpp_rb_loctab *loctab, struct mpp_rb_loctab_location *location, void *ctx
) {
    struct mpp_pprof_serctx_add_location_ctx *thunkctx = ctx;
    struct perftools_profiles_Location *loc_proto = perftools_profiles_Profile_add_location(thunkctx->ctx->profile_proto, thunkctx->ctx->arena);


    perftools_profiles_Location_set_id(loc_proto, location->id);
    perftools_profiles_Line *line_proto = perftools_profiles_Location_add_line(loc_proto, thunkctx->ctx->arena);
    perftools_profiles_Line_set_function_id(line_proto, location->function->id);
    perftools_profiles_Line_set_line(line_proto, location->line_number);

    return ST_CONTINUE;
}

struct mpp_pprof_serctx_add_function_ctx {
    struct mpp_pprof_serctx *ctx;
    int is_error;
    char *errbuf;
    size_t errbuflen;
};

static int mpp_pprof_serctx_add_function(
    struct mpp_rb_loctab *loctab, struct mpp_rb_loctab_function *function, void *ctx
) {
    struct mpp_pprof_serctx_add_function_ctx *thunkctx = ctx;
    struct perftools_profiles_Function *fn_proto = perftools_profiles_Profile_add_function(thunkctx->ctx->profile_proto, thunkctx->ctx->arena);

    perftools_profiles_Function_set_id(fn_proto, function->id);

#define FN_SET_STRINTERN_FIELD(field, str)                                                                  \
    do {                                                                                                    \
        int64_t interned = mpp_strtab_index_of(thunkctx->ctx->strindex, (str));                             \
        if (interned == -1) {                                                                               \
            ruby_snprintf(                                                                                  \
                thunkctx->errbuf, thunkctx->errbuflen,                                                      \
                "non-interned string %s passed for Function.#field", (str)                                  \
            );                                                                                              \
            return -1;                                                                                      \
        }                                                                                                   \
        perftools_profiles_Function_set_##field(fn_proto, interned);                                        \
    } while (0)

    FN_SET_STRINTERN_FIELD(name, function->function_name);
    FN_SET_STRINTERN_FIELD(system_name, function->function_name);
    FN_SET_STRINTERN_FIELD(filename, function->file_name);

#undef FN_SET_STRINTERN_FIELD


    return ST_CONTINUE;
}


// This method is used to set the location table used in backtrace samples to be added
//
// This method will actually take a reference to all relevant strings currently in loctab->strtab via
// a call to mpp_strtab_index; thus, after this method returns, it is safe for another thread to
// continue using the stringtab because we have what we need in our index.
int mpp_pprof_serctx_set_loctab(
    struct mpp_pprof_serctx *ctx, struct mpp_rb_loctab *loctab, char *errbuf, size_t errbuflen
) {
    ctx->loctab = loctab;

    // Intern some strings we'll need to produce our output
    mpp_strtab_intern_cstr(loctab->strtab, "allocations", &ctx->internstr_allocations, NULL);
    mpp_strtab_intern_cstr(loctab->strtab, "count", &ctx->internstr_count, NULL);
    mpp_strtab_intern_cstr(loctab->strtab, "allocation_size", &ctx->internstr_allocation_size, NULL);
    mpp_strtab_intern_cstr(loctab->strtab, "bytes", &ctx->internstr_bytes, NULL);
    mpp_strtab_intern_cstr(loctab->strtab, "retained_objects", &ctx->internstr_retained_objects, NULL);
    mpp_strtab_intern_cstr(loctab->strtab, "retained_size", &ctx->internstr_retained_size, NULL);

    ctx->strindex = mpp_strtab_index(loctab->strtab);
    MPP_ASSERT_MSG(ctx->strindex, "mpp_strtab_index returned 0");

    // Set up the string table in the protobuf
    upb_StringView *stringtab_list_proto =
        perftools_profiles_Profile_resize_string_table(ctx->profile_proto, ctx->strindex->str_list_len, ctx->arena);
    for (int64_t i = 0; i < ctx->strindex->str_list_len; i++) {
        upb_StringView *stringtab_proto = &stringtab_list_proto[i];
        struct mpp_strtab_el *intern_tab_el = ctx->strindex->str_list[i];
        stringtab_proto->data = intern_tab_el->str;
        stringtab_proto->size = intern_tab_el->str_len;
    }

    // Set up the sample types etc.
    perftools_profiles_ValueType *allocations_vt =
        perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
    perftools_profiles_ValueType *allocation_size_vt =
        perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
    perftools_profiles_ValueType *retained_objects_vt =
        perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
    perftools_profiles_ValueType *retained_size_vt =
        perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
#define VT_SET_STRINTERN_FIELD(vt, field, str)                                                              \
    do {                                                                                                    \
        int64_t interned = mpp_strtab_index_of(ctx->strindex, (str));                                       \
        if (interned == -1) {                                                                               \
            ruby_snprintf(errbuf, errbuflen, "non-interned string %s passed for ValueType.#field", (str));  \
            return -1;                                                                                      \
        }                                                                                                   \
        perftools_profiles_ValueType_set_##field(vt, interned);                                             \
    } while (0)

    VT_SET_STRINTERN_FIELD(allocations_vt, type, ctx->internstr_allocations);
    VT_SET_STRINTERN_FIELD(allocations_vt, unit, ctx->internstr_count);
    VT_SET_STRINTERN_FIELD(allocation_size_vt, type, ctx->internstr_allocation_size);
    VT_SET_STRINTERN_FIELD(allocation_size_vt, unit, ctx->internstr_bytes);
    VT_SET_STRINTERN_FIELD(retained_objects_vt, type, ctx->internstr_retained_objects);
    VT_SET_STRINTERN_FIELD(retained_objects_vt, unit, ctx->internstr_count);
    VT_SET_STRINTERN_FIELD(retained_size_vt, type, ctx->internstr_retained_size);
    VT_SET_STRINTERN_FIELD(retained_size_vt, unit, ctx->internstr_bytes);
#undef VT_SET_STRINTERN_FIELD

    // Set up the location array.
    struct mpp_pprof_serctx_add_location_ctx loc_add_ctx;
    loc_add_ctx.ctx = ctx;
    loc_add_ctx.errbuf = errbuf;
    loc_add_ctx.errbuflen = errbuflen;
    loc_add_ctx.is_error = 0;
    mpp_rb_loctab_each_location(ctx->loctab, mpp_pprof_serctx_add_location, &loc_add_ctx);
    if (loc_add_ctx.is_error) {
        return -1;
    }

    // And the same for functions pretty much
    struct mpp_pprof_serctx_add_function_ctx fn_add_ctx;
    fn_add_ctx.ctx = ctx;
    fn_add_ctx.errbuf = errbuf;
    fn_add_ctx.errbuflen = errbuflen;
    fn_add_ctx.is_error = 0;
    mpp_rb_loctab_each_function(ctx->loctab, mpp_pprof_serctx_add_function, &fn_add_ctx);
    if (loc_add_ctx.is_error) {
        return -1;
    }

    return 0;
}

int mpp_pprof_serctx_add_sample(
    struct mpp_pprof_serctx *ctx, struct mpp_sample *sample, int sample_type, char *errbuf, size_t errbuflen
) {
    perftools_profiles_Sample *sample_proto = perftools_profiles_Profile_add_sample(ctx->profile_proto, ctx->arena);
    uint64_t *location_ids =
        perftools_profiles_Sample_resize_location_id(sample_proto, sample->bt->frames_count, ctx->arena);

    // We need to iterate through this backtrace backwards; Ruby makes it easy for us to get this backtrace
    // in most-recent-call-last format, but pprof wants most-recent-call-first.
    for (int64_t i = 0; i < sample->bt->frames_count; i++) {
        location_ids[i] = sample->bt->frame_locations[sample->bt->frames_count - i - 1];
    }

    // Values are (allocation_count, allocation_size, retained_count, retained_size).
    int64_t allocation_count = 0;
    int64_t allocation_size = 0;
    int64_t retained_count = 0;
    int64_t retained_size = 0;
    if (sample_type == MPP_SAMPLE_TYPE_ALLOCATION) {
        allocation_count = 1;
        allocation_size = (int64_t)sample->allocation_size;
    }
    if (sample_type == MPP_SAMPLE_TYPE_HEAP) {
        retained_count = 1;
        retained_size = (int64_t)sample->current_size;
    }
    perftools_profiles_Sample_add_value(sample_proto, allocation_count, ctx->arena);
    perftools_profiles_Sample_add_value(sample_proto, allocation_size, ctx->arena);
    perftools_profiles_Sample_add_value(sample_proto, retained_count, ctx->arena);
    perftools_profiles_Sample_add_value(sample_proto, retained_size, ctx->arena);
    return 0;
}

// Serializes the contained protobuf, and gzips the result. Writes a pointer to the memory in *buf_out,
// and its length to buflen_out. The returned pointer is freed when mpp_pprof_serctx_destroy() is called,
// and should NOT be individually freed by the caller in any way (and nor is it valid after the call
// to destroy()).
int mpp_pprof_serctx_serialize(
    struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf, size_t errbuflen
) {
    // It looks like some codepaths might leak the protobuf_data pointer, but that's OK - it's in
    // the ctx->arena so it'll get freed when ctx does.
    size_t protobuf_data_len;
    char *protobuf_data = perftools_profiles_Profile_serialize(ctx->profile_proto, ctx->arena, &protobuf_data_len);

    // Gzip it as per standard.
    z_stream strm;
    int r;
    int retval = 0;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.msg = NULL;
    int windowBits = 15;
    int GZIP_ENCODING = 16;
    r = deflateInit2(
        &strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
        windowBits | GZIP_ENCODING, 8, Z_DEFAULT_STRATEGY
    );
    if (r != Z_OK) {
        ruby_snprintf(errbuf, errbuflen, "error initializing zlib (errno %d: %s)", r, strm.msg ?: "");
        return -1;
    }
    strm.avail_in = (unsigned int)protobuf_data_len;
    strm.next_in = (unsigned char *)protobuf_data;

    const size_t out_chunk_size = 4096;
    char *gzip_data = upb_Arena_Malloc(ctx->arena, out_chunk_size);
    size_t gzip_data_allocd_len = out_chunk_size;
    strm.avail_out = out_chunk_size;
    strm.next_out = (unsigned char *)gzip_data;
    while(true) {
        int flush = strm.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH;
        r = deflate(&strm, flush);
        if (r == Z_STREAM_END) {
            break;
        } else if (r != Z_OK) {
            // uh wat?
            ruby_snprintf(errbuf, errbuflen, "error doing zlib output (errno %d: %s)", r, strm.msg ?: "");
            retval = -1;
            goto zstream_free;
        }

        if (strm.avail_out == 0) {
            size_t old_gzip_data_allocd_len = gzip_data_allocd_len;
            gzip_data_allocd_len += out_chunk_size;
            gzip_data = upb_Arena_Realloc(ctx->arena, gzip_data, old_gzip_data_allocd_len, gzip_data_allocd_len);
            strm.avail_out = out_chunk_size;
            strm.next_out = (unsigned char *)(gzip_data + old_gzip_data_allocd_len);
        }
    }
    // Set output pointers
    *buf_out = gzip_data;
    *buflen_out = gzip_data_allocd_len - strm.avail_out;

zstream_free:
    deflateEnd(&strm);
    return retval;
}
