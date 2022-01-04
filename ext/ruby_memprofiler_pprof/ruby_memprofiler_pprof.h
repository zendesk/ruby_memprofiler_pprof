#ifndef __RUBY_MEMPROFILER_PPROF_H
#define __RUBY_MEMPROFILER_PPROF_H

#include <pthread.h>
#include <stdint.h>

#include <ruby.h>
#include "upb/upb/upb.h"
#include "pprof.upb.h"

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


// These declarations just wrap some things from the standard library that should "always succeed", but call
// rb_sys_fail() if they fail to abort the program.
void *mpp_xmalloc(size_t sz);
void *mpp_realloc(void *mem, size_t newsz);
void mpp_free(void *mem);
void mpp_pthread_mutex_lock(pthread_mutex_t *m);
void mpp_pthread_mutex_unlock(pthread_mutex_t *m);
int mpp_pthread_mutex_trylock(pthread_mutex_t *m);
void mpp_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
void mpp_pthread_mutex_destroy(pthread_mutex_t *m);

// Need a handy assertion macro. It would be nice to re-use rb_bug for some of this, but that actually
// requires the GVL (it walks the Ruby stack frames, for one) and we (want to) run some code outside
// the GVL, so this assertion macro has to be threadsafe. So we just implement it in pretty much a
// similar way to how stdlib's assert() works (plus some stuff to prefix the gem name to the abort message
// so users know who is at fault).
__attribute__ ((noreturn))
void mpp_assert_fail(const char *msg, const char *assertion, const char *file, const char *line, const char *fn);
#define MPP_ASSERT_MSG(expr, msg)                                               \
    do {                                                                        \
        if ((expr) == 0) {                                                      \
            mpp_assert_fail((msg), #expr, __FILE__, "##__LINE__##", __func__);  \
        }                                                                       \
    } while (0)                                                                 \
// ======== STRTAB DECLARATIONS ========

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

// ======== BACKTRACE DECLARATIONS ========
struct mpp_rb_backtrace_frame {
    // The (null-terminated, interned) filename. Might just be "(native code)" or such
    // for C extensions. And its size, not including termination.
    const char *filename;
    size_t filename_len;
    // Line number in the filename. Might be zero if we can't compute it, or for C
    // extensions.
    int64_t line_number;
    // The (null terminated, interned) function name, and its size (not including termination)
    const char *function_name;
    size_t function_name_len;
    // The (null terminated, interned) label name. This might be the same as the function
    // name, or it might be something "block in <method>". And its size.
    const char *label;
    size_t label_len;
    // A "function id". This is a value that is supposed to uniquely identify the function
    // in this process, but has no meaning outside of the context of this particular process.
    // See backtrace.c for a description of how this is computed.
    uint64_t function_id;
    // A "location id" - this is supposed to uniquely identify a line of code (within the
    // context of this process, like function_id).
    uint64_t location_id;
};

struct mpp_rb_backtrace {
    // The array of frames - most recent call FIRST.
    struct mpp_rb_backtrace_frame *frames;
    int64_t frames_count;
    // Memory size of frames, which might actually be bigger than
    // sizeof(struct mpp_rb_backtrace_frame) * frames_count
    size_t memsize;
};

// Capture a (current) backtrace, and free it.
// Note that the free function does NOT free the struct mpp_rb_backtrace itself.
void mpp_rb_backtrace_init(struct mpp_rb_backtrace *bt, struct str_intern_tab *strtab);
void mpp_rb_backtrace_destroy(struct mpp_rb_backtrace *bt, struct str_intern_tab *strtab);
size_t mpp_rb_backtrace_memsize(struct mpp_rb_backtrace *bt);

// ======= MAIN DATA STRUCTURE DECLARATIONS ========
struct mpp_sample {
    // The backtrace for this sample
    struct mpp_rb_backtrace bt;
    // Sample has a refcount - because it's used both in the heap profiling and in the allocation profiling.
    int64_t refcount;
    // Next element in the allocation profiling sample list. DO NOT use this in the heap profiling table.
    struct mpp_sample *next_alloc;
};

// ======== PPROF SERIALIZATION ROUTINES ========

// Forward-declare the struct, so that it's opaque to C land (in the C++ file, it will be defined as holdling
// a reference to some protobuf classes).
struct pprof_serialize_state;

struct pprof_serialize_state *rmmp_pprof_serialize_init();
void rmmp_pprof_serialize_add_strtab(struct pprof_serialize_state *state, struct str_intern_tab_index *strtab_ix);
void rmmp_pprof_serialize_add_alloc_samples(struct pprof_serialize_state *state, struct mpp_sample *sample_list);
void rmmp_pprof_serialize_to_memory(struct pprof_serialize_state *state, char **outbuf, size_t *outlen, int *abort_flag);
void rmmp_pprof_serialize_destroy(struct pprof_serialize_state *state);

// ======== PROTO SERIALIZATION ROUTINES ========
struct mpp_pprof_serctx {
    // 1 if has been initialized, else zero
    int initialized;
    // Defines the allocation routine & memory arena used by this serialisation context. When the ctx
    // is destroyed, we free the entire arena, so no other (protobuf) memory needs to be individually
    // freed.
    upb_alloc allocator;
    upb_arena *arena;
    // String intern index; recall that holding this object does _not_ require that we have exclusive
    // use of the underlying string intern table, so it's safe for us to use this in a separate thread.
    struct str_intern_tab_index strindex;
    // Mapping of (uint64_t) -> 0 (so basically a set) for whether function & location IDs have already
    // been inserted into *profile_proto
    st_table *added_functions;
    st_table *added_locations;
    // The protobuf representation we are building up.
    perftools_profiles_Profile *profile_proto;

    // We need to keep interned copies of some strings that will wind up in the protobuf output.
    // This is so that we can put constant values like "allocations" and "count" into our pprof output
    // (the pprof format requires these strings to be in the string table along with the rest of them)
    const char *internstr_allocations;
    const char *internstr_count;
};

void mpp_pprof_serctx_init(struct mpp_pprof_serctx *ctx);
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx);
void mpp_pprof_serctx_set_strtab(struct mpp_pprof_serctx *ctx, struct str_intern_tab *strtab);
void mpp_pprof_serctx_add_sample(struct mpp_pprof_serctx *ctx, struct mpp_sample *sample);
int mpp_pprof_serctx_serialize(struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf, size_t errbuflen);
#ifdef __cplusplus
}
#endif

#endif
