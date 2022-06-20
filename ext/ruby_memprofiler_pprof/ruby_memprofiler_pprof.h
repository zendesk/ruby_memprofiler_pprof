#ifndef __RUBY_MEMPROFILER_PPROF_H
#define __RUBY_MEMPROFILER_PPROF_H

#include <pthread.h>
#include <stdint.h>
#include <ruby.h>
#include <backtracie.h>

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

// Look anything up that needs to be done at runtime.
void mpp_compat_init();

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

VALUE mpp_rb_gc_disable_no_rest();

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

// ======== MARKMEMOIZER DECLARATIONS ========

// When holding references to lots of backtracie objects, which in turn contain references to the Ruby iseq
// & self, actually going around to mark them all can take a significant amount of time during GC. We can
// attempt to reduce this by instead keeping a refcounted list of these objects, and marking them once each
// rather than once for every place that it appears.

struct mpp_mark_memoizer {
    // Mapping of VALUE -> refcount of this VALUE.
    st_table *table;
};

struct mpp_mark_memoizer *mpp_mark_memoizer_new();
void mpp_mark_memoizer_destroy(struct mpp_mark_memoizer *memo);
// Inserts VALUE into the memoizer, or else bumps its refcount. Returns the new refcount.
unsigned int mpp_mark_memoizer_add(struct mpp_mark_memoizer *memo, VALUE value);
// Decrements refcount of VALUE, possibly removing from the map. Returns the new refcount.
unsigned int mpp_mark_memoizer_delete(struct mpp_mark_memoizer *memo, VALUE value);
// Marks all the objects in the memoizer (once)
void mpp_mark_memoizer_mark(struct mpp_mark_memoizer *memo);
#ifdef HAVE_RB_GC_MARK_MOVABLE
// Handles GC compaction
void mpp_mark_memoizer_compact(struct mpp_mark_memoizer *memo);
#endif
// Returns the memsize of this memoizer.
size_t mpp_mark_memoizer_memsize(struct mpp_mark_memoizer *memo);

// ======== FUNCTAB DECLARATIONS ========

// The pprof format requires that functions in samples be referred to by an integer "function_id". The format
// imposes no opinions on what the ID should be (i.e. it need not be a zero-based list, like the string intern
// table), but clearly the intention is that function_id would represent the memory address of a function in
// a compiled language.
//
// This is much tricker in Ruby, because the VALUE objects representing ruby methods or iseqs can come, go,
// and even move! Plus, the names of methods need to be computed in-process, and can't be symbolised later on
// in the way a C memory address could.
//
// We solve for this by keeping a table of functions (functab) which have been in the stack that allocated objects
// which are still live. The idea is that when a sample is "processed" (see below), we compute the name of each
// function in the stack (by calling backtracie methods), and store the name (and some other properties, like filename)
// in this table.
//
// The table is keyed by the object ID of the cme/iseq VALUE which represents the function; a ruby object ID is
// only valid whilst the VALUE it was for is still alive, so the functab also keeps a Ruby GC reference to the VALUE.
//
// The function entires in this table are refcounted. When a new sample is processed, the refcount for each function
// in that sample's backtrace goes up; when the sample is freed, the sample methods organise to decrement the
// refcount.

struct mpp_functab {
    // The actual table of (unsigned long #object_id) -> (struct mpp_functab_entry *)
    st_table *function_map;
    // Number of functions in the table
    size_t function_count;
    // String interning table; all of the function names/file names generated by this table are
    // interned into the strtab.
    struct mpp_strtab *strtab;
};

struct mpp_functab_entry {
    // Function ID of self (the CME/iseq's #object_id)
    unsigned long id;
    // Refcount
    unsigned long refcount;
    // The retained VALUE for this function
    VALUE cme_or_iseq;
    // Interned pointer to function name
    const char *function_name;
    size_t function_name_len;
    // Interned pointer to file name
    const char *file_name;
    size_t file_name_len;
};

// Creates a new functab
struct mpp_functab *mpp_functab_new(struct mpp_strtab *strtab);
// Destroy the functab
void mpp_functab_destroy(struct mpp_functab *functab);
// Mark any owned Ruby VALUEs
void mpp_functab_gc_mark(struct mpp_functab *functab);
#ifdef HAVE_RB_GC_MARK_MOVABLE
// Move any owned Ruby VALUEs
void mpp_functab_gc_compact(struct mpp_functab *functab);
#endif
// Total size of the table, for accounting purposes. Will not include the size of
// any interned strings.
size_t mpp_functab_memsize(struct mpp_functab *functab);
// Adds the provided function to the function table (or increases its reference count if it's already there).
// This method will handle interning them into the stringtab.
// Returns the function ID.
unsigned long mpp_functab_add_by_value(struct mpp_functab *functab, VALUE cme_or_iseq, VALUE function_name, VALUE file_name);
// Removes a reference from the specified function
void mpp_functab_deref(struct mpp_functab *functab, unsigned long id);
// Look up a functab entry by ID.
struct mpp_functab_entry *mpp_functab_lookup(struct mpp_functab *functab, unsigned long id);

// ======== SAMPLE DECLARATIONS ========

// The struct mpp_sample is the core type for the data collected by ruby_memprofiler_pprof.
// A sample has a lifecycle governed by the flags parameter.
//
// A sample starts off life with the flag bit MPP_SAMPLE_FLAGS_BT_PROCESSED unset. This means
// that the raw_backtrace field contains the backtracie_bt_t which came out of backtracie, and
// processed_backtrace is undefined. This means that we contain the raw iseq/cme/self VALUEs that
// refer to the actual piece of code inside the Ruby VM for each frame.
//
// A sample needs to be "processed", which extracts string descriptions of the functions/files from
// the backtracie backtrace, and frees the underlying backtracie_bt_t (and thus allows those internal
// VALUEs to be GC'd at the appropriate time too. When a sample is processed, the flag
// MPP_SAMPLE_FLAGS_BT_PROCESSED gets set; this means that raw_backtrace is now undefined, and instead
// processed_backtrace points to a struct mpp_sample_bt_processed.
// After being "processed", a sample no longer retains references to the raw iseq/cme/self VALUEs that
// are contained within backtracie_bt_t. The names of those methods have been calculated and embedded
// in the function interning table (functab), and instead the sample simply holds a list of function
// IDs and line numbers (struct mpp_sample_bt_processed).
//
// If our freeobj tracepoint detects the VALUE tracked by a sample is freed, it sets the flag
// MPP_SAMPLE_FLAGS_VALUE_FREED and sets allocated_value_weak to Qundef.

#define MPP_SAMPLE_FLAGS_BT_PROCESSED (1 << 0)
#define MPP_SAMPLE_FLAGS_VALUE_FREED (1 << 1)

struct mpp_sample {
    // flags; see above for meaning.
    uint8_t flags;
    // The refcount will only ever be zero, one, or two; cram it into a small field because this struct
    // gets allocated a lot.
    uint8_t refcount;
    // VALUE of the sampled object that was allocated, or Qundef it it's freed.
    VALUE allocated_value_weak;
    union {
        // Valid if !(flags & MPP_SAMPLE_FLAGS_BT_PROCESSED)
        backtracie_bt_t raw_backtrace;
        // Valid if (flags & MPP_SAMPLE_FLAGS_BT_PROCESSED)
        struct mpp_sample_bt_processed  {
            size_t num_frames;
            struct mpp_sample_bt_processed_frame {
                // Refers to an entry in fucntab
                unsigned long function_id;
                // Line number on the specified function, if available; else zero.
                unsigned long line_number;
            } frames[];
        } *processed_backtrace;
    };
};

// Creates a new sample with zero flags and 1 refcount
// It requires a reference to a mark memoizer structure, and the raw
// backtrace which the sample starts with. The VALUEs from the raw backtrace
// get put in the mark memoizer cache.
struct mpp_sample *mpp_sample_new(VALUE allocated_value, backtracie_bt_t raw_backtrace, struct mpp_mark_memoizer *mark_memo);
// Mark any contained Ruby VALUEs
void mpp_sample_gc_mark(struct mpp_sample *sample);
#ifdef HAVE_RB_GC_MARK_MOVABLE
// Move any contained Ruby VALUEs
void mpp_sample_gc_compact(struct mpp_sample *sample);
#endif
// Total size of all things owned by the sample, for accounting purposes
size_t mpp_sample_memsize(struct mpp_sample *sample);
// Increments the refcount on sample
uint8_t mpp_sample_refcount_inc(struct mpp_sample *sample);
// Decrements the refcount on sample, freeing its resources if it drops to zero.
uint8_t mpp_sample_refcount_dec(struct mpp_sample *sample, struct mpp_functab *functab, struct mpp_mark_memoizer *mark_memo);
// "Processes" the sample. This involves walking the backtracie frames, stringifying all method names,
// and interning the strings & function definitions into the provided functab.
void mpp_sample_process(struct mpp_sample *sample, struct mpp_functab *functab, struct mpp_mark_memoizer *mark_memo);
// Mark the sample as freed (as in, the underlying value is freed)
void mpp_sample_mark_value_freed(struct mpp_sample *sample);

// ======== PROTO SERIALIZATION ROUTINES ========
struct mpp_pprof_serctx {
    // Defines the allocation routine & memory arena used by this serialisation context. When the ctx
    // is destroyed, we free the entire arena, so no other (protobuf) memory needs to be individually
    // freed.
    upb_alloc allocator;
    upb_Arena *arena;
    // Location table used for looking up fucntion IDs to strings.
    struct mpp_functab *function_table;
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
};

struct mpp_pprof_serctx *mpp_pprof_serctx_new(
        struct mpp_strtab *strtab, struct mpp_functab *functab, char *errbuf, size_t errbuflen
);
void mpp_pprof_serctx_destroy(struct mpp_pprof_serctx *ctx);
int mpp_pprof_serctx_add_sample(
    struct mpp_pprof_serctx *ctx, struct mpp_sample *sample, size_t object_size, char *errbuf, size_t errbuflen
);
int mpp_pprof_serctx_serialize(
    struct mpp_pprof_serctx *ctx, char **buf_out, size_t *buflen_out, char *errbuf, size_t errbuflen
);

// ======== COLLECTOR RUBY CLASS ========
void mpp_setup_collector_class();

#endif
