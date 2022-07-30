#include <ruby.h>
#include <zlib.h>

#include "ruby_memprofiler_pprof.h"

#define CHECK_IF_INTERRUPTED(action)                                                                                   \
  do {                                                                                                                 \
    uint8_t interrupted;                                                                                               \
    __atomic_load(&ctx->interrupt, &interrupted, __ATOMIC_SEQ_CST);                                                    \
    if (interrupted) {                                                                                                 \
      snprintf(errbuf, errbuflen, "interrupted");                                                                      \
      action;                                                                                                          \
    }                                                                                                                  \
  } while (0);

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

// Methods for a hash of (function ID, line number) -> (location)
// Needed because we have to intern a given function:line to a consistent location ID as per
// the protobuf spec.
// Key is a uint64_t[2].
static int intpair_st_hash_compare(st_data_t arg1, st_data_t arg2) {
  uint64_t *k1 = (uint64_t *)arg1;
  uint64_t *k2 = (uint64_t *)arg2;

  if (k1[0] != k2[0]) {
    return k1[0] < k2[0];
  } else {
    return k1[1] < k2[1];
  }
}

static st_index_t intpair_st_hash_hash(st_data_t arg) {
  uint64_t *k = (uint64_t *)arg;
  return st_hash(k, 2 * sizeof(uint64_t), FNV1_32A_INIT);
}

static const struct st_hash_type intpair_st_hash_type = {
    .compare = intpair_st_hash_compare,
    .hash = intpair_st_hash_hash,
};

// Initialize an already-allocated serialization context.
struct mpp_pprof_serctx *mpp_pprof_serctx_new(struct mpp_strtab *strtab, char *errbuf, size_t errbuflen) {
  struct mpp_pprof_serctx *ctx = mpp_xmalloc(sizeof(struct mpp_pprof_serctx));
  ctx->allocator.func = mpp_pprof_upb_arena_malloc;
  ctx->arena = upb_Arena_Init(NULL, 0, &ctx->allocator);
  ctx->profile_proto = perftools_profiles_Profile_new(ctx->arena);
  ctx->function_pbs = st_init_numtable();
  ctx->location_pbs = st_init_table(&intpair_st_hash_type);
  ctx->loc_counter = 1;
  ctx->interrupt = 0;

  // Intern some strings we'll need to produce our output
  mpp_strtab_intern_cstr(strtab, "count", &ctx->internstr_count, NULL);
  mpp_strtab_intern_cstr(strtab, "bytes", &ctx->internstr_bytes, NULL);
  mpp_strtab_intern_cstr(strtab, "retained_objects", &ctx->internstr_retained_objects, NULL);
  mpp_strtab_intern_cstr(strtab, "retained_size", &ctx->internstr_retained_size, NULL);

  // Build up the zero-based list of strings for interning.
  ctx->string_intern_index = mpp_strtab_index(strtab);
  MPP_ASSERT_MSG(ctx->string_intern_index, "mpp_strtab_index returned 0");

  // Set up the sample types etc.
  perftools_profiles_ValueType *retained_objects_vt =
      perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
  perftools_profiles_ValueType *retained_size_vt =
      perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
#define VT_SET_STRINTERN_FIELD(vt, field, str)                                                                         \
  do {                                                                                                                 \
    int64_t interned = mpp_strtab_index_of(ctx->string_intern_index, (str));                                           \
    if (interned == -1) {                                                                                              \
      ruby_snprintf(errbuf, errbuflen, "non-interned string %s passed for ValueType.#field", (str));                   \
      mpp_pprof_serctx_destroy(ctx);                                                                                   \
      return 0;                                                                                                        \
    }                                                                                                                  \
    perftools_profiles_ValueType_set_##field(vt, interned);                                                            \
  } while (0)

  VT_SET_STRINTERN_FIELD(retained_objects_vt, type, ctx->internstr_retained_objects);
  VT_SET_STRINTERN_FIELD(retained_objects_vt, unit, ctx->internstr_count);
  VT_SET_STRINTERN_FIELD(retained_size_vt, type, ctx->internstr_retained_size);
  VT_SET_STRINTERN_FIELD(retained_size_vt, unit, ctx->internstr_bytes);
#undef VT_SET_STRINTERN_FIELD

  return ctx;
}

// Destroys the serialization context. After this call, any stringtab indexes it held
// are released, and any memory from its internal state is freed. *ctx itself is also
// freed and must not be dereferenced after this.
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx) {
  upb_Arena_Free(ctx->arena);
  if (ctx->string_intern_index) {
    mpp_strtab_index_destroy(ctx->string_intern_index);
  }
  if (ctx->function_pbs) {
    st_free_table(ctx->function_pbs);
  }
  if (ctx->location_pbs) {
    st_free_table(ctx->location_pbs);
  }
  mpp_free(ctx);
}

struct mpp_pprof_serctx_map_add_ctx {
  struct mpp_pprof_serctx *ctx;
  int is_error;
  char *errbuf;
  size_t errbuflen;

  const char *function_name;
  size_t function_name_len;
  const char *file_name;
  size_t file_name_len;

  // for add_location
  int line_number;
  unsigned long location_id_out;
};

static int mpp_pprof_serctx_add_function(st_data_t *key, st_data_t *value, st_data_t data, int existing) {
  if (*value) {
    // already exists in the map, don't create it again.
    return ST_CONTINUE;
  }

  struct mpp_pprof_serctx_map_add_ctx *thunkctx = (struct mpp_pprof_serctx_map_add_ctx *)data;
  struct perftools_profiles_Function *fn_proto =
      perftools_profiles_Profile_add_function(thunkctx->ctx->profile_proto, thunkctx->ctx->arena);

  perftools_profiles_Function_set_id(fn_proto, (uintptr_t)thunkctx->function_name);

#define FN_SET_STRINTERN_FIELD(field, str)                                                                             \
  do {                                                                                                                 \
    int64_t interned = mpp_strtab_index_of(thunkctx->ctx->string_intern_index, (str));                                 \
    if (interned == -1) {                                                                                              \
      ruby_snprintf(thunkctx->errbuf, thunkctx->errbuflen, "non-interned string %s passed for Function.#field",        \
                    (str));                                                                                            \
      thunkctx->is_error = 1;                                                                                          \
      return ST_CONTINUE;                                                                                              \
    }                                                                                                                  \
    perftools_profiles_Function_set_##field(fn_proto, interned);                                                       \
  } while (0)

  FN_SET_STRINTERN_FIELD(name, thunkctx->function_name);
  FN_SET_STRINTERN_FIELD(system_name, thunkctx->function_name);
  FN_SET_STRINTERN_FIELD(filename, thunkctx->file_name);

#undef FN_SET_STRINTERN_FIELD

  *value = (st_data_t)fn_proto;
  return ST_CONTINUE;
}

static int mpp_pprof_serctx_add_location(st_data_t *key, st_data_t *value, st_data_t data, int existing) {
  struct mpp_pprof_serctx_map_add_ctx *thunkctx = (struct mpp_pprof_serctx_map_add_ctx *)data;
  if (*value) {
    // already exists in the map, don't create it again.
    struct perftools_profiles_Location *existing_loc = (struct perftools_profiles_Location *)(*value);
    thunkctx->location_id_out = perftools_profiles_Location_id(existing_loc);
    return ST_CONTINUE;
  }

  struct perftools_profiles_Location *loc_proto =
      perftools_profiles_Profile_add_location(thunkctx->ctx->profile_proto, thunkctx->ctx->arena);

  perftools_profiles_Location_set_id(loc_proto, thunkctx->ctx->loc_counter++);
  perftools_profiles_Line *line_proto = perftools_profiles_Location_add_line(loc_proto, thunkctx->ctx->arena);
  perftools_profiles_Line_set_function_id(line_proto, (uintptr_t)thunkctx->function_name);
  perftools_profiles_Line_set_line(line_proto, (int64_t)thunkctx->line_number);

  thunkctx->location_id_out = perftools_profiles_Location_id(loc_proto);
  *value = (st_data_t)loc_proto;
  return ST_CONTINUE;
}

int mpp_pprof_serctx_add_sample(struct mpp_pprof_serctx *ctx, struct mpp_sample *sample, char *errbuf,
                                size_t errbuflen) {
  CHECK_IF_INTERRUPTED(return -1);

  size_t frames_count = sample->frames_count;
  perftools_profiles_Sample *sample_proto = perftools_profiles_Profile_add_sample(ctx->profile_proto, ctx->arena);
  uint64_t *location_ids = perftools_profiles_Sample_resize_location_id(sample_proto, frames_count, ctx->arena);

  // Protobuf needs to be in most-recent-call-first, and backtracie is also in that order.
  for (size_t i = 0; i < frames_count; i++) {
    // Create protobufs for function/location ID as needed.
    struct mpp_pprof_serctx_map_add_ctx thunkctx;
    thunkctx.ctx = ctx;
    thunkctx.errbuf = errbuf;
    thunkctx.errbuflen = errbuflen;
    thunkctx.is_error = 0;
    thunkctx.function_name = sample->frames[i].function_name;
    thunkctx.function_name_len = sample->frames[i].function_name_len;
    thunkctx.file_name = sample->frames[i].file_name;
    thunkctx.file_name_len = sample->frames[i].file_name_len;
    thunkctx.location_id_out = 0;
    thunkctx.line_number = sample->frames[i].line_number;

    // Using the file name pointer as the function ID like this looks suspect, but works
    // because all of the function names are interned in the same string interning table.
    uintptr_t function_id = (uintptr_t)sample->frames[i].function_name;

    st_update(ctx->function_pbs, function_id, mpp_pprof_serctx_add_function, (st_data_t)&thunkctx);
    if (thunkctx.is_error) {
      return -1;
    }
    uint64_t loc_key[2] = {function_id, sample->frames[i].line_number};
    st_update(ctx->location_pbs, (st_data_t)&loc_key, mpp_pprof_serctx_add_location, (st_data_t)&thunkctx);
    if (thunkctx.is_error) {
      return -1;
    }
    MPP_ASSERT_MSG(thunkctx.location_id_out, "missing location ID out!");
    location_ids[i] = thunkctx.location_id_out;
  }

  // Values are (retained_count, retained_size).
  perftools_profiles_Sample_add_value(sample_proto, 1, ctx->arena);
  perftools_profiles_Sample_add_value(sample_proto, (int64_t)sample->allocated_value_objsize, ctx->arena);
  return 0;
}

// Serializes the contained protobuf, and gzips the result. Writes a pointer to the memory in *buf_out,
// and its length to buflen_out. The returned pointer is freed when mpp_pprof_serctx_destroy() is called,
// and should NOT be individually freed by the caller in any way (and nor is it valid after the call
// to destroy()).
int mpp_pprof_serctx_serialize(struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf,
                               size_t errbuflen) {
  CHECK_IF_INTERRUPTED(return -1);

  // Include the string table in the output
  upb_StringView *stringtab_list_proto = perftools_profiles_Profile_resize_string_table(
      ctx->profile_proto, ctx->string_intern_index->str_list_len, ctx->arena);
  for (int64_t i = 0; i < ctx->string_intern_index->str_list_len; i++) {
    upb_StringView *stringtab_proto = &stringtab_list_proto[i];
    struct mpp_strtab_el *intern_tab_el = ctx->string_intern_index->str_list[i];
    stringtab_proto->data = intern_tab_el->str;
    stringtab_proto->size = intern_tab_el->str_len;
  }

  CHECK_IF_INTERRUPTED(return -1);

  // It looks like some codepaths might leak the protobuf_data pointer, but that's OK - it's in
  // the ctx->arena so it'll get freed when ctx does.
  size_t protobuf_data_len;
  char *protobuf_data = perftools_profiles_Profile_serialize(ctx->profile_proto, ctx->arena, &protobuf_data_len);

  CHECK_IF_INTERRUPTED(return -1);

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
  char *gzip_data = upb_Arena_Malloc(ctx->arena, out_chunk_size);
  size_t gzip_data_allocd_len = out_chunk_size;
  strm.avail_out = out_chunk_size;
  strm.next_out = (unsigned char *)gzip_data;
  while (true) {
    CHECK_IF_INTERRUPTED(goto zstream_free);

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
