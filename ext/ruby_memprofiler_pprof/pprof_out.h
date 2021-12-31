#ifndef __RMMP_PPROF_OUT_H
#define __RMMP_PPROF_OUT_H

#include <ruby.h>

// This header is used to export the C++ bits (that use the C++ protobuf stuff) to the C bits
// (the ruby extension); so, it needs to be both valid C++ and C.

#ifdef __cplusplus
extern "C" {
#endif

struct internal_sample_bt_frame {
    uint64_t filename;              // Filename index into string intern table
    uint64_t lineno;                // Line number
    uint64_t function_name;         // Method name index into string intern table
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

struct string_tab_el {
    uint64_t index;     // Index which will go in the pprof protobuf & the sample frames
    char *str;          // Pointer to the null-terminated string itself
    size_t str_len;     // Length of str, not including null termination.
};


// Forward-declare the struct, so that it's opaque to C land (in the C++ file, it will be defined as holdling
// a reference to some protobuf classes).
struct pprof_serialize_state;

struct pprof_serialize_state *rmmp_pprof_serialize_init();
void rmmp_pprof_serialize_add_strtab(struct pprof_serialize_state *state, st_table *strtab);
void rmmp_pprof_serialize_add_alloc_samples(struct pprof_serialize_state *state, struct internal_sample *sample_list);
void rmmp_pprof_serialize_to_memory(struct pprof_serialize_state *state, char **outbuf, size_t *outlen, int *abort_flag);
void rmmp_pprof_serialize_destroy(struct pprof_serialize_state *state);

#ifdef __cplusplus
}
#endif

#endif