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

// Allocates, initializes and returns a new location table. A location table keeps track of a mapping
// of (compact!) location IDs to functions/line numbers. This keeps things in a convenient form for
// later turning to pprof, but also makes sure that we don't store the same char *'s for function/file
// names over and over again.
// Requires a reference to a string interning table, which must be valid for the lifetime of this
// loctab object.
struct mpp_rb_loctab *mpp_rb_loctab_new(struct mpp_strtab *strtab) {
    struct mpp_rb_loctab *loctab = mpp_xmalloc(sizeof(struct mpp_rb_loctab));
    loctab->strtab = strtab;
    loctab->functions = st_init_numtable();
    loctab->locations = st_init_numtable();
    loctab->function_count = 0;
    loctab->location_count = 0;
    return loctab;
}

// Destroys the loctab, including its own memory.
void mpp_rb_loctab_destroy(struct mpp_rb_loctab *loctab) {
    st_free_table(loctab->functions);
    st_free_table(loctab->locations);
    mpp_free(loctab);
}

struct fnloc_st_update_args {
    struct mpp_rb_loctab *loctab;
    uint64_t location_id;
    uint64_t function_id;
    struct mpp_rb_loctab_location *location;
    struct mpp_rb_loctab_function *function;
    VALUE fn_name_value;
    VALUE file_name_value;
    uint64_t location_line_number;
    int64_t function_line_number;
};


static int function_st_update(st_data_t *key, st_data_t *value, st_data_t ctx, int exists) {
    struct fnloc_st_update_args *args = (struct fnloc_st_update_args *)ctx;

    if (exists) {
        // Function already exists; put it in our outargs
        args->function = (struct mpp_rb_loctab_function *)*value;
    } else {
        // Function doesn't exist; build it.
        args->function = mpp_xmalloc(sizeof(struct mpp_rb_loctab_function));
        *value = (st_data_t)args->function;
        args->function->refcount = 0; // Will be incrmeented in backtrace_capture
        args->function->id = args->function_id;
        args->loctab->function_count++;

        mpp_strtab_intern_rbstr(
            args->loctab->strtab, args->fn_name_value,
            &args->function->function_name, &args->function->function_name_len
        );

        if (RTEST(args->file_name_value)) {
            mpp_strtab_intern_rbstr(
                args->loctab->strtab, args->file_name_value,
                &args->function->file_name, &args->function->file_name_len
            );
        } else {
            mpp_strtab_intern_cstr(
                args->loctab->strtab, "(unknown filename)",
                &args->function->file_name, &args->function->file_name_len
            );
        }
        args->function->line_number = args->function_line_number;
    }

    return ST_CONTINUE;
}

static int location_st_update(st_data_t *key, st_data_t *value, st_data_t ctx, int exists) {
    struct fnloc_st_update_args *args = (struct fnloc_st_update_args *)ctx;

    if (exists) {
        // Location already exists - put it in our outargs.
        args->location = (struct mpp_rb_loctab_location *)*value;
        args->function = args->location->function;
    } else {
        // Location does not already exist. Make a new one.
        args->location = mpp_xmalloc(sizeof(struct mpp_rb_loctab_location));
        *value = (st_data_t)args->location;
        args->location->refcount = 0; // will be incremented in backtrace_capture
        args->location->id = args->location_id;
        args->loctab->location_count++;

        // Need to find a _function_ for it too.
        st_update(args->loctab->functions, args->function_id, function_st_update, (st_data_t)args);
        args->location->function = args->function;
        args->location->line_number = args->location_line_number;
    }

    return ST_CONTINUE;
}

static int function_st_deref(st_data_t *key, st_data_t *value, st_data_t ctx, int exists) {
    struct mpp_rb_loctab *loctab = (struct mpp_rb_loctab *)ctx;

    MPP_ASSERT_MSG(exists, "attempted to decrement refcount on non-existing function");
    struct mpp_rb_loctab_function *fn = (struct mpp_rb_loctab_function *)*value;
    MPP_ASSERT_MSG(fn->refcount > 0, "attempted to decrement zero refcount on function");
    fn->refcount--;
    if (fn->refcount == 0) {
        // Unref its string table entries.
        mpp_strtab_release(loctab->strtab, fn->function_name, fn->function_name_len);
        mpp_strtab_release(loctab->strtab, fn->file_name, fn->file_name_len);
        mpp_free(fn);
        loctab->function_count--;
        return ST_DELETE;
    }
    return ST_CONTINUE;
}

static int location_st_deref(st_data_t *key, st_data_t *value, st_data_t ctx, int exists) {
    struct mpp_rb_loctab *loctab = (struct mpp_rb_loctab *)ctx;

    MPP_ASSERT_MSG(exists, "attempted to decrement refcount on non-existing location");
    struct mpp_rb_loctab_location *loc = (struct mpp_rb_loctab_location *)*value;
    MPP_ASSERT_MSG(loc->refcount > 0, "attempted to decrement zero refcount on location");
    loc->refcount--;
    if (loc->refcount == 0) {
        // Deref its function too.
        st_update(loctab->functions, loc->function->id, function_st_deref, (st_data_t)loctab);
        mpp_free(loc);
        loctab->location_count--;
        return ST_DELETE;
    }
    return ST_CONTINUE;
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
//
// This method allocates a struct mpp_rb_backtrace and saves it to *bt_out. The reason for this awkward
// calling convention (as opposed to just returning it) is so that the caller can detect if we got longjmp'd
// out of here by some of the Ruby calls below, and appropriately destroy *bt_out via a call to destroy.
void mpp_rb_backtrace_capture(struct mpp_rb_loctab *loctab, struct mpp_rb_backtrace **bt_out) {
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

    *bt_out = mpp_xmalloc(sizeof(struct mpp_rb_backtrace));
    struct mpp_rb_backtrace *bt = *bt_out;
    bt->frame_locations = mpp_xmalloc(sizeof(uint64_t) * max_backtrace_size);
    // Set bt->frames_count to zero, to start with, and only increment it when we see a frame
    // in the backtrace we can actually understand. We might skip over some of them, so max_backtrace_size
    // is a maximum of how many frames there might be in the backtrace.
    bt->frames_count = 0;
    // But do keep track of the memsize
    bt->memsize = sizeof(uint64_t) * max_backtrace_size;

    ptrdiff_t i;
    const rb_control_frame_t *cfp;
    for (i = 0, cfp = start_cfp; i < max_backtrace_size; i++, cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp)) {

        // Collect all the information we might need about this frame and store it in this struct
        struct fnloc_st_update_args frame_args;
        frame_args.loctab = loctab;
        if (cfp->iseq && cfp->pc) {
            // I believe means this backtrace frame is ruby code
            size_t iseq_pos = (size_t)(cfp->pc - cfp->iseq->body->iseq_encoded);
            // To quote Ruby:
            // "use pos-1 because PC points next instruction at the beginning of instruction"
            if (iseq_pos) iseq_pos--;
            frame_args.location_line_number = rb_iseq_line_no(cfp->iseq, iseq_pos);
            frame_args.fn_name_value = rb_iseq_method_name(cfp->iseq);
            frame_args.file_name_value = rb_iseq_path(cfp->iseq);
            frame_args.function_line_number = NUM2ULONG(rb_iseq_first_lineno(cfp->iseq));

            // Use the object ID of the function name (which _should_ be interned, right, and so unique?)
            // as the function ID.
            frame_args.function_id = NUM2ULONG(rb_obj_id(frame_args.fn_name_value));
            // Use the lower 48 bits (which is the sizeof an address on x86_64) of the function name,
            // and the line number in the top 16 bits, as the "location id".
            // Guess this won't work reliably if your function has more than 16k lines, in which case...
            // ...just get a better function?
            frame_args.location_id =
                (frame_args.location_line_number << 48)  | (frame_args.function_id & 0x0000FFFFFFFFFFFF);

        } else if (RUBYVM_CFUNC_FRAME_P(cfp)) {
            // I believe means that this backtrace frame is a call to a cfunc


            const rb_callable_method_entry_t *me = rb_vm_frame_method_entry(cfp);

            frame_args.location_line_number = 0;
            frame_args.function_line_number = 0;
            frame_args.fn_name_value = rb_id2str(me->def->original_id);
            frame_args.file_name_value = Qnil;

            // Use the symbol ID of the method name (which is interned forever) as both the function
            // ID and the location ID (since we have no line numbers).
            frame_args.location_id = me->def->original_id;
            frame_args.function_id = me->def->original_id;
        } else {
            // No idea what this means. It's silently ignored in vm_backtrace.c. Guess we will too.
            continue;
        }

        // Store the location frame.
        bt->frame_locations[bt->frames_count] = frame_args.location_id;
        bt->frames_count++;

        // Lookup, or allocate & store, the location/function struct.
        st_update(loctab->locations, frame_args.location_id, location_st_update, (st_data_t)&frame_args);
        // Either we created a new one, or looked up an existing one, but either way we need to bump its refcount.
        frame_args.location->refcount++;
        frame_args.function->refcount++;
    }
}

void mpp_rb_backtrace_capture_slowrb(struct mpp_rb_loctab *loctab, struct mpp_rb_backtrace **bt_out) {
    VALUE ruby_bt = rb_funcall(rb_thread_current(), rb_intern("backtrace_locations"), 0);
    int64_t ruby_bt_len = RARRAY_LEN(ruby_bt);

    *bt_out = mpp_xmalloc(sizeof(struct mpp_rb_backtrace));
    struct mpp_rb_backtrace *bt = *bt_out;
    bt->frame_locations = mpp_xmalloc(sizeof(uint64_t) * ruby_bt_len);
    bt->frames_count = 0;
    bt->memsize = sizeof(uint64_t) * ruby_bt_len;

    for (int64_t i = 0; i < ruby_bt_len; i++) {
        // The backtrace_locations result is backwards compared to the fast version.
        VALUE ruby_bt_loc = RARRAY_AREF(ruby_bt, ruby_bt_len - i - 1);

        // Intern the function name as the function ID, and the full string as the loc id.
        VALUE fn_name = rb_funcall(ruby_bt_loc, rb_intern("base_label"), 0);
        const char *fn_name_interned;
        size_t fn_name_interned_len;
        mpp_strtab_intern_rbstr(loctab->strtab, fn_name, &fn_name_interned, &fn_name_interned_len);
        VALUE loc_name = rb_funcall(ruby_bt_loc, rb_intern("to_s"), 0);
        const char *loc_name_interned;
        size_t loc_name_interned_len;
        mpp_strtab_intern_rbstr(loctab->strtab, loc_name, &loc_name_interned, &loc_name_interned_len);

        VALUE file_name = rb_funcall(ruby_bt_loc, rb_intern("path"), 0);

        VALUE line_no = rb_funcall(ruby_bt_loc, rb_intern("lineno"), 0);
        uint64_t line_no_int = 0;
        if (RTEST(line_no)) {
            line_no_int = NUM2ULONG(line_no);
        }

        bt->frame_locations[i] = (uint64_t)loc_name_interned;
        bt->frames_count++;

        // Lookup, or allocate & store, the location/function struct.
        struct fnloc_st_update_args frame_args;
        frame_args.loctab = loctab;
        frame_args.location_id = (uint64_t)loc_name_interned;
        frame_args.location_line_number = line_no_int;
        frame_args.fn_name_value = fn_name;
        frame_args.file_name_value = file_name;
        frame_args.function_line_number = 0;
        frame_args.function_id = (uint64_t)fn_name_interned;
        st_update(loctab->locations, frame_args.location_id, location_st_update, (st_data_t)&frame_args);
        frame_args.location->refcount++;
        frame_args.function->refcount++;

        // We _DEFINITELY_ leak memory here. We interned the function name string/location string to use
        // as the unique int64 location ID, but we're not freeing it here. This is because we need to keep
        // it in the table so that it continues to be unique (and some other location doesn't wind up with
        // the same ID).
        // Since this method basically exists for benchmarking, and real users should be using the CFP
        // method, I'll live with the leak for now until I figure out a better unique ID for the function.
    }
}

void mpp_rb_backtrace_destroy(struct mpp_rb_loctab *loctab, struct mpp_rb_backtrace *bt) {
    for (int64_t i = 0; i < bt->frames_count; i++) {
        st_update(loctab->locations, bt->frame_locations[i], location_st_deref, (st_data_t)loctab);
    }
    mpp_free(bt->frame_locations);
    mpp_free(bt);
}

size_t mpp_rb_backtrace_memsize(struct mpp_rb_backtrace *bt) {
    return bt->memsize;
}

size_t mpp_rb_loctab_memsize(struct mpp_rb_loctab *loctab) {
    return sizeof(*loctab) +
        sizeof(struct mpp_rb_loctab_location) * loctab->location_count +
        sizeof(struct mpp_rb_loctab_function) * loctab->function_count +
        st_memsize(loctab->locations) +
        st_memsize(loctab->functions);
}

struct mpp_rb_loctab_each_location_ctx {
    mpp_rb_loctab_each_location_cb cb;
    void *cb_ctx;
    struct mpp_rb_loctab *loctab;
};

static int mpp_rb_loctab_each_location_thunk(st_data_t key, st_data_t value, st_data_t ctx) {
    struct mpp_rb_loctab_each_location_ctx * thunkctx = (struct mpp_rb_loctab_each_location_ctx *)ctx;
    struct mpp_rb_loctab_location *loc = (struct mpp_rb_loctab_location *)value;
    return thunkctx->cb(thunkctx->loctab, loc, thunkctx->cb_ctx);
}

void mpp_rb_loctab_each_location(struct mpp_rb_loctab *loctab, mpp_rb_loctab_each_location_cb cb, void *ctx) {
    struct mpp_rb_loctab_each_location_ctx thunkctx;
    thunkctx.loctab = loctab;
    thunkctx.cb = cb;
    thunkctx.cb_ctx = ctx;
    st_foreach(loctab->locations, mpp_rb_loctab_each_location_thunk, (st_data_t)&thunkctx);
}

struct mpp_rb_loctab_each_function_ctx {
    mpp_rb_loctab_each_function_cb cb;
    void *cb_ctx;
    struct mpp_rb_loctab *loctab;
};

static int mpp_rb_loctab_each_function_thunk(st_data_t key, st_data_t value, st_data_t ctx) {
    struct mpp_rb_loctab_each_function_ctx * thunkctx = (struct mpp_rb_loctab_each_function_ctx *)ctx;
    struct mpp_rb_loctab_function *loc = (struct mpp_rb_loctab_function *)value;
    return thunkctx->cb(thunkctx->loctab, loc, thunkctx->cb_ctx);
}

void mpp_rb_loctab_each_function(struct mpp_rb_loctab *loctab, mpp_rb_loctab_each_function_cb cb, void *ctx) {
    struct mpp_rb_loctab_each_function_ctx thunkctx;
    thunkctx.loctab = loctab;
    thunkctx.cb = cb;
    thunkctx.cb_ctx = ctx;
    st_foreach(loctab->functions, mpp_rb_loctab_each_function_thunk, (st_data_t)&thunkctx);
}
