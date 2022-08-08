
#include <string.h>

#include <ruby.h>
#include <ruby/st.h>

#include "ruby_memprofiler_pprof.h"

// This structure is essentially a slightly fancy set of strings. When you call mpp_strset_index_for_cstr(), you get an
// integer index representing that string, starting from zero for the first string given. If this is called twice with
// the same string, you will get the same number back.
//
// The twist is mpp_strset_index_for_pinned_value; when you call this, with a VALUE that is a T_STRING, we extract the
// cstring from the ruby string and assign it an integer index like normal. However, if you call it again with the same
// VALUE, we'll quickly look up the same integer _without having to hash the underlying string_. This should hopefully
// save some CPU cycles on string comparisons and FNV1 hashing in cases where the same VALUE is referenced many times.
//
// Finally, we also keep the reverse index of integer -> cstring, so that we can use this strset to construct a pprof
// protobuf string interning table in pprof_out.c
struct mpp_strset_cdata {
  VALUE str_hash;
  VALUE ptr_hash;
  VALUE reverse_index;
  int count;
};

static VALUE cStrset;
static void strset_cdata_gc_mark(void *ptr);
static void strset_gc_free(void *ptr);
static size_t strset_gc_memsize(const void *ptr);
#ifdef HAVE_RB_GC_MARK_MOVABLE
static void strset_cdata_gc_compact(void *ptr);
#endif
static const rb_data_type_t strset_cdata_type = {"strset_cdata",
                                                 {
                                                     strset_cdata_gc_mark,
                                                     strset_gc_free,
                                                     strset_gc_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
                                                     strset_cdata_gc_compact,
#endif
                                                     {0}, /* reserved */
                                                 },
                                                 /* parent, data, [ flags ] */
                                                 NULL,
                                                 NULL,
                                                 0};

void mpp_setup_strset_class() {
  VALUE mMemprofilerPprof = rb_const_get(rb_cObject, rb_intern("MemprofilerPprof"));
  cStrset = rb_define_class_under(mMemprofilerPprof, "Collector", rb_cObject);
  rb_undef_alloc_func(cStrset);
}

VALUE mpp_strset_new() {
  struct mpp_strset_cdata *strset_cdata;
  VALUE v = TypedData_Make_Struct(cStrset, struct mpp_strset_cdata, &strset_cdata_type, strset_cdata);

  strset_cdata->str_hash = rb_hash_new();
  strset_cdata->ptr_hash = rb_hash_new();
  strset_cdata->reverse_index = rb_ary_new();
  strset_cdata->count = 0;

  return v;
}

static void strset_cdata_gc_mark(void *ptr) {
  struct mpp_strset_cdata *strset = (struct mpp_strset_cdata *)ptr;
  rb_gc_mark_movable(strset->str_hash);
  rb_gc_mark_movable(strset->ptr_hash);
  rb_gc_mark_movable(strset->reverse_index);
}

static void strset_gc_free(void *ptr) {}

static size_t strset_gc_memsize(const void *ptr) { return sizeof(struct mpp_strset_cdata); }

static void strset_cdata_gc_compact(void *ptr) {
  struct mpp_strset_cdata *strset = (struct mpp_strset_cdata *)ptr;
  strset->str_hash = rb_gc_location(strset->str_hash);
  strset->ptr_hash = rb_gc_location(strset->ptr_hash);
  strset->reverse_index = rb_gc_location(strset->reverse_index);
}

void mpp_strset_free(struct mpp_strset *strset) {
  st_free_table(strset->ptr_hash);
  mpp_free(strset);
}

struct strset_update_ctx {
  struct mpp_strset *strset;
  int index_out;
};

static int strset_update_func(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  struct strset_update_ctx *ctx = (struct strset_update_ctx *)arg;
  if (!existing) {
    int new_index = ctx->strset->count++;
    *value = new_index;
    if (ctx->strset->reverse_index_capa <= new_index) {
      ctx->strset->reverse_index_capa *= 2;
      mpp_realloc(ctx->strset->reverse_index, ctx->strset->reverse_index_capa);
    }
    ctx->strset->reverse_index[value]
  }
  ctx->index_out = *value;
  return ST_CONTINUE;
}

static int ptrset_update_func(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  struct strset_update_ctx *ctx = (struct strset_update_ctx *)arg;
  if (existing) {
    ctx->index_out = *value;
    return ST_CONTINUE;
  }

  VALUE str_value = *key;
  char *str = rb_string_value_cstr(&str_value);
  int new_index = if (!existing) { *value = ctx->strset->count++; }
  ctx->index_out = *value;
  return ST_CONTINUE;
}

int mpp_strset_index_for_str(struct mpp_strset *strset, const char *str) {
  struct strset_update_ctx ctx = {
      .strset = strset,
      .index_out = -1,
  };
  st_update(strset->str_set, (st_data_t)str, strset_update_func, (st_data_t)&ctx);
  return ctx.index_out;
}

int mpp_strset_index_for_ptr(struct mpp_strset *strset, VALUE ptr) {}
