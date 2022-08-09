#include <ruby.h>
#include <ruby/st.h>
#include <zlib.h>

#include "pprof.upb.h"
#include "ruby/st.h"
#include "ruby_memprofiler_pprof.h"
#include "upb/upb.h"

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

// I copied this magic number out of st.c from Ruby.
#define FNV1_32A_INIT 0x811c9dc5

static st_index_t intpair_st_hash_hash(st_data_t arg) {
  uint64_t *k = (uint64_t *)arg;
  return st_hash(k, 2 * sizeof(uint64_t), FNV1_32A_INIT);
}

static const struct st_hash_type intpair_st_hash_type = {
    .compare = intpair_st_hash_compare,
    .hash = intpair_st_hash_hash,
};

// Methods for a hash of (string, len) -> (string tab index)
struct str_st_hash_key {
  const char *str;
  size_t str_len;
};

static int str_st_hash_compare(st_data_t arg1, st_data_t arg2) {
  struct str_st_hash_key *k1 = (struct str_st_hash_key *)arg1;
  struct str_st_hash_key *k2 = (struct str_st_hash_key *)arg2;

  size_t smaller_len = (k1->str_len > k2->str_len) ? k2->str_len : k1->str_len;
  int cmp = memcmp(k1->str, k2->str, smaller_len);

  if (cmp != 0 || k1->str_len == k2->str_len) {
    // Either: one of the first smaller_len bytes were different, or
    // they were the same, AND the lenghts are the same, which make them the same string.
    return cmp;
  }
  // The first smaller_len bytes are the same, but one is longer than the other.
  // The shorter string should be considered smaller.
  return k1->str_len > k2->str_len ? 1 : -1;
}

static st_index_t str_st_hash_hash(st_data_t arg) {
  struct str_st_hash_key *k = (struct str_st_hash_key *)arg;
  return st_hash(k->str, k->str_len, FNV1_32A_INIT);
}

static const struct st_hash_type str_st_hash_type = {
    .compare = str_st_hash_compare,
    .hash = str_st_hash_hash,
};

struct intern_string_hash_update_ctx {
  struct mpp_pprof_serctx *serctx;
  bool copy;
  bool did_retain_out;
  int index_out;
};

static int intern_string_hash_update(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  struct intern_string_hash_update_ctx *ctx = (struct intern_string_hash_update_ctx *)arg;
  if (existing) {
    // Value was already in the hash; just return its existing index.
    ctx->index_out = *value;
  } else {
    struct mpp_pprof_serctx *serctx = ctx->serctx;
    bool copy = ctx->copy;
    upb_Arena *arena = serctx->arena;
    // Value NOT already in the hash. We need to add it.
    // Need to actually create a new *key; the one passed into st_update is a stack pointer, which won't be valid after
    // intern_string returns.
    struct str_st_hash_key *new_key = upb_Arena_Malloc(arena, sizeof(struct str_st_hash_key));
    struct str_st_hash_key *old_key = (struct str_st_hash_key *)*key;
    const char *old_str = old_key->str;
    size_t old_str_len = old_key->str_len;
    // May also need to copy the string, if it's not already a pointer to the arena.
    if (copy) {
      char *new_str = upb_Arena_Malloc(arena, old_str_len + 1);
      memcpy(new_str, old_str, old_str_len);
      new_str[old_str_len] = '\0';
      new_key->str = new_str;
    } else {
      new_key->str = old_str;
      ctx->did_retain_out = true;
    }
    new_key->str_len = old_str_len;
    *key = (st_data_t)new_key;

    // Now increment the value
    *value = serctx->strings_counter++;
    ctx->index_out = *value;
  }
  return ST_CONTINUE;
}

static int intern_string(struct mpp_pprof_serctx *serctx, const char *str, size_t len) {
  struct str_st_hash_key key = {.str = str, .str_len = len};
  struct intern_string_hash_update_ctx ctx = {.serctx = serctx, .index_out = 0, .copy = true};
  st_update(serctx->strings, (st_data_t)&key, intern_string_hash_update, (st_data_t)&ctx);
  return ctx.index_out;
}

static int intern_scratch_buffer(struct mpp_pprof_serctx *serctx) {
  // If the string length is larger than the buffer capacity, clamp to capacity.
  // ->scratch_buffer_capa gets set as the return value of backtracie_*_cstr, which returns the number
  // of bytes that _would_ be needed to store the whole output.
  size_t str_len = serctx->scratch_buffer_strlen;
  if (str_len >= serctx->scratch_buffer_capa) {
    str_len = serctx->scratch_buffer_capa - 1;
  }
  struct str_st_hash_key key = {.str = serctx->scratch_buffer, .str_len = str_len};
  struct intern_string_hash_update_ctx ctx = {.serctx = serctx, .index_out = 0, .copy = false, .did_retain_out = false};
  st_update(serctx->strings, (st_data_t)&key, intern_string_hash_update, (st_data_t)&ctx);
  if (ctx.did_retain_out) {
    serctx->scratch_buffer = NULL;
    serctx->scratch_buffer_capa = 0;
    serctx->scratch_buffer_strlen = 0;
  }
  return ctx.index_out;
}

static void ensure_scratch_buffer(struct mpp_pprof_serctx *serctx) {
  if (!serctx->scratch_buffer) {
    serctx->scratch_buffer = upb_Arena_Malloc(serctx->arena, 256);
    serctx->scratch_buffer_capa = 256;
    serctx->scratch_buffer_strlen = 0;
  }
}

// Initialize an already-allocated serialization context.
struct mpp_pprof_serctx *mpp_pprof_serctx_new(char *errbuf, size_t errbuflen) {
  struct mpp_pprof_serctx *ctx = mpp_xmalloc(sizeof(struct mpp_pprof_serctx));
  ctx->allocator.func = mpp_pprof_upb_arena_malloc;
  ctx->arena = upb_Arena_Init(NULL, 0, &ctx->allocator);
  ctx->profile_proto = perftools_profiles_Profile_new(ctx->arena);
  ctx->function_pbs = st_init_numtable();
  ctx->location_pbs = st_init_table(&intpair_st_hash_type);
  ctx->strings = st_init_table(&str_st_hash_type);
  ctx->loc_counter = 1;
  ctx->strings_counter = 0;
  ctx->interrupt = 0;
  ctx->scratch_buffer = NULL;
  ctx->scratch_buffer_capa = 0;
  ctx->scratch_buffer_strlen = 0;

  // Pprof requires that "" be interned at position 0 in the string table, so intern that now.
  intern_string(ctx, "", 0);

  // Set up the sample types etc.
  perftools_profiles_ValueType *retained_objects_vt =
      perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
  perftools_profiles_ValueType_set_type(retained_objects_vt,
                                        intern_string(ctx, "retained_objects", strlen("retained_objects")));
  perftools_profiles_ValueType_set_unit(retained_objects_vt, intern_string(ctx, "count", strlen("count")));
  perftools_profiles_ValueType *retained_size_vt =
      perftools_profiles_Profile_add_sample_type(ctx->profile_proto, ctx->arena);
  perftools_profiles_ValueType_set_type(retained_size_vt, intern_string(ctx, "retained_size", strlen("retained_size")));
  perftools_profiles_ValueType_set_unit(retained_size_vt, intern_string(ctx, "bytes", strlen("bytes")));

  return ctx;
}

// Destroys the serialization context. After this call, any stringtab indexes it held
// are released, and any memory from its internal state is freed. *ctx itself is also
// freed and must not be dereferenced after this.
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx) {
  if (ctx->function_pbs) {
    st_free_table(ctx->function_pbs);
  }
  if (ctx->location_pbs) {
    st_free_table(ctx->location_pbs);
  }
  if (ctx->strings) {
    st_free_table(ctx->strings);
  }
  upb_Arena_Free(ctx->arena);
  mpp_free(ctx);
}

struct mpp_pprof_serctx_map_add_ctx {
  struct mpp_pprof_serctx *ctx;
  int is_error;
  char *errbuf;
  size_t errbuflen;

  int function_name;
  int file_name;
  unsigned long function_id;

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

  perftools_profiles_Function_set_id(fn_proto, thunkctx->function_id);

  perftools_profiles_Function_set_name(fn_proto, thunkctx->function_name);
  perftools_profiles_Function_set_system_name(fn_proto, thunkctx->function_name);
  perftools_profiles_Function_set_filename(fn_proto, thunkctx->file_name);

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
  perftools_profiles_Line_set_function_id(line_proto, thunkctx->function_id);
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
    thunkctx.location_id_out = 0;

    // Intern the frame names & filenames.
    ensure_scratch_buffer(ctx);
    ctx->scratch_buffer_strlen =
        mpp_sample_frame_function_name(sample, i, ctx->scratch_buffer, ctx->scratch_buffer_capa);
    thunkctx.function_name = intern_scratch_buffer(ctx);

    ensure_scratch_buffer(ctx);
    ctx->scratch_buffer_strlen = mpp_sample_frame_file_name(sample, i, ctx->scratch_buffer, ctx->scratch_buffer_capa);
    thunkctx.file_name = intern_scratch_buffer(ctx);

    thunkctx.line_number = mpp_sample_frame_line_number(sample, i);
    thunkctx.function_id = mpp_sample_frame_function_id(sample, i);
    st_update(ctx->function_pbs, thunkctx.function_id, mpp_pprof_serctx_add_function, (st_data_t)&thunkctx);
    if (thunkctx.is_error) {
      return -1;
    }
    uint64_t loc_key[2] = {thunkctx.function_id, thunkctx.line_number};
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

static int write_each_string_table_entry(st_data_t key, st_data_t value, st_data_t arg) {
  struct str_st_hash_key *string_key = (struct str_st_hash_key *)key;
  upb_StringView *stringtab_list_proto = (upb_StringView *)arg;
  int index = value;
  stringtab_list_proto[index].data = string_key->str;
  stringtab_list_proto[index].size = string_key->str_len;
  return ST_CONTINUE;
}

// Serializes the contained protobuf, and gzips the result. Writes a pointer to the memory in *buf_out,
// and its length to buflen_out. The returned pointer is freed when mpp_pprof_serctx_destroy() is called,
// and should NOT be individually freed by the caller in any way (and nor is it valid after the call
// to destroy()).
int mpp_pprof_serctx_serialize(struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf,
                               size_t errbuflen) {
  CHECK_IF_INTERRUPTED(return -1);

  // Include the string table in the output
  upb_StringView *stringtab_list_proto =
      perftools_profiles_Profile_resize_string_table(ctx->profile_proto, ctx->strings_counter, ctx->arena);
  st_foreach(ctx->strings, write_each_string_table_entry, (st_data_t)stringtab_list_proto);

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
