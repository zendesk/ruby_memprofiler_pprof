#include "ruby_private.h"

#include "ruby_memprofiler_pprof.h"

// An implementation of rb_gc_disable_no_rest(), which is defined non-static in gc.c in >= 2.7
// but not given public symbol visibility.
VALUE mpp_rb_gc_disable_no_rest() {
  int old_dont_gc = GET_VM()->objspace->flags.dont_gc;
  GET_VM()->objspace->flags.dont_gc = 1;
  return old_dont_gc ? Qtrue : Qfalse;
}

// An implementation of rb_gc_memsize_of. Actually, in all Ruby versions >= 2.6, this has public
// symbol visibility, so just proxy through to it.
size_t mpp_rb_obj_memsize_of(VALUE obj) { return rb_obj_memsize_of(obj); }

// An implementation of is_pointer_to_heap, which is static in gc.c
static int mpp_is_pointer_to_heap(rb_objspace_t *objspace, void *ptr) {
  register RVALUE *p = RANY(ptr);
  register struct heap_page *page;
  register size_t hi, lo, mid;

  if (p < objspace->heap_pages.range[0] || p > objspace->heap_pages.range[1])
    return FALSE;
  if ((VALUE)p % sizeof(RVALUE) != 0)
    return FALSE;
  /* check if p looks like a pointer using bsearch*/
  lo = 0;
  hi = objspace->heap_pages.allocated_pages;
  while (lo < hi) {
    mid = (lo + hi) / 2;
    page = objspace->heap_pages.sorted[mid];
    if (page->start <= p) {
#ifdef HAVE_VARIABLE_SLOT_SIZE
      // >= Ruby 3.1
      if ((uintptr_t)p < ((uintptr_t)page->start + (page->total_slots * page->slot_size))) {
#else
      // <= Ruby 3.0
      if (p < page->start + page->total_slots) {
#endif
        if (page->flags.in_tomb) {
          return FALSE;
        } else {
#ifdef HAVE_VARIABLE_SLOT_SIZE
          if ((NUM_IN_PAGE(p) * sizeof(RVALUE)) % page->slot_size != 0)
            return FALSE;
#endif
          return TRUE;
        }
      }
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return FALSE;
}

// Answers the question, would rb_obj_memsize_of crash on this object?
bool mpp_is_value_still_validish(VALUE obj) {
  if (obj == Qundef) {
    return false;
  }
  if (!mpp_is_pointer_to_heap(GET_VM()->objspace, (void *)obj)) {
    return false;
  }
  int type = RB_BUILTIN_TYPE(obj);
  // do NOT return true for T_NODE; rb_obj_memsize_of() can't handle it.
  switch (type) {
  case T_OBJECT:
  case T_MODULE:
  case T_CLASS:
  case T_ICLASS:
  case T_STRING:
  case T_ARRAY:
  case T_HASH:
  case T_REGEXP:
  case T_DATA:
  case T_MATCH:
  case T_FILE:
  case T_RATIONAL:
  case T_COMPLEX:
  case T_IMEMO:
  case T_FLOAT:
  case T_SYMBOL:
  case T_BIGNUM:
  case T_STRUCT:
    return true;
  }
  return false;
}

// Peeks into internal GVL structures to spy if someone else is waiting for the GVL; we can
// then be polite and yield it for them.
bool mpp_is_someone_else_waiting_for_gvl() {
  rb_global_vm_lock_t gvl;
#ifdef HAVE_GET_RACTOR
  gvl = GET_RACTOR()->threads.gvl;
#else
  gvl = GET_VM()->gvl;
#endif
  pthread_mutex_lock(&gvl.lock);
  bool someone_waiting = list_empty(&gvl.waitq);
  pthread_mutex_unlock(&gvl.lock);
  return someone_waiting;
}

// Unfreezes a passed in object so we can force setting something on
// its internal attributes hash.
VALUE mpp_rb_ivar_set_ignore_frozen(VALUE obj, ID key, VALUE value) {
  bool was_frozen = RB_OBJ_FROZEN(obj);
  RB_FL_UNSET_RAW(obj, FL_FREEZE);
  VALUE ret = rb_ivar_set(obj, key, value);
  if (was_frozen) {
    RB_FL_SET_RAW(obj, FL_FREEZE);
  }
  return ret;
}
