// backtracie: Ruby gem for beautiful backtraces
// Copyright (C) 2021 Ivo Anjo <ivo@ivoanjo.me>
//
// This file is part of backtracie.
//
// backtracie is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// backtracie is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with backtracie.  If not, see <http://www.gnu.org/licenses/>.

#ifndef BACKTRACIE_PUBLIC_H
#define BACKTRACIE_PUBLIC_H

#include <ruby.h>
#include <stdbool.h>
#include <stdint.h>

// clang-format off
#if defined(_WIN32) || defined(_WIN64)
# ifdef BACKTRACIE_EXPORTS
#   define BACKTRACIE_API __declspec(dllexport)
# else
#   define BACKTRACIE_API __declspec(dllimport)
# endif
#else
# define BACKTRACIE_API __attribute__((visibility("default")))
#endif
// clang-format on

typedef struct {
  // The first element of this struct is a bitfield, comprised of uint32_t's.
  // Ruby requires that VALUE be at least 32-bit but not necessarily 64-bit;
  // using uint32_t for the bitfield ensures we have a minimum of pointless
  // padding in this struct no matter the system.

  // 1 -> ruby frame / 0 -> cfunc frame
  uint32_t is_ruby_frame : 1;
  // 1 means self really is the self object, 0 means it's the class of the self.
  uint32_t self_is_real_self : 1;
  // The iseq & cme; one, both, or neither might actually be available; if
  // they're not, they will be Qnil. These need to be GC marked if you wish to
  // keep the location alive.
  VALUE iseq;
  VALUE callable_method_entry;
  // This is either self, or rb_class_of(self), depending on a few thing.
  // It will be the actual self object for the frame if
  //   - self is the toplevel binding,
  //   - self is rb_mRubyVMFrozenCore,
  //   - self is a class or module
  // Otherwise, it will be rb_class_of(self).
  // This is done so that, should the caller decide to retain the raw_location
  // instance for a while, GC mark its VALUEs, and stringify the backtrace
  // later, we both:
  //   - retain enough information to actually produce a good name for the
  //     method,
  //   - but also don't hold onto random objects that would otherwise be GC'd
  //     just because they happened to be in a backtrace.
  // This needs to be marked if you wish to keep the location alive
  VALUE self_or_self_class;
  // Raw PC pointer; not much use to callers, but saved so we can calculate the
  // lineno later on.
  const void *pc;
} raw_location;

// Returns the number of Ruby frames currently live on the thread; this
// can be used to judge how many times backtracie_capture_frame_for_thread()
// should be called to capture the actual frames.
BACKTRACIE_API int backtracie_frame_count_for_thread(VALUE thread);
// Fills in the raw_location struct with the details of the Ruby call stack
// frame on the given thread at the given index. The given index may or may not
// refer to a "valid" frame on the ruby stack; there are some entries on the
// Ruby stack which are not actually meaningful from a call-stack perspective.
//
// The maximum allowable value for i is
// (backtracie_frame_count_for_thread(thread) - 1). Any value higher than this
// will crash.

// If the given index DOES refer to a valid frame on the Ruby stack:
//   - *loc will be filled in with details of the frame,
//   - the return value will be true
// If the given index DOES NOT refer to a valid frame, then:
//   - *loc will be totally untouched,
//   - the return value will be false
//
// The intended usage of this API looks something like this:
//
//   VALUE thread = rb_thread_current();
//   int max_frame_count = backtracie_frame_count_for_thread(thread);
//   raw_location *locs = xcalloc(frame_count ,sizeof(raw_location));
//   int frame_counter = 0;
//   for (int i = 0; i < max_frame_count; i++) {
//     bool is_valid = backtracie_capture_frame_for_thread(thread, i,
//                                                         &locs[frame_counter]);
//     if (is_valid) frame_counter++;
//   }
BACKTRACIE_API
bool backtracie_capture_frame_for_thread(VALUE thread, int frame_index,
                                         raw_location *loc);
// Get the "qualified method name" for the frame. This is a string that best
// describes what method is being called, intended for human interpretation.
// Writes a NULL-term'd string of at most buflen chars (including NULL
// terminator) to *buf. It returns the number of characters (NOT including NULL
// terminator) that would be required to store the full string (which might be >
// buflen, if it has been truncated). Essentially, has the same semantics as
// snprintf or strlcat.
BACKTRACIE_API
size_t backtracie_frame_name_cstr(const raw_location *loc, char *buf,
                                  size_t buflen);
// Like backtracie_frame_name_cstr, but will allocate memory to ensure that
// there is no truncation of the method name; returns a Ruby string.
BACKTRACIE_API
VALUE backtracie_frame_name_rbstr(const raw_location *loc);
// Returns the filename of the source file for this backtrace location. Has the
// same string handling semantics as backtracie_frame_name_cstr.
//
// Pass true/false for absolute to get the full, realpath vs just the path.
//
// If loc is a cfunc, and therefore has no filename, 0 is returned (and no
// string is written to *buf)
BACKTRACIE_API
size_t backtracie_frame_filename_cstr(const raw_location *loc, bool absolute,
                                      char *buf, size_t buflen);
// Like backtracie_frame_filename_cstr, but returns a Ruby string. Will allocate
// memory to ensure there is no truncation. Returns Qnil if there is no
// filename.
BACKTRACIE_API
VALUE backtracie_frame_filename_rbstr(const raw_location *loc, bool absolute);

// Returns the source line number of the given location, if it's a Ruby frame
// (otherwise, returns 0).
BACKTRACIE_API
int backtracie_frame_line_number(const raw_location *loc);
// Returns a string which would be like the "label" returned by
// rb_profile_frames or Thread#backtrace or similar.
BACKTRACIE_API
size_t backtracie_frame_label_cstr(const raw_location *loc, bool base,
                                   char *buf, size_t buflen);
// Like backtracie_frame_label_cstr, but returns a ruby string.
BACKTRACIE_API
VALUE backtracie_frame_label_rbstr(const raw_location *loc, bool base);
// Returns a VALUE that can be passed into the rb_profile_frames family of
// methods
BACKTRACIE_API
VALUE backtracie_frame_for_rb_profile(const raw_location *loc);
// Marks the contained ruby objects; for use when you want to persist the
// raw_location object beyond the current call stack.
BACKTRACIE_API
void backtracie_frame_mark(const raw_location *loc);
// Like backtracie_frame_mark, but calls rb_gc_mark_movable if available.
BACKTRACIE_API
void backtracie_frame_mark_movable(const raw_location *loc);
// Updates *loc's VALUEs to point to possibly-moved locations
BACKTRACIE_API
void backtracie_frame_compact(raw_location *loc);

// This is an optional facility to create a VALUE containing an array of
// backtracie frames, which itself owns the process of marking them. When using
// this, you need only mark the wrapper, and the wrapper will take care of
// marking the individual frames. NOTE - if you keep a reference to this value
// on the stack, you _probably_ need to use RB_GC_GUARD() on it, because the
// Ruby GC cannot trace usage of the underlying pointer.
BACKTRACIE_API
VALUE backtracie_frame_wrapper_new(size_t capa);
// Returns the underying array of frames for use
BACKTRACIE_API
raw_location *backtracie_frame_wrapper_frames(VALUE wrapper);
// This returns a pointer to the len, so you can update the size of the
// contained list of frames (up to capa)
BACKTRACIE_API
int *backtracie_frame_wrapper_len(VALUE wrapper);

typedef struct {
  // 1 -> ruby frame / 0 -> cfunc frame
  uint16_t is_ruby_frame : 1;
  // What is in the method_quaalifier field?
  // 0 -> rb_class_of(the VALUE the method was called on)
  // 1 -> the VALUE the method was called on
  // 2 -> the class that the callable_method_entry is defined on.
  uint16_t method_qualifier_contents : 2;
  // 1 means the "method name" union contains a CME method ID, 0 means it
  // contains a frame base_label.
  uint16_t has_cme_method_id : 1;
  // 1 means iseq_type is populated
  uint16_t has_iseq_type : 1;

  // Comes from iseq->body->type; is an iseq_type enum.
  uint16_t iseq_type;

  // Line number - we calculate this at capture time because it's fast and thus
  // we don't need to store the iseq.
  uint32_t line_number;

  union {
    // Comes from callable_method_entry->def->original_id
    ID cme_method_id;
    // Comes from iseq->body->location.base_label
    VALUE base_label;
  } method_name;

  // VALUE for the filename (abs), or Qnil
  VALUE filename;

  // See method_qualifier_contents flag.
  VALUE method_qualifier;
} minimal_location_t;

BACKTRACIE_API
bool backtracie_capture_minimal_frame_for_thread(VALUE thread, int frame_index,
                                                 minimal_location_t *loc);

BACKTRACIE_API
size_t backtracie_minimal_frame_name_cstr(const minimal_location_t *loc,
                                          char *buf, size_t buflen);
BACKTRACIE_API
size_t backtracie_minimal_filename_cstr(const minimal_location_t *loc,
                                        char *buf, size_t buflen);
#endif
