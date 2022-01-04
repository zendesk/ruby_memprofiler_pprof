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
void mpp_pprof_serctx_init(struct mpp_pprof_serctx *ctx) {
    ctx->allocator.func = mpp_pprof_upb_arena_malloc;
    ctx->arena = upb_arena_init(NULL, 0, &ctx->allocator);
    ctx->profile_proto = perftools_profiles_Profile_new(ctx->arena);
    ctx->added_functions = rb_st_init_numtable();
    ctx->added_locations = rb_st_init_numtable();
    memset(&ctx->strindex, 0, sizeof(ctx->strindex));
    ctx->initialized = 1;
}

// Destroys the serialization context. After this call, any stringtab indexes it held
// are released, and any memory from its internal state is freed. *ctx itself is _NOT_ freed;
// if it is not stack allocated, freeing it is the responsibility of the caller.
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx) {
    if (!ctx->initialized) {
        return;
    }
    upb_arena_free(ctx->arena);
    rb_st_free_table(ctx->added_functions);
    rb_st_free_table(ctx->added_locations);
    mpp_strtab_index_destroy(&ctx->strindex);
}

// This method is used to set the stringtab interning table used on the strings in the samples.
// This method will actually take a reference to all relevant strings currently in strtab via
// a call to mpp_strtab_index; thus, after this method returns, it is safe for another thread to
// continue using the stringtab because we have what we need in our index.
void mpp_pprof_serctx_set_strtab(struct mpp_pprof_serctx *ctx, struct str_intern_tab *strtab) {
    // Intern some strings we'll need to produce our output
    mpp_strtab_intern(strtab, "allocations", MPP_STRTAB_USE_STRLEN, &ctx->internstr_allocations, NULL);
    mpp_strtab_intern(strtab, "count", MPP_STRTAB_USE_STRLEN, &ctx->internstr_count, NULL);

    mpp_strtab_index(strtab, &ctx->strindex);

    upb_strview *stringtab_list_proto =
        perftools_profiles_Profile_resize_string_table(ctx->profile_proto, ctx->strindex.str_list_len, ctx->arena);
    for (int64_t i = 0; i < ctx->strindex.str_list_len; i++) {
        upb_strview *stringtab_proto = &stringtab_list_proto[i];
        struct str_intern_tab_el *intern_tab_el = ctx->strindex.str_list[i];
        stringtab_proto->data = intern_tab_el->str;
        stringtab_proto->size = intern_tab_el->str_len;
    }

    perftools_profiles_ValueType *vt =
        perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
    perftools_profiles_ValueType_set_type(vt, mpp_strtab_index_of(&ctx->strindex, ctx->internstr_allocations));
    perftools_profiles_ValueType_set_unit(vt, mpp_strtab_index_of(&ctx->strindex, ctx->internstr_count));
}

static void mpp_pprof_serctx_add_function(struct mpp_pprof_serctx *ctx, struct mpp_rb_backtrace_frame *frame) {
    // Have we already added this function before?
    if (rb_st_lookup(ctx->added_functions, frame->function_id, NULL)) {
        return;
    }

    // No, add it.
    perftools_profiles_Function *fn_proto = perftools_profiles_Profile_add_function(ctx->profile_proto, ctx->arena);
    perftools_profiles_Function_set_id(fn_proto, frame->function_id);
    perftools_profiles_Function_set_name(fn_proto, mpp_strtab_index_of(&ctx->strindex, frame->function_name));
    perftools_profiles_Function_set_system_name(fn_proto, mpp_strtab_index_of(&ctx->strindex, frame->function_name));
    perftools_profiles_Function_set_filename(fn_proto, mpp_strtab_index_of(&ctx->strindex, frame->filename));

    rb_st_insert(ctx->added_functions, frame->function_id, 0);
}

static void mpp_pprof_serctx_add_location(struct mpp_pprof_serctx *ctx, struct mpp_rb_backtrace_frame *frame) {
    // Have we already added this location before?
    if (rb_st_lookup(ctx->added_locations, frame->location_id, NULL)) {
        return;
    }

    // No, add it.
    perftools_profiles_Location *loc_proto = perftools_profiles_Profile_add_location(ctx->profile_proto, ctx->arena);
    perftools_profiles_Location_set_id(loc_proto, frame->location_id);
    perftools_profiles_Line *line_proto = perftools_profiles_Location_add_line(loc_proto, ctx->arena);
    perftools_profiles_Line_set_function_id(line_proto, frame->function_id);
    perftools_profiles_Line_set_line(line_proto, frame->line_number);

    rb_st_insert(ctx->added_locations, frame->location_id, 0);
}

void mpp_pprof_serctx_add_sample(struct mpp_pprof_serctx *ctx, struct mpp_sample *sample) {
    perftools_profiles_Sample *sample_proto = perftools_profiles_Profile_add_sample(ctx->profile_proto, ctx->arena);
    uint64_t *location_ids =
        perftools_profiles_Sample_resize_location_id(sample_proto, sample->bt.frames_count, ctx->arena);

    // We need to iterate through this backtrace backwards; Ruby makes it easy for us to get this backtrace
    // in most-recent-call-last format, but pprof wants most-recent-call-first.
    for (int64_t i = 0; i < sample->bt.frames_count; i++) {
        struct mpp_rb_backtrace_frame *frame = &sample->bt.frames[sample->bt.frames_count - i - 1];
        location_ids[i] = frame->location_id;

        // Adds the function & location protos to the main proto by ID, if they're not there already.
        mpp_pprof_serctx_add_function(ctx, frame);
        mpp_pprof_serctx_add_location(ctx, frame);
    }

    // TODO: values & labels
    perftools_profiles_Sample_add_value(sample_proto, 1, ctx->arena);
}

// Serializes the contained protobuf, and gzips the result. Writes a pointer to the memory in *buf_out,
// and its length to buflen_out. The returned pointer is freed when mpp_pprof_serctx_destroy() is called,
// and should NOT be individually freed by the caller in any way (and nor is it valid after the call
// to destroy()).
int mpp_pprof_serctx_serialize(struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf, size_t errbuflen) {
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
    r = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits | GZIP_ENCODING, 8, Z_DEFAULT_STRATEGY);
    if (r != Z_OK) {
        ruby_snprintf(errbuf, errbuflen, "error initializing zlib (errno %d: %s)", r, strm.msg ?: "");
        return -1;
    }
    strm.avail_in = (unsigned int)protobuf_data_len;
    strm.next_in = (unsigned char *)protobuf_data;

    const size_t out_chunk_size = 4096;
    char *gzip_data = upb_arena_malloc(ctx->arena, out_chunk_size);
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
            gzip_data = upb_arena_realloc(ctx->arena, gzip_data, old_gzip_data_allocd_len, gzip_data_allocd_len);
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
