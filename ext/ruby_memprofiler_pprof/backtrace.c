// External ruby headers
#include <ruby.h>
#include <ruby/debug.h>
// Also include _INTERNAL_ ruby headers, from the debase-ruby_core_source gem.
// This will let us reach into the guts of the Ruby interpreter in a very, VERY
// API-unstable way.
#include <vm_core.h>
#include <iseq.h>

#include "ruby_memprofiler_pprof.h"

// We need access to the rb_vm_frame_method_entry method from vm_insnhelper.c; this function
// gives us the method information for C functions in Ruby backtraces.
// We have a prototype in vm_core.h, and the implementation in vm_insnhelper.c is "extern"
// (i.e. it is not "static").
//
// That would _normally_ mean we can simply just call it, BUT - Ruby is compiled by default
// with -fvisibility=hidden. That makes the function callable by other C files in the Ruby
// source tree, but it's NOT callable from code in other .so files (like our C extension);
// the linker marks the symbol as STV_HIDDEN in the .so file when it's linked. So, if we
// simply try to call it, when requiring our extension, the dynamic loader will complain
// that it can't resolve the symbol rb_vm_frame_method_entry.
//
// (In case you were wondering, other Ruby functions that are supposed to be called from
// C extensions are marked with "#pragma GCC visibility push(default)", which marks the
// function with "default visibility" and thus overrides -fvisibility=hidden and allows
// the function to be called).
//
// To work around this, I copied the definition of rb_vm_frame_method_entry (and
// check_method_entry, which it calls) into this file. I checked and the implementation
// is pretty much identical from Ruby 2.6.0 -> 3.1.0 so this _should_ be OK. Note that
// we also mark _our_ copy of this function as __attribute__(( visibility("hidden") ))
// so that we don't export it either.

static rb_callable_method_entry_t *
check_method_entry(VALUE obj, int can_be_svar)
{
    if (obj == Qfalse) return NULL;

#if VM_CHECK_MODE > 0
    if (!RB_TYPE_P(obj, T_IMEMO)) rb_bug("check_method_entry: unknown type: %s", rb_obj_info(obj));
#endif

    switch (imemo_type(obj)) {
        case imemo_ment:
            return (rb_callable_method_entry_t *)obj;
        case imemo_cref:
            return NULL;
        case imemo_svar:
            if (can_be_svar) {
                return check_method_entry(((struct vm_svar *)obj)->cref_or_me, 0);
            }
            // falls through
        default:
#if VM_CHECK_MODE > 0
            rb_bug("check_method_entry: svar should not be there:");
#endif
            return NULL;
    }
}

__attribute__(( visibility("hidden") ))
const rb_callable_method_entry_t *
rb_vm_frame_method_entry(const rb_control_frame_t *cfp)
{
    const VALUE *ep = cfp->ep;
    rb_callable_method_entry_t *me;

    while (!VM_ENV_LOCAL_P(ep)) {
        if ((me = check_method_entry(ep[VM_ENV_DATA_INDEX_ME_CREF], 0)) != NULL) return me;
        ep = VM_ENV_PREV_EP(ep);
    }
    return check_method_entry(ep[VM_ENV_DATA_INDEX_ME_CREF], 1);
}

// Captures a backtrace!
//
// This method uses internal Ruby headers to implement a rough copy of what backtrace_each in vm_backtrace.c
// does. This is _TREMENDOUSLY_ faster than actually calling rb_make_backtrace, because that creates Ruby
// VALUE objects to hold its result. Doing so from a newobj tracepoint is legal, AFAICT, but really slow
// (from my profiling of the profiler, doing this causes the garbage collector to run, _a lot_, in the
// middle of creating the original object).
//
// By poking directly at the VM internal structures, we can stuff the result into C structures and not
// allocate any ruby VALUEs avoiding this issue. It's a _LOT_ faster - it takes the overhead from ~50%
// (with 1% of allocations ampled), to < 1%. So it's definitely worthwhile despite how disgusting it is.
//
// Also note: This captures backtraces most recent call LAST, which is the opposite order to how the pprof
// protobuf wants them, so they have to be reversed later. It's not so convenient to just capture it in
// the correct order because we may have to skip some frames; thus if we filled in the frame array backwards,
// we might not actually wind up filling frames[0].
void mpp_rb_backtrace_init(struct mpp_rb_backtrace *bt, struct mpp_strtab *strtab) {
    const rb_control_frame_t *last_cfp = GET_EC()->cfp;
    const rb_control_frame_t *start_cfp = RUBY_VM_END_CONTROL_FRAME(GET_EC());

    // Allegedly, according to vm_backtrace.c, we need to skip the first two control frames because they
    // are "dummy frames", whatever that means.
    start_cfp = RUBY_VM_NEXT_CONTROL_FRAME(start_cfp);
    start_cfp = RUBY_VM_NEXT_CONTROL_FRAME(start_cfp);

    // Calculate how many frames are in this backtrace.
    ptrdiff_t max_backtrace_size;
    if (start_cfp < last_cfp) {
        max_backtrace_size = 0;
    } else {
        max_backtrace_size = start_cfp - last_cfp + 1;
    }

    bt->frames = mpp_xmalloc(sizeof(struct mpp_rb_backtrace_frame) * max_backtrace_size);
    // Set bt->frames_count to zero, to start with, and only increment it when we see a frame
    // in the backtrace we can actually understand. We might skip over some of them, so max_backtrace_size
    // is a maximum of how many frames there might be in the backtrace.
    bt->frames_count = 0;
    // But do keep track of the memsize
    bt->memsize = sizeof(struct mpp_rb_backtrace_frame) * max_backtrace_size;

    ptrdiff_t i;
    const rb_control_frame_t *cfp;
    for (i = 0, cfp = start_cfp; i < max_backtrace_size; i++, cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp)) {
        struct mpp_rb_backtrace_frame *frame = &bt->frames[bt->frames_count];

        if (cfp->iseq && cfp->pc) {
            // I believe means this backtrace frame is ruby code

            size_t iseq_pos = (size_t)(cfp->pc - cfp->iseq->body->iseq_encoded);
            frame->line_number = rb_iseq_line_no(cfp->iseq, iseq_pos);

            // Extract the names and intern them.
            VALUE function_name_val = rb_iseq_method_name(cfp->iseq);
            VALUE label_val = rb_iseq_label(cfp->iseq);
            VALUE filepath_val = rb_iseq_path(cfp->iseq);
            mpp_strtab_intern_rbstr(strtab, function_name_val, &frame->function_name, &frame->function_name_len);
            mpp_strtab_intern_rbstr(strtab, label_val, &frame->label, &frame->label_len);
            mpp_strtab_intern_rbstr(strtab, filepath_val, &frame->filename, &frame->filename_len);


            // Calculate the IDs
            frame->function_id = FIX2ULONG(rb_obj_id(function_name_val));
            // Use the lower 48 bits (which is the sizeof an address on x86_64) of the function name,
            // and the line number in the top 16 bits, as the "location id".
            // Guess this won't work reliably if your function has more than 16k lines, in which case...
            // ...just get a better function?
            frame->location_id = (frame->line_number << 48)  | (frame->function_id & 0x0000FFFFFFFFFFFF);

            bt->frames_count++;
        } else if (RUBYVM_CFUNC_FRAME_P(cfp)) {
            // I believe means that this backtrace frame is a call to a cfunc
            const rb_callable_method_entry_t *me = rb_vm_frame_method_entry(cfp);

            // We can't have a line number for C code.
            frame->line_number = 0;

            VALUE function_name_val = rb_id2str(me->def->original_id);
            mpp_strtab_intern_rbstr(strtab, function_name_val, &frame->function_name, &frame->function_name_len);
            // For cfuncs, the label should just be the same as the function name, I believe.
            // Note that we are _NOT_ just copying the pointers, to ensure that the intern refcount goes up.
            mpp_strtab_intern_rbstr(strtab, function_name_val, &frame->label, &frame->label_len);
            // We can't get a filepath for Cfuncs. Just use the string "(cfunc)"
            mpp_strtab_intern(strtab, "(cfunc)", MPP_STRTAB_USE_STRLEN, &frame->filename, &frame->filename_len);

            // As for IDs - use the symbol ID of the method name (which is interned forever) as both the function
            // ID and the location ID (since we have no line numbers).
            frame->function_id = me->def->original_id;
            frame->location_id = me->def->original_id;

            bt->frames_count++;
        } else {
            // No idea what this means. It's silently ignored in vm_backtrace.c. Guess we will too.
        }
    }
}

void mpp_rb_backtrace_destroy(struct mpp_rb_backtrace *bt, struct mpp_strtab *strtab) {
    // Decrement the refcount on each interned string.
    for (int64_t i = 0; i < bt->frames_count; i++) {
        struct mpp_rb_backtrace_frame *frame = &bt->frames[i];
        mpp_strtab_release(strtab, frame->filename, frame->filename_len);
        mpp_strtab_release(strtab, frame->function_name, frame->function_name_len);
        mpp_strtab_release(strtab, frame->label, frame->label_len);
    }

    mpp_free(bt->frames);
    bt->frames_count = 0;
    bt->memsize = 0;
}

size_t mpp_rb_backtrace_memsize(struct mpp_rb_backtrace *bt) {
    return bt->memsize;
}
