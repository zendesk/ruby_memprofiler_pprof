#ifndef __RUBY_MEMPROFILER_PPROF_H
#define __RUBY_MEMPROFILER_PPROF_H

#include "extconf.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <ruby.h>

// UPB header files trip up a BUNCH of -Wshorten-64-to-32
// Also ignore -Wpragmas so that if -Wshorten-64-to-32 isn't present
// (it's a clang only thing), GCC doesn't warn about the unknown warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#include <upb/upb.h>
#include "pprof.upb.h"
#pragma GCC diagnostic pop


// ======== COMPAT DECLARATIONS ========

// For handling differences in ruby versions
#ifndef HAVE_RB_GC_MARK_MOVABLE
#define rb_gc_mark_movable(v) rb_gc_mark(v)
#endif

#ifndef RB_PASS_KEYWORDS
#define RB_SCAN_ARGS_LAST_HASH_KEYWORDS 3
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
// our assertion macro if they fail to abort the program.
void *mpp_xmalloc(size_t sz);
void *mpp_realloc(void *mem, size_t newsz);
void mpp_free(void *mem);
void mpp_pthread_mutex_lock(pthread_mutex_t *m);
void mpp_pthread_mutex_unlock(pthread_mutex_t *m);
int mpp_pthread_mutex_trylock(pthread_mutex_t *m);
void mpp_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
void mpp_pthread_mutex_destroy(pthread_mutex_t *m);
void mpp_pthread_mutexattr_init(pthread_mutexattr_t *a);
void mpp_pthread_mutexattr_destroy(pthread_mutexattr_t *a);
void mpp_pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
void mpp_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

// Need a handy assertion macro. It would be nice to re-use rb_bug for some of this, but that actually
// requires the GVL (it walks the Ruby stack frames, for one) and we (want to) run some code outside
// the GVL, so this assertion macro has to be threadsafe. So we just implement it in pretty much a
// similar way to how stdlib's assert() works (plus some stuff to prefix the gem name to the abort message
// so users know who is at fault).
#define MPP_ASSERT_STRINGIFY1(x) #x
#define MPP_ASSERT_STRINGIFY2(x) MPP_ASSERT_STRINGIFY1(x)
#define MPP_ASSERT__LINE MPP_ASSERT_STRINGIFY2(__LINE__)
__attribute__ ((noreturn))
void mpp_assert_fail(const char *msg, const char *assertion, const char *file, const char *line, const char *fn);
#define MPP_ASSERT_MSG(expr, msg)                                                   \
    do {                                                                            \
        if ((expr) == 0) {                                                          \
            mpp_assert_fail((msg), #expr, __FILE__, MPP_ASSERT__LINE, __func__);    \
        }                                                                           \
    } while (0)
#define MPP_ASSERT_FAIL(msg) MPP_ASSERT_MSG(0, msg)

// Log a debug message to "somewhere". This could be smarter in future, but for now, this'll do.
// The implementation here does not depend on holding the GVL.
// Will automatically add a trailing newline.
void mpp_log_debug(const char *pattern, ...);

// ======== RUBY HACKS DECLARATIONS ========

// An implementation of rb_gc_disable_no_rest; disables the GC without itself triggering
// the finalisation of the current sweep phase of the GC.
VALUE mpp_rb_gc_disable_no_rest();
// An implementation of rb_obj_memsize_of; tells us how big an object is.
VALUE mpp_rb_obj_memsize_of(VALUE obj);
// Tells us whether the given VALUE is valid enough still for rb_obj_memsize_of to
// work on it.
bool mpp_is_value_still_validish(VALUE obj);
// Is some other thread blocked waiting for the GVL?
bool mpp_is_someone_else_waiting_for_gvl();
// Like rb_ivar_set, but ignore frozen status.
VALUE mpp_rb_ivar_set_ignore_frozen(VALUE obj, ID key, VALUE value);

// ======== STRBUILDER DECLARATIONS ========
struct mpp_strbuilder {
    char *original_buf;
    char *curr_ptr;
    size_t original_bufsize;
    size_t attempted_size;
};

void mpp_strbuilder_append(struct mpp_strbuilder *str, const char *cat);
void mpp_strbuilder_appendf(struct mpp_strbuilder *str, const char *fmt, ...);
void mpp_strbuilder_append_value(struct mpp_strbuilder *str, VALUE val);
VALUE mpp_strbuilder_to_value(struct mpp_strbuilder *str);
void mpp_strbuilder_init(struct mpp_strbuilder *str, char *buf, size_t bufsize);

// ======== STRTAB DECLARATIONS ========

#define MPP_STRTAB_USE_STRLEN (-1)
#define MPP_STRTAB_UNKNOWN_LITERAL "(unknown)"
#define MPP_STRTAB_UNKNOWN_LITERAL_LEN ((int)strlen(MPP_STRTAB_UNKNOWN_LITERAL))

// Specialisation of the st_hash for "strings with length".
struct mpp_strtab_key {
    const char *str;    // Pointer to the string; n.b. it does NOT need a null-terminator
    size_t str_len;     // Size of the string, not including any null terminator.
};

// I copied this magic number out of st.c from Ruby.
#define FNV1_32A_INIT 0x811c9dc5

struct mpp_strtab_el {
    // Pointer to null-terminated string that has been interned
    char *str;
    // Length of str, NOT INCLUDING the null terminator
    size_t str_len;
    // Number of times this string has been interned. When the refcount drops to zero, the string
    // is removed from the table and str is free'd.
    uint64_t refcount;
    // We cleverly keep the key _inside_ the value, so we don't need a separate bunch of malloc'd
    // memory for each _key_ as well as each _value_.
    struct mpp_strtab_key key;
};

// struct mpp_strtab is a string interning table; interning strings with one of the mpp_strtab_intern* methods
// will return a C string which is guaranteed to be the same pointer as any other "equal" string that was interned.
// Interning the same string multiple times bumps its refcount; a string can also be released (with the
// mpp_strtab_release* methods) to decrement its refcount. When the refcount is zero, the string is freed from the
// intering table.
struct mpp_strtab {
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

// A mpp_strtab_index is a view on a strtab which assigns a number from zero to N for every string in the table.
// Constructing a mpp_strtab_index (with mpp_strtab_index) adds one to the refcount on every string in the
// table, and destroying the index subtracts from the refcount. This is needed because the pprof format requires
// that strings be referred to by a zero-based index into a list of strings.
struct mpp_strtab_index {
    // The table this index is a part of and came from.
    struct mpp_strtab *tab;
    // The list & length of interned strings.
    struct mpp_strtab_el **str_list;
    int64_t str_list_len;
    // This st_hash is used to convert already-interned pointers to index into str_list.
    // It's a map of (uintptr_t) -> (int64_t)
    st_table *pos_table;
};


// NOTE - there's better documentation of what these methods do in strtab.c itself.

// Initializes a new, empty string intern table. This will allocate memory that remains owned by the strtab
// module and saves it in tab. It also allocates memory for struct intern_tab itself.
struct mpp_strtab *mpp_strtab_new();

// Destroys a string intern table, including freeing the underlying memory used by tab, and freeing
// the memory pointed to by tab itself.
void mpp_strtab_destroy(struct mpp_strtab *tab);

// Get the size of all memory used by the table
size_t mpp_strtab_memsize(struct mpp_strtab *tab);

// Intern new strings (or increment the refcount of already-interned ones)
void mpp_strtab_intern(
    struct mpp_strtab *tab, const char *str, int str_len,
    const char **interned_str_out, size_t *interned_str_len_out
);
void mpp_strtab_intern_rbstr(
    struct mpp_strtab *tab, VALUE rbstr,
    const char **interned_str_out, size_t *interned_str_len_out
);
void mpp_strtab_intern_cstr(
    struct mpp_strtab *tab, const char *str,
    const char **interned_str_out, size_t *interned_str_len_out
);
void mpp_strtab_intern_strbuilder(
    struct mpp_strtab *tab, struct mpp_strbuilder *builder,
    const char **interned_str_out, size_t *interned_str_len_out
);
// Decrement the refcount of elements in the intern table.
void mpp_strtab_release(struct mpp_strtab *tab, const char *str, size_t str_len);
void mpp_strtab_release_rbstr(struct mpp_strtab *tab, VALUE rbstr);

// Methods for building a zero-based list of interned pointers, for building the final string table
// in the pprof protobuf.
struct mpp_strtab_index *mpp_strtab_index(struct mpp_strtab *tab);
void mpp_strtab_index_destroy(struct mpp_strtab_index *ix);
int64_t mpp_strtab_index_of(struct mpp_strtab_index *ix, const char *interned_ptr);
typedef void (*mpp_strtab_each_fn)(int64_t el_ix, const char *interned_str, size_t interned_str_len, void *ctx);
void mpp_strtab_each(struct mpp_strtab_index *ix, mpp_strtab_each_fn fn, void *ctx);

// ======== BACKTRACE DECLARATIONS ========
struct mpp_backtrace_frame {
    // These strings are interned in the strtab.
    const char *function_name;
    size_t function_name_len;
    const char *file_name;
    size_t file_name_len;
    int line_number;
};
void mpp_setup_backtrace();
// Meaning of the return bits for mpp_capture_backtrace_frame
#define MPP_BT_MORE_FRAMES (1 << 0)
#define MPP_BT_FRAME_VALID (1 << 1)
// The memory semantics of mpp_capture_backtrace_frame are a bit tricky. They are:
//     - When called, frameout must point to an (unitialized) mpp_backtrace_frame
//     - On return, the *_name and *_len fields of that struct will point to interned
//       strings, interned into strtab
//     - It is the _caller's_ responsibility to de-intern these strings when it's done
//       with them
//     - That happens, at the moment, in mpp_sample_refcount_dec
unsigned long mpp_capture_backtrace_frame(
    VALUE thread, unsigned long frame, struct mpp_backtrace_frame *frameout,
    struct mpp_strtab *strtab
);
unsigned long mpp_backtrace_frame_count(VALUE thread);

// ======== SAMPLE DECLARATIONS ========

// The struct mpp_sample is the core type for the data collected by ruby_memprofiler_pprof.

struct mpp_sample {
    // VALUE of the sampled object that was allocated, or Qundef it it's freed.
    VALUE allocated_value_weak;
    size_t allocated_value_objsize;
    size_t frames_count;
    size_t frames_capacity;
    struct mpp_backtrace_frame frames[];
};

// Creates a new sample with the given frames capacity
struct mpp_sample *mpp_sample_new(unsigned long frames_capacity);
// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample);
// free the sample, including decrementing the refcount on any strings in the backtrace frames.
void mpp_sample_free(struct mpp_sample *sample, struct mpp_strtab *strtab);

// ======== PROTO SERIALIZATION ROUTINES ========
struct mpp_pprof_serctx {
    // Defines the allocation routine & memory arena used by this serialisation context. When the ctx
    // is destroyed, we free the entire arena, so no other (protobuf) memory needs to be individually
    // freed.
    upb_alloc allocator;
    upb_Arena *arena;
    // String intern index; recall that holding this object does _not_ require that we have exclusive
    // use of the underlying string intern table, so it's safe for us to use this in a separate thread.
    struct mpp_strtab_index *string_intern_index;
    // Map of function ID -> function protobuf
    st_table *function_pbs;
    // Map of (function ID, line number) -> location protobufs
    st_table *location_pbs;
    // Counter for assigning location IDs
    uint64_t loc_counter;
    // The protobuf representation we are building up.
    perftools_profiles_Profile *profile_proto;

    // We need to keep interned copies of some strings that will wind up in the protobuf output.
    // This is so that we can put constant values like "allocations" and "count" into our pprof output
    // (the pprof format requires these strings to be in the string table along with the rest of them)
    const char *internstr_count;
    const char *internstr_bytes;
    const char *internstr_retained_objects;
    const char *internstr_retained_size;

    // Toggle to interrupt (toggled from Ruby's GVL unblocking function)
    uint8_t interrupt;
};

struct mpp_pprof_serctx *mpp_pprof_serctx_new(
        struct mpp_strtab *strtab, char *errbuf, size_t errbuflen
);
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx);
int mpp_pprof_serctx_add_sample(
    struct mpp_pprof_serctx *ctx, struct mpp_sample *sample, char *errbuf, size_t errbuflen
);
int mpp_pprof_serctx_serialize(
    struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf, size_t errbuflen
);

// ======== COLLECTOR RUBY CLASS ========
void mpp_setup_collector_class();

#endif
