#include <pthread.h>
#include <string.h>

#include <ruby.h>
#include <ruby/st.h>

#include "ruby_memprofiler_pprof.h"

static int mpp_strtab_strcompare(st_data_t arg1, st_data_t arg2) {
  struct mpp_strtab_key *k1 = (struct mpp_strtab_key *)arg1;
  struct mpp_strtab_key *k2 = (struct mpp_strtab_key *)arg2;

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

static st_index_t mpp_strtab_strhash(st_data_t arg) {
  struct mpp_strtab_key *k = (struct mpp_strtab_key *)arg;
  return st_hash(k->str, k->str_len, FNV1_32A_INIT);
}

static const struct st_hash_type mpp_strtab_hash_type = {
    .compare = mpp_strtab_strcompare,
    .hash = mpp_strtab_strhash,
};

// Frees the memory associated with a single struct str_intern_tab_el
static void mpp_strtab_free_intern_tab_el(struct mpp_strtab_el *el) {
  mpp_free(el->str);
  mpp_free(el);
}

struct mpp_strtab_table_extract_els_args {
  struct mpp_strtab_el **el_ary;
  int64_t el_ary_len;
  int64_t el_ary_capa;
  int should_delete;
};

// Used in st_foreach to extract every table element to an array. If arg has should_delete set, then
// this will also remove it from the table too.
static int mpp_strtab_table_extract_els(st_data_t key, st_data_t value, st_data_t arg) {
  struct mpp_strtab_el *el = (struct mpp_strtab_el *)value;
  struct mpp_strtab_table_extract_els_args *args = (struct mpp_strtab_table_extract_els_args *)arg;

  MPP_ASSERT_MSG(args->el_ary_len < args->el_ary_capa,
                 "strtab: array passed in to mpp_strtab_table_extract_els not big enough?");

  args->el_ary[args->el_ary_len] = el;
  args->el_ary_len++;

  if (args->should_delete) {
    return ST_DELETE;
  } else {
    el->refcount++;
    return ST_CONTINUE;
  }
}

struct mpp_strtab_table_decrement_el_refcount_args {
  struct mpp_strtab_el *el;
};

// Used in st_update to decrement the refcount of a table entry; if the refcount drops to zero, the
// struct str_intern_tab_el is freed and the entry removed from the table.
static int mpp_strtab_table_decrement_el_refcount(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  MPP_ASSERT_MSG(existing && value, "strtab: attempted to decrement refcount on non-present element");
  struct mpp_strtab_el *el = (struct mpp_strtab_el *)*value;

  // We need to store the value we are operating on back in the *args array, so our caller can free the element
  // if its refcount dropped to zero.
  struct mpp_strtab_table_decrement_el_refcount_args *args = (struct mpp_strtab_table_decrement_el_refcount_args *)arg;
  args->el = el;

  MPP_ASSERT_MSG(el->refcount > 0, "strtab: attempted to decrement refcount below zero");
  el->refcount--;

  // Remvoe it from the table if it was the last reference.
  return el->refcount == 0 ? ST_DELETE : ST_CONTINUE;
}

// Allocates and configures a struct str_intern_tab.
struct mpp_strtab *mpp_strtab_new() {
  struct mpp_strtab *tab = mpp_xmalloc(sizeof(struct mpp_strtab));
  tab->table = st_init_table(&mpp_strtab_hash_type);
  tab->table_count = 0;
  tab->table_entry_size = 0;

  // According to pprof rules, every string table needs ""
  mpp_strtab_intern(tab, "", 0, &tab->interned_empty_str, NULL);

  return tab;
}

// Immediately frees all memory held by *tab. After this call, any referneces to interned strings outside
// of this module are dangling. Also frees *tab itself.
void mpp_strtab_destroy(struct mpp_strtab *tab) {

  // We need to first copy all the elements of the table into an array, and _then_ remove them from the table,
  // and only then free the elements. This is because we use the element str pointer as the key of the table,
  // and freeing that before removing the element from the table would mean that it would do some use-after-free.
  struct mpp_strtab_table_extract_els_args table_loop_args;
  table_loop_args.should_delete = 1;
  table_loop_args.el_ary_capa = tab->table_count;
  table_loop_args.el_ary_len = 0;
  table_loop_args.el_ary = mpp_xmalloc(tab->table_count * sizeof(struct mpp_strtab_el *));
  st_foreach(tab->table, mpp_strtab_table_extract_els, (st_data_t)&table_loop_args);

  // Now we can free the table elements themselves, and the memory they hold
  for (int64_t i = 0; i < table_loop_args.el_ary_len; i++) {
    tab->table_count--;
    tab->table_entry_size -= sizeof(struct mpp_strtab_el) + table_loop_args.el_ary[i]->str_len + 1;
    mpp_strtab_free_intern_tab_el(table_loop_args.el_ary[i]);
  }
  mpp_free(table_loop_args.el_ary);

  // And _now_ it's finally OK to delete the table itself.
  st_free_table(tab->table);
  mpp_free(tab);
}

// Get the memory size of the table, for use in reporting the struct memsize to Ruby.
// Does NOT include sizeof(tab).
size_t mpp_strtab_memsize(struct mpp_strtab *tab) { return st_memsize(tab->table) + tab->table_entry_size; }

// Implementation for interning a string. Interns the string str, and writes the interned string pointer to
// *interned_str_out and the length (not including null terminator) to *interned_str_len_out.
static void mpp_strtab_intern_impl(struct mpp_strtab *tab, const char *str, int str_len, const char **interned_str_out,
                                   size_t *interned_str_len_out) {
  // Consider interning a NULL string as the same as an "" empty string.
  if (!str) {
    str = "";
  }

  // First - is str already in the table?
  struct mpp_strtab_el *interned_value;
  struct mpp_strtab_key lookup_key = {
      .str = str,
      .str_len = str_len,
  };
  if (st_lookup(tab->table, (st_data_t)&lookup_key, (st_data_t *)&interned_value)) {
    // Yes it is. Increment its refcount and return its interned value.
    interned_value->refcount++;
    *interned_str_out = interned_value->str;
    if (interned_str_len_out) {
      *interned_str_len_out = interned_value->str_len;
    }
    return;
  }

  // It's not - we need to intern it then.
  interned_value = mpp_xmalloc(sizeof(struct mpp_strtab_el));
  // We always intern a _copy_ of the string, so that the caller is free to dispose of str as they will.
  // We also null-terminate our copy, since that's convenient and free for us to do at this point.
  interned_value->str_len = str_len;
  interned_value->str = mpp_xmalloc(interned_value->str_len + 1);
  memcpy(interned_value->str, str, interned_value->str_len);
  interned_value->str[interned_value->str_len] = '\0';

  // The key is actually embedded in the value. Set it up.
  interned_value->key.str_len = interned_value->str_len;
  interned_value->key.str = interned_value->str;

  // Refcount starts at one (from this call)
  interned_value->refcount = 1;

  int r = st_insert(tab->table, (st_data_t)&interned_value->key, (st_data_t)interned_value);
  MPP_ASSERT_MSG(r == 0, "strtab: attempted to overwrite intern entry");

  tab->table_count++;
  tab->table_entry_size += sizeof(*interned_value) + interned_value->str_len + 1;
  *interned_str_out = interned_value->str;
  if (interned_str_len_out) {
    *interned_str_len_out = interned_value->str_len;
  }
}

// Interns the string str into this string table, writing an interned pointer to the string to interned_str_out.
// Notes:
//     - str need not be null terminated.
//     - str_len is the length of str, NOT INCLUDING any null termination.
//     - Alternatively, if str_len is MPP_STRTAB_USE_STRLEN, then it is calculated by calling strlen(str) (and
//       thus in this case, str MUST be null-terminated). This is really only designed for interning literals.
//     - Retains no reference to str; it's copied if needed. The caller is free to do what they wish with
//       str after this method returns.
//     - The returned *interned_str_out IS null terminated.
//     - The returned *interned_str_len_out is the length of *interned_str_out, NOT INCLUDING the null
//       termination.
void mpp_strtab_intern(struct mpp_strtab *tab, const char *str, int str_len, const char **interned_str_out,
                       size_t *interned_str_len_out) {
  if (str_len == MPP_STRTAB_USE_STRLEN) {
    str_len = (int)strlen(str);
  }
  mpp_strtab_intern_impl(tab, str, str_len, interned_str_out, interned_str_len_out);
}

// Does what mpp_strtab_intern does, but calculates the length from strlen()
void mpp_strtab_intern_cstr(struct mpp_strtab *tab, const char *str, const char **interned_str_out,
                            size_t *interned_str_len_out) {
  int str_len = (int)strlen(str);
  mpp_strtab_intern_impl(tab, str, str_len, interned_str_out, interned_str_len_out);
}

static VALUE mpp_strtab_stringify_value(VALUE val) { return rb_sprintf("%" PRIsVALUE, val); }

static void mpp_strtab_rbstr_to_tmpstr(VALUE rbstr, const char **str, int *str_len) {
  VALUE to_s_val;
  if (RB_TYPE_P(rbstr, RUBY_T_STRING)) {
    // Already a string.
    to_s_val = rbstr;
  } else {
    // Try and convert it.
    int tag = 0;
    VALUE original_ex = rb_errinfo();
    to_s_val = rb_protect(mpp_strtab_stringify_value, rbstr, &tag);
    if (tag) {
      // it threw. Make sure rb_erroinfo wasn't clobbered.
      rb_set_errinfo(original_ex);
      to_s_val = Qundef;
    }
  }

  if (RB_TYPE_P(to_s_val, RUBY_T_STRING)) {
    *str = RSTRING_PTR(to_s_val);
    *str_len = (int)RSTRING_LEN(to_s_val);
  } else {
    // to_s returned a non-string.
    *str = MPP_STRTAB_UNKNOWN_LITERAL;
    *str_len = MPP_STRTAB_UNKNOWN_LITERAL_LEN;
  }
}

// Interns the Ruby string rbstr into the table, copying it to native memory if required. Writes the interned
// pointer to a null-terminated c string *interned_str_out and the length (not including null terminator)
// to *interned_str_len_out.
// Notes:
//     - This will work fine even if rbstr has NULLs in it.
//     - Will convert rbstr to a char * via the following process:
//         - If it's an RSTRING, just use its value
//         - Otherwise, call #to_s on it, and use that value if that's an RSTRING.
//         - Otherwise, use a built-in default string.
//     - Retains no reference to rbstr; it's copied if needed. The caller is free to do what they wish with
//       str after this method returns.
//     - The returned *interned_str_out IS null terminated.
//     - The returned *interned_str_len_out is the length of *interned_str_out, NOT INCLUDING the null
//       termination.
//     - This needs to be called under the GVL, obviously, because it's using Ruby VALUEs.
void mpp_strtab_intern_rbstr(struct mpp_strtab *tab, VALUE rbstr, const char **interned_str_out,
                             size_t *interned_str_len_out) {
  const char *str;
  int str_len;
  mpp_strtab_rbstr_to_tmpstr(rbstr, &str, &str_len);
  mpp_strtab_intern(tab, str, str_len, interned_str_out, interned_str_len_out);
}

// Interns the string pointed to by *builder
void mpp_strtab_intern_strbuilder(struct mpp_strtab *tab, struct mpp_strbuilder *builder, const char **interned_str_out,
                                  size_t *interned_str_len_out) {
  size_t strsize = builder->attempted_size;
  if (strsize >= builder->original_bufsize) {
    strsize = builder->original_bufsize - 1;
  }
  mpp_strtab_intern(tab, builder->original_buf, strsize, interned_str_out, interned_str_len_out);
}

static void mpp_strtab_release_by_key(struct mpp_strtab *tab, struct mpp_strtab_key lookup_key) {
  struct mpp_strtab_table_decrement_el_refcount_args cb_args;
  cb_args.el = NULL;
  st_update(tab->table, (st_data_t)&lookup_key, mpp_strtab_table_decrement_el_refcount, (st_data_t)&cb_args);

  // If the found element had its refcount dropped to zero, free it; note that this _MUST_ happen _AFTER_
  // removing it from the table, because we use the str pointer on the element as the key in the table and
  // the comparison function will read freed memory if we free it before removing it.
  MPP_ASSERT_MSG(cb_args.el, "strtab: did not write the updated element to cb_args.el?");
  if (cb_args.el->refcount == 0) {
    tab->table_count--;
    tab->table_entry_size -= sizeof(*cb_args.el) + cb_args.el->str_len + 1;
    mpp_strtab_free_intern_tab_el(cb_args.el);
  }
}

// Releases a reference to the string str in the intern table;
void mpp_strtab_release(struct mpp_strtab *tab, const char *str, size_t str_len) {
  // Consider interning a NULL string as the same as an "" empty string.
  if (!str) {
    str = "";
  }

  struct mpp_strtab_key lookup_key = {
      .str = str,
      .str_len = str_len,
  };
  mpp_strtab_release_by_key(tab, lookup_key);
}

// Releases a reference to a ruby string str in the intern table; see mpp_strtab_intern_rbstr for details on how
// the conversion from VALUE to char * is done.
void mpp_strtab_release_rbstr(struct mpp_strtab *tab, VALUE rbstr) {
  const char *str;
  int str_len;
  mpp_strtab_rbstr_to_tmpstr(rbstr, &str, &str_len);
  mpp_strtab_release(tab, str, str_len);
}

// Creates a zero-based "list index" of the current contents of tab and stores it in *ix_out.
// This "list index" can be used to turn interned pointers into zero-based ordinals into the
// index as requried by the pprof protobuf format.
// Once the index structure is created, it is safe to use this structure concurrently with the
// table itself (i.e. so samples can continue to be collected in the profiler).
// Note that it is NOT safe to destroy the index concurrently with table use however.
struct mpp_strtab_index *mpp_strtab_index(struct mpp_strtab *tab) {
  struct mpp_strtab_index *ix = mpp_xmalloc(sizeof(struct mpp_strtab_index));

  // Accumulate a pointer to every element.
  struct mpp_strtab_table_extract_els_args table_loop_args;
  table_loop_args.should_delete = 0;
  table_loop_args.el_ary_len = 0;
  table_loop_args.el_ary_capa = tab->table_count;
  table_loop_args.el_ary = mpp_xmalloc(tab->table_count * sizeof(struct mpp_strtab_el *));
  // This _also_ has the effect of adding one to the refcount of each item in the table.
  st_foreach(tab->table, mpp_strtab_table_extract_els, (st_data_t)&table_loop_args);

  // Just save the list straight on the index - the index owns that now.
  ix->str_list = table_loop_args.el_ary;
  ix->str_list_len = table_loop_args.el_ary_len;
  ix->pos_table = st_init_numtable();
  ix->tab = tab;

  // According to pprof rules, every string table needs to have "" at position zero for some reason.
  // Keep track of where "" winds up so we can swap it afterwards intl position 0.
  int64_t emptystr_index = -1;

  for (int64_t i = 0; i < ix->str_list_len; i++) {
    struct mpp_strtab_el *el = ix->str_list[i];
    // Insert it into the interned ptr table.
    int r = st_insert(ix->pos_table, (st_data_t)el->str, i);
    MPP_ASSERT_MSG(r == 0, "strtab: duplicate entry while building pos_table?");
    if (el->str == tab->interned_empty_str) {
      emptystr_index = i;
    }
  }

  // Swap whatever's in 0 with wherever "" is.
  MPP_ASSERT_MSG(emptystr_index >= 0, "strtab: empty was not present while building pos_table?");
  struct mpp_strtab_el *tmp = ix->str_list[0];
  ix->str_list[0] = ix->str_list[emptystr_index];
  ix->str_list[emptystr_index] = tmp;
  // st_insert also does update!
  st_insert(ix->pos_table, (st_data_t)tab->interned_empty_str, 0);
  st_insert(ix->pos_table, (st_data_t)tmp->str, emptystr_index);

  return ix;
}

// Destroys a previously created index. Must not be called concurrently with any other method on
// the stringtab or index. Does NOT free the memory ix itself.
void mpp_strtab_index_destroy(struct mpp_strtab_index *ix) {
  // As far as st_hash is concerned, ix->pos_table is just a mapping of numbers -> numbers; it does
  // not dereference the pointers in any way, so it's safe to just destroy the hash right off the bat
  // without carefully removing the elements first, unlike tab->table.
  st_free_table(ix->pos_table);

  // Decrement refcounts & maybe free them from the underlying tab.
  for (int64_t i = 0; i < ix->str_list_len; i++) {
    struct mpp_strtab_el *el = ix->str_list[i];
    mpp_strtab_release_by_key(ix->tab, el->key);
  }

  mpp_free(ix->str_list);
}

// Returns the index of the provided interned pointer in our list, or else -1.
int64_t mpp_strtab_index_of(struct mpp_strtab_index *ix, const char *interned_ptr) {
  int64_t found_index;
  int r = st_lookup(ix->pos_table, (st_data_t)interned_ptr, (st_data_t *)&found_index);
  if (!r) {
    return -1;
  }
  return found_index;
}
