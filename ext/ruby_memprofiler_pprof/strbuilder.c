#include <stdarg.h>
#include <stdio.h>
#include "ruby_memprofiler_pprof.h"

void mpp_strbuilder_appendf(struct mpp_strbuilder *str, const char *fmt, ...) {
    va_list fmtargs;
    va_start(fmtargs, fmt);

    // The size left in the buffer
    size_t max_writesize = str->original_bufsize - (str->original_buf - str->curr_ptr);
    // vsnprintf returns the number of bytes it _would_ have written, not including
    // the null terminator.
    size_t attempted_writesize_wo_nullterm = vsnprintf(
        str->curr_ptr, max_writesize,
        fmt, fmtargs
    );
    str->attempted_size += attempted_writesize_wo_nullterm;
    if (attempted_writesize_wo_nullterm >= max_writesize) {
        // If the string (including nullterm) would have exceeded the bufsize,
        // point str->curr_ptr to one-past-the-end of the buffer.
        // This will make subsequent calls to vsnprintf() receive zero for
        // max_writesize, no further things can be appended to this buffer.
        str->curr_ptr = str->original_buf + str->original_bufsize;
    } else {
        // If there's still room in the buffer, advance the curr_ptr.
        str->curr_ptr = str->curr_ptr + attempted_writesize_wo_nullterm;
    }

    va_end(fmtargs);
}

void mpp_strbuilder_append(struct mpp_strbuilder *str, const char *cat) {
    size_t max_writesize = str->original_bufsize - (str->original_buf - str->curr_ptr);
    size_t attempted_writesize_wo_nullterm = strlcat(str->curr_ptr, cat, max_writesize);
    str->attempted_size += attempted_writesize_wo_nullterm;
    if (attempted_writesize_wo_nullterm >= max_writesize) {
        str->curr_ptr = str->original_buf + str->original_bufsize;
    } else {
        str->curr_ptr = str->curr_ptr + attempted_writesize_wo_nullterm;
    }
}

void mpp_strbuilder_append_value(struct mpp_strbuilder *str, VALUE val) {
    MPP_ASSERT_MSG(RB_TYPE_P(val, T_STRING), "non T_STRING passed into mpp_strbuilder_append_value");

    const char *val_ptr = RSTRING_PTR(val);
    size_t val_len = RSTRING_LEN(val);

    size_t max_writesize = str->original_bufsize - (str->original_buf - str->curr_ptr);
    size_t chars_to_copy = val_len;
    if (chars_to_copy + 1 > max_writesize) {
        chars_to_copy = max_writesize - 1; // leave room for NULL terminator.
    }
    memcpy(str->curr_ptr, val_ptr, chars_to_copy);
    str->curr_ptr[chars_to_copy] = '\0';
    str->attempted_size += val_len;
    if (val_len + 1 > max_writesize) {
        str->curr_ptr = str->original_buf + str->original_bufsize;
    } else {
        str->curr_ptr += val_len;
    }


    RB_GC_GUARD(val);
}

VALUE mpp_strbuilder_to_value(struct mpp_strbuilder *str) {
    return rb_str_new2(str->original_buf);
}

void mpp_strbuilder_init(struct mpp_strbuilder *str, char *buf, size_t bufsize) {
    str->original_buf = buf;
    str->original_bufsize = bufsize;
    str->curr_ptr = str->original_buf;
    str->attempted_size = 0;
    if (str->original_bufsize > 0) {
        str->original_buf[0] = '\0';
    }
}
