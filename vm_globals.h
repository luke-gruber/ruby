#ifndef RUBY_VM_GLOBALS_H
#define RUBY_VM_GLOBALS_H
/**********************************************************************

  vm_globals.h -

**********************************************************************/

#include "ruby/atomic.h"
#include "ruby/internal/dllexport.h"
#include "ruby/internal/stdbool.h"
#include "ruby/internal/static_assert.h"
#include "ruby/internal/stdalign.h"

struct rb_ractor_struct;
struct rb_vm_struct;

/* Globals the hottest GC paths read together: gc_current_objspace_of() on every
 * objspace resolution, the write barrier on every reference store.  One over-aligned
 * struct costs them a single page-relative address and a single cache line instead of
 * one of each per global.  Add a member only if those paths read it, and only if it is
 * written once -- a word stored at runtime would false-share the line. */
struct rb_vm_globals {
    struct rb_ractor_struct *single_main_ractor; /* ractor.c */
    struct rb_vm_struct *current_vm_ptr;         /* vm.c */

    /* Nonzero once the cleanup walk has stashed an objspace, which is the only case
     * where a thread needs one other than its own Ractor's.  One unsynchronised load
     * gates it, keeping the per-reference marking path and the malloc paths clear of
     * the slow resolution.  It is never unwound, so a stale zero only costs a
     * resolution that predates the stash (gc.c). */
    rb_atomic_t gc_objspace_override_count;

    /* Stays true once the process has gone multi-Ractor (rb_multi_ractor_p goes back
     * to false when the other Ractors finish).  Used by verification that spans
     * generation state built while multiple Ractors ran (gc.c). */
    bool gc_ever_multi_ractor;
};

/* 32 divides both the 64- and 128-byte line sizes, so a struct this size can never
 * straddle one.  Grow the alignment if the struct outgrows it. */
RUBY_EXTERN RBIMPL_ALIGNAS(32) struct rb_vm_globals ruby_vm_globals;
RBIMPL_STATIC_ASSERT(rb_vm_globals_fits_one_cache_line, sizeof(struct rb_vm_globals) <= 32);

#endif /* RUBY_VM_GLOBALS_H */
