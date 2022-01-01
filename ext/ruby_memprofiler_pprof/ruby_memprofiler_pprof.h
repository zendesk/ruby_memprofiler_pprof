#ifndef __RUBY_MEMPROFILER_PPROF_H
#define __RUBY_MEMPROFILER_PPROF_H

#include <pthread.h>
#include <stdint.h>

#include <ruby.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======== COMPAT DECLARATIONS ========

// For handling differences in ruby versions
#ifndef HAVE_RB_GC_MARK_MOVABLE
#define rb_gc_mark_moveable(v) rb_gc_mark(v)
#endif

#ifndef RB_PASS_KEYWORDS
#define rb_scan_args_kw(kw, c, v, s, ...) rb_scan_args(c, v, s, __VA_ARGS__)
#endif

#ifndef HAVE_RB_EXT_RACTOR_SAFE
#define rb_ext_ractor_safe(x) do {} while (0)
#endif

// Apparently "I just want a random number, without thinking about whether it's
// threadsafe, without thinking about whether some other part of the process needs
// the global seed to be set to some deterministic value, and without calling into
// the kernel every time" is... too much to ask for.
// BSD has arc4random(3) for this, but for glibc we have to use one of the threadsafe
// RNG's and seed a global instance of it, guarded by a mutex....
// These methods wrap all that rubbish up.
uint32_t mpp_rand();
void mpp_rand_init();


// These declarations just wrap some things from the standard librar that should "always succeed", but call
// rb_sys_fail() if they fail to abort the program.
void *mpp_xmalloc(size_t sz);
void mpp_free(void *mem);
void mpp_pthread_mutex_lock(pthread_mutex_t *m);
void mpp_pthread_mutex_unlock(pthread_mutex_t *m);
int mpp_pthread_mutex_trylock(pthread_mutex_t *m);
void mpp_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
void mpp_pthread_mutex_destroy(pthread_mutex_t *m);

// ======= MAIN DATA STRUCTURE DECLARATIONS ========
struct internal_sample_bt_frame {
    const char *filename;           // Filename
    uint64_t lineno;                // Line number
    const char *function_name;      // Method name
    uint64_t function_id;           // An opaque ID we will stuff in the pprof proto file for this fn
    uint64_t location_id;           // An opaque ID we will stuff in the pprof proto file for this location.
};

struct internal_sample {
    struct internal_sample_bt_frame *bt_frames; // Array of backtrace frames
    size_t bt_frames_count;                     // Number of backtrace frames
    int refcount;                               // Sample has a refcount - because it's used both in the heap profiling
    //   and in the allocation profiling
    struct internal_sample *next_alloc;         // Next element in the allocation profiling sample list. DO NOT use
    //   this in the heap profiling table.
};

// ======= STRTAB DECLARATIONS ========

#define MPP_STRTAB_USE_STRLEN (-1)
#define MPP_STRTAB_UNKNOWN_LITERAL "(unknown)"
#define MPP_STRTAB_UNKNOWN_LITERAL_LEN ((int)strlen(MPP_STRTAB_UNKNOWN_LITERAL))

// Specialisation of the st_hash for "strings with length".
struct str_intern_tab_key {
    const char *str;    // Pointer to the string; n.b. it does NOT need a null-terminator
    size_t str_len;     // Size of the string, not including any null terminator.
};

// I copied this magic number out of st.c from Ruby.
#define FNV1_32A_INIT 0x811c9dc5

struct str_intern_tab_el {
    // Pointer to null-terminated string that has been interned
    char *str;
    // Length of str, NOT INCLUDING the null terminator
    size_t str_len;
    // Number of times this string has been interned. When the refcount drops to zero, the string
    // is removed from the table and str is free'd.
    uint64_t refcount;
    // We cleverly keep the key _inside_ the value, so we don't need a separate bunch of malloc'd
    // memory for each _key_ as well as each _value_.
    struct str_intern_tab_key key;
};

struct str_intern_tab {
    // 1 if the table is initialized, else zero
    int initialized;
    // The actual table which contains a mapping of (string hash) -> (str_intern_tab_el *)
    st_table *table;
    // Number of entries in table.
    int64_t table_count;
    // (approximate) allocated size of the _entries_ in the st_table (but not the st_table
    // itself).
    size_t table_entry_size;
    // Value of "" interned in the table.
    const char *interned_empty_str;
};

struct str_intern_tab_index {
    // The table this index is a part of and came from.
    struct str_intern_tab *tab;
    // The list & length of interned strings.
    struct str_intern_tab_el **str_list;
    int64_t str_list_len;
    // This st_hash is used to convert already-interned pointers to index into str_list.
    // It's a map of (uintptr_t) -> (int64_t)
    st_table *pos_table;
};


// NOTE - there's better documentation of what these methods do in strtab.c itself.

// Initializes a new, empty string intern table. This will allocate memory that remains owned by the strtab
// module and saves it in tab. It does _not_ allocate memory for struct intern_tab itself.
void mpp_strtab_init(struct str_intern_tab *tab);

// Destroys a string intern table, including freeing the underlying memory used by tab, but NOT freeing
// any memory pointed to by tab itself.
void mpp_strtab_destroy(struct str_intern_tab *tab);

// Get the size of all memory used by the table
size_t mpp_strtab_memsize(struct str_intern_tab *tab);

// Intern new strings (or increment the refcount of already-interned ones)
void mpp_strtab_intern(
    struct str_intern_tab *tab, const char *str, int str_len,
    const char **interned_str_out, size_t *interned_str_len_out
);
void mpp_strtab_intern_rbstr(
    struct str_intern_tab *tab, VALUE rbstr,
    const char **interned_str_out, size_t *interned_str_len_out
);

// Decrement the refcount of elements in the intern table.
void mpp_strtab_release(struct str_intern_tab *tab, const char *str, size_t str_len);
void mpp_strtab_release_rbstr(struct str_intern_tab *tab, VALUE rbstr);

// Methods for building a zero=-based list of interned pointers, for building the final string table
// in the pprof protobuf.
void mpp_strtab_index(struct str_intern_tab *tab, struct str_intern_tab_index *ix);
void mpp_strtab_index_destroy(struct str_intern_tab_index *ix);
int64_t mpp_strtab_index_of(struct str_intern_tab_index *ix, const char *interned_ptr);
typedef void (*mpp_strtab_each_fn)(int64_t el_ix, const char *interned_str, size_t interned_str_len, void *ctx);
void mpp_strtab_each(struct str_intern_tab_index *ix, mpp_strtab_each_fn fn, void *ctx);

// ======== PPROF SERIALIZATION ROUTINES ========

// Forward-declare the struct, so that it's opaque to C land (in the C++ file, it will be defined as holdling
// a reference to some protobuf classes).
struct pprof_serialize_state;

struct pprof_serialize_state *rmmp_pprof_serialize_init();
void rmmp_pprof_serialize_add_strtab(struct pprof_serialize_state *state, struct str_intern_tab_index *strtab_ix);
void rmmp_pprof_serialize_add_alloc_samples(struct pprof_serialize_state *state, struct internal_sample *sample_list);
void rmmp_pprof_serialize_to_memory(struct pprof_serialize_state *state, char **outbuf, size_t *outlen, int *abort_flag);
void rmmp_pprof_serialize_destroy(struct pprof_serialize_state *state);

#ifdef __cplusplus
}
#endif

#endif
