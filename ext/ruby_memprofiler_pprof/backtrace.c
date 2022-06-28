#include "ruby_private.h"
#include "ruby_memprofiler_pprof.h"

#include <string.h>
#include <stdarg.h>

static VALUE main_object;

struct internal_frame_data {
    bool cfunc;
    const rb_callable_method_entry_t *cme;
    const rb_iseq_t *iseq;
    VALUE self;
};

static void qualified_method_name_for_frame(
    struct internal_frame_data data, struct mpp_strbuilder *strou
);
static void mod_to_s(VALUE klass, struct mpp_strbuilder *strout);
static void mod_to_s_singleton(VALUE klass, struct mpp_strbuilder *strout);
static void mod_to_s_anon(VALUE klass, struct mpp_strbuilder *strout);
static void mod_to_s_refinement(VALUE refinement_module, struct mpp_strbuilder *strout);
static void method_qualifier(struct internal_frame_data data, struct mpp_strbuilder *strout);
static void method_name(struct internal_frame_data data, struct mpp_strbuilder *strout);
static bool iseq_is_block_or_eval(const rb_iseq_t *iseq);
static void iseq_path(const rb_iseq_t *iseq, struct mpp_strbuilder *strout);
static int iseq_calc_lineno(const rb_iseq_t *iseq, const VALUE *pc);

struct frame_cache_key {
    // Either the defined_class of a CME, or the class_of(self)
    // for an iseq.
    VALUE method_defined_on;
    // Either a CME, or an iseq
    VALUE method_def;
};

struct frame_cache_value {
    const char *qualified_method_name;
    size_t qualified_method_name_len;
    const char *file_name;
    size_t file_name_len;
};

struct frame_cache_lru_node {
    struct frame_cache_lru_node *next;
    struct frame_cache_lru_node *prev;
    struct frame_cache_key key;
    struct frame_cache_value value;
};

struct frame_cache {
    st_table *table;
    struct frame_cache_lru_node *lru_head;
    struct frame_cache_lru_node *lru_tail;
    size_t count;
    struct mpp_strtab *strtab;
    VALUE finalizer_callback;
};

static int frame_st_hash_compare(st_data_t arg1, st_data_t arg2) {
    struct frame_cache_key *k1 = (struct frame_cache_key *)arg1;
    struct frame_cache_key *k2 = (struct frame_cache_key *)arg2;

    if (k1->method_defined_on != k2->method_defined_on) {
        return k1->method_defined_on < k2->method_defined_on;
    } else {
        return k1->method_def < k2->method_def;
    }
}

static st_index_t frame_st_hash_hash(st_data_t arg) {
    struct frame_cache_key *k = (struct frame_cache_key *)arg;
    return st_hash(k, sizeof(struct frame_cache_key), FNV1_32A_INIT);
}

static const struct st_hash_type frame_st_hash_type = {
    .compare = frame_st_hash_compare,
    .hash = frame_st_hash_hash,
};

static void frame_cache_gc_mark(void *ptr);
static void frame_cache_gc_free(void *ptr);
static size_t frame_cache_gc_memsize(const void *ptr);
#ifdef HAVE_RB_GC_MARK_MOVABLE
static void frame_cache_gc_compact(void *ptr);
#endif
static void frame_cache_init(struct frame_cache *ch, struct mpp_strtab *strtab);
static void frame_cache_add(
    struct frame_cache *ch, struct frame_cache_key key, struct frame_cache_value value
);
static int frame_cache_add_updatefunc(
    st_data_t *key, st_data_t *value, st_data_t data, int existing
);
static bool frame_cache_lookup(
    struct frame_cache *ch, struct frame_cache_key key, struct frame_cache_value *value
);
static struct frame_cache *frame_cache_cdata;
static VALUE frame_cache_value;

static const rb_data_type_t frame_cache_cdata_type = {
    "collector_cdata",
    {
        frame_cache_gc_mark, frame_cache_gc_free, frame_cache_gc_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
        frame_cache_gc_compact,
#endif
        { 0 }, /* reserved */
    },
    /* parent, data, [ flags ] */
    NULL, NULL, 0
};

void mpp_setup_backtrace() {
    main_object = rb_funcall(
        rb_const_get(rb_cObject, rb_intern("TOPLEVEL_BINDING")),
        rb_intern("eval"),
        1, rb_str_new2("self")
    );

    frame_cache_value = TypedData_Make_Struct(
        rb_cObject, struct frame_cache, &frame_cache_cdata_type, frame_cache_cdata
    );
    rb_global_variable(&frame_cache_value);
}

static void frame_cache_gc_mark(void *ptr) {
    struct frame_cache *ch = (struct frame_cache *)ptr;

}

static void frame_cache_gc_free(void *ptr) {
    struct frame_cache *ch = (struct frame_cache *)ptr;
    struct frame_cache_lru_node *node = ch->lru_head;
    while (node) {
        struct frame_cache_lru_node *next = node->next;
        mpp_strtab_release(
            ch->strtab,
            node->value.qualified_method_name, node->value.qualified_method_name_len
        );
        mpp_strtab_release(
            ch->strtab,
            node->value.file_name, node->value.file_name_len
        );
        mpp_free(node);
        node = next;
    }
}

static size_t frame_cache_gc_memsize(const void *ptr) {
    return 0;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
static void frame_cache_gc_compact(void *ptr) {
    struct frame_cache *ch = (struct frame_cache *)ptr;
    ch->finalizer_callback = rb_gc_location(ch->finalizer_callback);

    struct frame_cache_lru_node *node = ch->lru_head;
    while (node) {
        VALUE new_method_defined_on = rb_gc_location(node->key.method_defined_on);
        VALUE new_method_def = rb_gc_locaton(node->key.method_def);
        if (
            new_method_defined_on != node->key.method_defined_on ||
            new_method_def != node->key.method_def
        ) {
            int already_existed = st_delete(ch->table, (st_data_t)&node->key, NULL);
            MPP_ASSERT_MSG(already_existed, "frame cache compact: node not found in table");
            node->key.method_def = new_method_defined_on;
            node->key.method_def = new_method_def;
            already_existed = st_insert(ch->table, (st_data_t)&node->key, (st_data_t)&node);
            MPP_ASSERT_MSG(!already_existed, "frame cache compact: double-insert of node");
        }
        node = node->next;
    }
}
#endif

static void frame_cache_init(struct frame_cache *ch, struct mpp_strtab *strtab) {
    ch->lru_head = NULL;
    ch->lru_tail = NULL;
    ch->table = st_init_table(&frame_st_hash_type);
    ch->count = 0;
    ch->strtab = strtab;
}

static void frame_cache_add(
    struct frame_cache *ch, struct frame_cache_key key, struct frame_cache_value value
) {
    struct frame_cache_lru_node *node = mpp_xmalloc(sizeof(struct frame_cache_lru_node));
    node->key = key;
    node->value = value;
    node->prev = NULL;
    node->next = ch->lru_head;
    ch->lru_head = node;
    int already_existed = st_insert(ch->table, (st_data_t)&key, (st_data_t)&node);
    MPP_ASSERT_MSG(!already_existed, "double-insert into frame LRU cache");
    ch->count++;

    rb_define_finalizer()
}

static bool frame_cache_lookup(
    struct frame_cache *ch, struct frame_cache_key key, struct frame_cache_value *value
) {
    struct frame_cache_lru_node *node;
    int exists = st_lookup(ch->table, (st_data_t)&key, (st_data_t *)&node);
    if (!exists) {
        return false;
    }

    // Object already exists in the cache, bump it up the LRU list.
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        // We were the tail.
        ch->lru_tail = node->prev;
    }
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        // We were the LRU head. Do nothing
    }

    if (ch->lru_head != node) {
        node->prev = NULL;
        node->next = ch->lru_head;
        ch->lru_head = node;
    }

    *value = node->value;
    return true;
}



unsigned long mpp_backtrace_frame_count(VALUE thread) {
    rb_thread_t *thread_data = (rb_thread_t *) DATA_PTR(thread);
    rb_execution_context_t *ec = thread_data->ec;

    const rb_control_frame_t *last_cfp = ec->cfp;
    // +2 because of the two dummy frames.
    const rb_control_frame_t *start_cfp = RUBY_VM_END_CONTROL_FRAME(GET_EC()) - 2;

    if (start_cfp < last_cfp) {
        return 0;
    } else {
        return (unsigned long)(start_cfp - last_cfp + 1);
    }
}

unsigned long mpp_capture_backtrace_frame(
    VALUE thread, unsigned long frame, struct mpp_backtrace_frame *frameout,
    struct mpp_strtab *strtab
) {
    rb_thread_t *thread_data = (rb_thread_t *) DATA_PTR(thread);
    rb_execution_context_t *ec = thread_data->ec;

    // The frame argument is zero-based with zero being "the frame closest to where execution is now"
    // (I couldn't decide if this was supposed to be the "top" or "bottom" of the callstack; but lower
    // frame argument --> more recently called function.
    
    const rb_control_frame_t *cfp = ec->cfp + frame;
    if (!RUBY_VM_VALID_CONTROL_FRAME_P(cfp, RUBY_VM_END_CONTROL_FRAME(ec) - 2)) {
        // +2 because of the two "dummy" frames at the bottom of the stack.
        // Means we're past the end of the stack. Return without MPP_BT_MORE_FRAMES,
        // to have our caller stop fetching frames.
        return 0;
    }

    const rb_callable_method_entry_t *cme = rb_vm_frame_method_entry(cfp);
    struct internal_frame_data frame_data = {
        .cfunc = false,
        .cme = cme,
        .iseq = cfp->iseq,
        .self = cfp->self,
    };

    if (cfp->iseq && !cfp->pc) {
        // Apparently means that this frame shouldn't appear in a backtrace
        return MPP_BT_MORE_FRAMES;
    } else if (VM_FRAME_RUBYFRAME_P(cfp)) {
        // A frame executing Ruby code
        frame_data.cfunc = false;
    } else if (cme && cme->def->type == VM_METHOD_TYPE_CFUNC) {
        // A frame executing C code
        frame_data.cfunc = true;
    } else {
        // Also shouldn't appear in backtraces.
        return MPP_BT_MORE_FRAMES;
    }

    char buf[256];
    struct mpp_strbuilder builder;
    mpp_strbuilder_init(&builder, buf, sizeof(buf));

    // Fill in the method name and intern it
    qualified_method_name_for_frame(frame_data, &builder);
    mpp_strtab_intern_strbuilder(
        strtab, &builder, &frameout->function_name, &frameout->function_name_len
    );

    mpp_strbuilder_init(&builder, buf, sizeof(buf));

    if (frame_data.cfunc) {
        // The built-in ruby stuff uses the next-highest Ruby frame as the filename for
        // cfuncs in backtraces. That's not all that useful, and also it's bit tricky to
        // keep track of that in one pass through the stack without any kind of dynamic
        // allocation. Just put some generic rubbish in the filename.
        mpp_strbuilder_append(&builder, "(cfunc)");
        frameout->line_number = 0;
    } else {
        iseq_path(frame_data.iseq, &builder);
        frameout->line_number = iseq_calc_lineno(frame_data.iseq, cfp->pc);
    }

    mpp_strtab_intern_strbuilder(
        strtab, &builder, &frameout->file_name, &frameout->file_name_len
    );

    return MPP_BT_MORE_FRAMES | MPP_BT_FRAME_VALID;
}


static void qualified_method_name_for_frame(
    struct internal_frame_data data, struct mpp_strbuilder *strout
) {
    method_qualifier(data, strout);
    method_name(data, strout);
}

static void method_qualifier(struct internal_frame_data data, struct mpp_strbuilder *strout) {
    VALUE defined_class = data.cme ? data.cme->defined_class : Qnil;
    VALUE class_of_defined_class = RTEST(defined_class) ? rb_class_of(defined_class) : Qnil;


    if (RTEST(class_of_defined_class) && FL_TEST(class_of_defined_class, RMODULE_IS_REFINEMENT)) {
        // The method being called is defined on a refinement.
        VALUE refinement_module = class_of_defined_class;
        mod_to_s_refinement(refinement_module, strout);
        mpp_strbuilder_append(strout, "#");
        return;
    }

    if (data.self == main_object) {
        // Special case - calling methods directly on the toplevel binding.
        mpp_strbuilder_append(strout, "Object$<main>#");
        return;
    } else if (data.self == rb_mRubyVMFrozenCore) {
        // Special case - this object is not accessible from Ruby, but
        // using main#lambda calls it.
        mpp_strbuilder_append(strout, "RubyVM::FrozenCore#");
        return;
    }

    VALUE self_class = rb_class_of(data.self);
    VALUE real_self_class = rb_class_real(self_class);

    if (RTEST(defined_class) && FL_TEST(defined_class, FL_SINGLETON)) {
        if (real_self_class == rb_cModule || real_self_class == rb_cClass) {
            // This is a special case - it means a singleton method is being called
            // directly on a module - e.g. if we have:
            //
            //   class Klazz; def self.moo; end; end;
            //
            // Then:
            //
            //    Klazz.moo => Should fall into this block.
            //    Klazz.new.singleton_class.moo => Will _not_ fall into this block.
            mod_to_s_singleton(defined_class, strout);
            mpp_strbuilder_append(strout, ".");
            return;
        }
    }

    if (CLASS_OR_MODULE_P(data.self)) {
        if (real_self_class == rb_cModule || real_self_class == rb_cClass) {
            // This special case means that self is a module/class instance, which means
            // we're executing code inside a module (i.e. module Foo; ...; end; )
            // In that case, we want to print "Foo" instead of "Module".
            mod_to_s(data.self, strout);
            mpp_strbuilder_append(strout, "#");
            return;    
        }
    }

    // The base case - use either the class on which the CME is defined, if we have a CME,
    // or else the class of self, as the method qualifier.
    VALUE method_target = RTEST(defined_class) ? defined_class : self_class;
    mod_to_s(method_target, strout);
    mpp_strbuilder_append(strout, "#");
}

static void method_name(struct internal_frame_data data, struct mpp_strbuilder *strout)  {
    if (RTEST(data.cme)) {
        // With a callable method entry, things are simple; just use that.
        VALUE method_name = rb_id2str(data.cme->called_id);
        mpp_strbuilder_append_value(strout, method_name);
        if (iseq_is_block_or_eval(data.iseq)) {
            mpp_strbuilder_append(strout, "{block}");
        }
    } else if (RTEST(data.iseq)) {
        // With no CME, we _DO NOT_ want to use iseq->base_label if we're a block, because otherwise
        // it will print something like "block in (something)". In fact, using the iseq->base_label
        // is pretty much a last resort. If we manage to write _anything_ else in our backtrace, we
        // won't use it.
        bool did_write_anything = false;
        if (RB_TYPE_P(data.self, T_CLASS)) {
            // No CME, and self being a class/module, means we're executing code inside a class Foo; ...; end;
            mpp_strbuilder_append(strout, "{class exec}");
            did_write_anything = true;
        }
        if (RB_TYPE_P(data.self, T_MODULE)) {
            mpp_strbuilder_append(strout, "{module exec}");
            did_write_anything = true;
        }
        if (iseq_is_block_or_eval(data.iseq)) {
            mpp_strbuilder_append(strout, "{block}");
            did_write_anything = true;
        }
        if (!did_write_anything) {
            // As a fallback, use whatever is on the base_label.
            VALUE location_name = data.iseq->body->location.base_label;
            mpp_strbuilder_append_value(strout, location_name);
        }
    } else {
        MPP_ASSERT_FAIL("don't know how to set method name");
    }
}

static void mod_to_s_singleton(VALUE klass, struct mpp_strbuilder *strout) {
    VALUE singleton_of = rb_class_real(klass);
    // If this is the singleton_class of a Class, or Module, we want to print
    // the _value_ of the object, and _NOT_ its class.
    // Basically:
    //    module MyModule; end;
    //    klazz = MyModule.singleton_class =>
    //        we want to output "MyModule"
    //       
    //    klazz = Something.new.singleton_class =>
    //        we want to output "Something"
    //
    if (singleton_of == rb_cModule || singleton_of == rb_cClass) {
        // The first case. Use the id_attached symbol to get what this is the
        // singleton_class _of_.
        st_lookup(RCLASS_IV_TBL(klass), id__attached__, (st_data_t *)&singleton_of);
    }
    mod_to_s(singleton_of, strout);
}

static void mod_to_s_anon(VALUE klass, struct mpp_strbuilder *strout) {
    // Anonymous module/class - print the name of the first non-anonymous super.
    // something like "#{klazz.ancestors.map(&:name).compact.first}$anonymous"
    //
    // Note that if klazz is a module, we want to do this on klazz.class, not klazz
    // itself:
    //
    //   irb(main):008:0> m = Module.new
    //   => #<Module:0x00000000021a7208>
    //   irb(main):009:0> m.ancestors
    //   => [#<Module:0x00000000021a7208>]
    //   # Not very useful - nothing with a name is in the ancestor chain
    //   irb(main):010:0> m.class.ancestors
    //   => [Module, Object, Kernel, BasicObject]
    //   # Much more useful - we can call this Module$anonymous.
    //
    VALUE superclass;
    VALUE superclass_name = Qnil;
    if (RB_TYPE_P(klass, T_CLASS)) {
        superclass = klass;
    } else {
        superclass = rb_class_of(klass);
    }

    do {
        superclass = RCLASS_SUPER(superclass);
        MPP_ASSERT_MSG(RTEST(superclass), "anonymous class has nil superclass");
        superclass_name = rb_mod_name(superclass);
    } while (!RTEST(superclass_name));
    mpp_strbuilder_append_value(strout, superclass_name);
}

static void mod_to_s_refinement(VALUE refinement_module, struct mpp_strbuilder *strout) {
    ID id_refined_class;
    CONST_ID(id_refined_class, "__refined_class__");
    VALUE refined_class = rb_attr_get(refinement_module, id_refined_class);
    ID id_defined_at;
    CONST_ID(id_defined_at, "__defined_at__");
    VALUE defined_at = rb_attr_get(refinement_module, id_defined_at);

    mod_to_s(refined_class, strout);
    mpp_strbuilder_append(strout, "$refinement@");
    mod_to_s(defined_at, strout);
}

static void mod_to_s(VALUE klass, struct mpp_strbuilder *strout) {
    if (!CLASS_OR_MODULE_P(klass) && !RB_TYPE_P(klass, T_ICLASS)) {
        // An instance of something was passed in here - get the class of it.
        klass = rb_class_of(klass);
    }
  
    if (FL_TEST(klass, FL_SINGLETON)) {
        mod_to_s_singleton(klass, strout);
        mpp_strbuilder_append(strout, "$singleton");
        return;
    }

    VALUE klass_name = rb_mod_name(klass);
    if (!RTEST(rb_mod_name(klass))) {
        mod_to_s_anon(klass, strout);
        mpp_strbuilder_append(strout, "$anonymous");
        return;
    }

    // Non-anonymous module/class.
    // something like "#{klazz.name}"
    mpp_strbuilder_append_value(strout, klass_name);
}

static bool iseq_is_block_or_eval(const rb_iseq_t *iseq) {
    if (!RTEST(iseq)) return false;
    return iseq->body->type == ISEQ_TYPE_BLOCK ||
        iseq->body->type == ISEQ_TYPE_EVAL;
}

// This is mostly a reimplementation of pathobj_path from vm_core.h
static void iseq_path(const rb_iseq_t *iseq, struct mpp_strbuilder *strout) {
    if (!RTEST(iseq)) {
        mpp_strbuilder_append(strout, "(unknown)");
        return;
    }

    VALUE pathobj = iseq->body->location.pathobj;
    VALUE path_str;
    if (RB_TYPE_P(pathobj, T_STRING)) {
	    path_str = pathobj;
    } else {
        MPP_ASSERT_MSG(RB_TYPE_P(pathobj, T_ARRAY), "pathobj in iseq was not array?");
        path_str = RARRAY_AREF(pathobj, PATHOBJ_PATH);
    }
    
    if (RTEST(path_str)) {
        mpp_strbuilder_append_value(strout, path_str);
    } else {
        mpp_strbuilder_append(strout, "(unknown)");
    }
}

// This is mostly a reimplementation of calc_lineno from vm_backtrace.c
static int iseq_calc_lineno(const rb_iseq_t *iseq, const VALUE *pc) {
    if (!RTEST(iseq)) {
        return 0;
    }
    if (!pc) {
        // This can happen during VM bootup.
        return 0;
    }
    else {
        size_t pos = pc - iseq->body->iseq_encoded;; // no overflow
        if (pos) {
            // use pos-1 because PC points next instruction at the beginning of instruction
            pos--;
        }
        return rb_iseq_line_no(iseq, pos);
    }
}
