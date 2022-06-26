#include "ruby_private.h"
#include "ruby_memprofiler_pprof.h"

#include <string.h>
#include <stdarg.h>

struct mpp_string_builder {
    char *buf;
    size_t bufsize;
    char *curr_ptr;
};

void mpp_string_builder_appendf(struct mpp_string_builder *builder, const char *fmt, ...) {
    va_list fmtargs;
    va_start(fmtargs, fmt);
    size_t remaining_bufsize = builder->bufsize - (builder->curr_ptr - builder->buf);
    size_t chars_would_write = vsnprintf(builder->curr_ptr, remaining_bufsize, fmt, fmtargs);
    if (chars_would_write >= remaining_bufsize) {
        // This will cause remaining_bufsize to be zero on subsequent invocations, resulting in nothing
        // further being written.
        builder->curr_ptr = builder->buf + builder->bufsize;
    } else {
        builder->curr_ptr = builder->curr_ptr + chars_would_write;
    }
    va_end(fmtargs);
}

static void capture_ruby_backtrace_frame(VALUE thread, int frame) {
    rb_thread_t *thread_data = (rb_thread_t *) DATA_PTR(thread);
    rb_execution_context_t *ec = thread_data->ec;

    // The frame argument is zero-based with zero being "the frame closest to where execution is now"
    // (I couldn't decide if this was supposed to be the "top" or "bottom" of the callstack; but lower
    // frame argument --> more recently called function.
    const rb_control_frame_t *cfp = ec->cfp + frame;
    MPP_ASSERT_MSG(
        RUBY_VM_VALID_CONTROL_FRAME_P(cfp, RUBY_VM_END_CONTROL_FRAME(ec)),
        "Computed control frame was not valid!"
    );

    const rb_callable_method_entry_t *cme = rb_vm_frame_method_entry(cfp);

    if (cfp->iseq && !cfp->pc) {
        // Apparently means that this frame shouldn't appear in a backtrace
    } else if (VM_FRAME_RUBYFRAME_P(cfp)) {
        // A frame executing Ruby code
    } else if (cme && cme->def->type == VM_METHOD_TYPE_CFUNC) {
        // A frame executing C code
    } else {
        // Also shouldn't appear in backtraces.
    }
}

static VALUE bt_defined_class(rb_callable_method_entry_t *cme) {
    if (!RTEST(cme)) {
        return Qnil;
    }
    return cme->defined_class;
}

struct mpp_bt_frame_data {
    rb_callable_method_entry_t *cme;
};

static inline VALUE cme_defined_class(rb_callable_method_entry_t *cme) {
    return RTEST(cme) ? cme->defined_class : Qnil;
}

static inline bool class_is_refinement(VALUE klass, VALUE klass_of_klass) {
    if (!RTEST(klass)) return false;
    return FL_TEST(klass_of_klass, RMODULE_IS_REFINEMENT);
}

static inline bool class_is_singleton(VALUE klass) {
    return FL_TEST(klass, FL_SINGLETON);
}

static inline VALUE refinement_get_refined_class(VALUE refinement_module) {
    ID id_refined_class;
    CONST_ID(id_refined_class, "__refined_class__");
    return rb_attr_get(refinement_module, id_refined_class);
}

// A version of rb_class_path that doesn't allocate any VALUEs
static inline void class_path(VALUE obj, struct mpp_string_builder *out) {
    
}

// A version of rb_any_to_s that doesn't allocate any VALUEs
static inline void any_to_s(VALUE obj, struct mpp_string_builder *out) {

}


// A version of rb_mod_to_s that doesn't allocate any VALUEs.
static inline void module_to_s(VALUE klass, struct mpp_string_builder *out) {
    if (class_is_singleton(klass)) {
        mpp_string_builder_appendf(out, "<#Class:");
        VALUE attached_to = rb_ivar_get(klass, id__attached__);
        if (CLASS_OR_MODULE_P(attached_to)) {
            module_to_s(attached_to, out);
        } else {

        }
        mpp_string_builder_appendf(out, ">");
    }
}


static void bt_frame_stringify_name(struct mpp_bt_frame_data frame_data, struct mpp_string_builder *name_out) {
    VALUE defined_klass = cme_defined_class(frame_data.cme);
    VALUE klass_of_defined_klass = rb_class_of(defined_klass);

    if (class_is_refinement(defined_klass, klass_of_defined_klass)) {
        VALUE refined_class = refinement_get_refined_class(klass_of_defined_klass);


    }
}