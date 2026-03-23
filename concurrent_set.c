#include "internal.h"
#include "internal/gc.h"
#include "internal/concurrent_set.h"
#include "ruby/atomic.h"
#include "vm_sync.h"

#define CONCURRENT_SET_CONTINUATION_BIT ((VALUE)0x2)
#define CONCURRENT_SET_KEY_MASK (~CONCURRENT_SET_CONTINUATION_BIT)

enum concurrent_set_special_values {
    CONCURRENT_SET_EMPTY = 0,
    CONCURRENT_SET_DELETED = 1,
    CONCURRENT_SET_MOVED = 5, // continuation bit is 0x02, so 0x05 doesn't have bits in conflict with it
    CONCURRENT_SET_SPECIAL_VALUE_COUNT = 6,
};

# define CONCURRENT_SET_HASH_DELETED ((VALUE)1)

struct concurrent_set_entry {
    VALUE hash;
    VALUE key;
};

struct concurrent_set {
    rb_atomic_t size;
    unsigned int capacity;
    rb_atomic_t deleted_entries;
    const struct rb_concurrent_set_funcs *funcs;
    struct concurrent_set_entry *entries;
};

static bool
concurrent_set_mark_continuation(struct concurrent_set_entry *entry, VALUE raw_key)
{
    if (raw_key & CONCURRENT_SET_CONTINUATION_BIT) return true;

    VALUE new_key = raw_key | CONCURRENT_SET_CONTINUATION_BIT; // NOTE: raw_key can be CONCURRENT_SET_EMPTY
    VALUE prev_key = rbimpl_atomic_value_cas(&entry->key, raw_key, new_key, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_ACQUIRE);

    if (prev_key == raw_key || prev_key == new_key) {
        return true;
    }
    else if ((prev_key & CONCURRENT_SET_KEY_MASK) == CONCURRENT_SET_DELETED) {
        return true; // key has been made DELETED (tombstone)
    }
    else {
        // key has been made EMPTY, and anything could have happened to this slot since then. Need to retry.
        return false;
    }
}

static VALUE
concurrent_set_hash(const struct concurrent_set *set, VALUE key)
{
    VALUE hash = set->funcs->hash(key);
    if (hash == 0) hash = ~(VALUE)0;
    if (hash == CONCURRENT_SET_HASH_DELETED) hash++;
    RUBY_ASSERT(hash != 0);
    return hash;
}

static void
concurrent_set_free(void *ptr)
{
    struct concurrent_set *set = ptr;
    xfree(set->entries);
}

static size_t
concurrent_set_size(const void *ptr)
{
    const struct concurrent_set *set = ptr;
    return sizeof(struct concurrent_set) +
        (set->capacity * sizeof(struct concurrent_set_entry));
}

/* Hack: Though it would be trivial, we're intentionally avoiding WB-protecting
 * this object. This prevents the object from aging and ensures it can always be
 * collected in a minor GC.
 * Longer term this deserves a better way to reclaim memory promptly.
 */
static void
concurrent_set_mark(void *ptr)
{
    (void)ptr;
}

static const rb_data_type_t concurrent_set_type = {
    .wrap_struct_name = "VM/concurrent_set",
    .function = {
        .dmark = concurrent_set_mark,
        .dfree = concurrent_set_free,
        .dsize = concurrent_set_size,
    },
    /* Hack: NOT WB_PROTECTED on purpose (see above) */
    /* Note: NOT RUBY_TYPED_EMBEDDABLE. The struct must be xmalloc'd separately
     * so that rb_concurrent_set_delete_by_identity can access it via a cached
     * pointer without dereferencing the VALUE (which may be on an mprotected
     * page during compaction). */
    .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_CONCURRENT_FREE_SAFE
};

VALUE
rb_concurrent_set_new(const struct rb_concurrent_set_funcs *funcs, int capacity)
{
    struct concurrent_set *set;
    VALUE obj = TypedData_Make_Struct(0, struct concurrent_set, &concurrent_set_type, set);
    set->funcs = funcs;
    set->entries = ZALLOC_N(struct concurrent_set_entry, capacity);
    set->capacity = capacity;
    return obj;
}

void *
rb_concurrent_set_get_data(VALUE set_obj)
{
    return RTYPEDDATA_GET_DATA(set_obj);
}

rb_atomic_t
rb_concurrent_set_size(VALUE set_obj)
{
    struct concurrent_set *set = RTYPEDDATA_GET_DATA(set_obj);

    return RUBY_ATOMIC_LOAD(set->size);
}

struct concurrent_set_probe {
    int idx;
    int d;
    int mask;
};

static int
concurrent_set_probe_start(struct concurrent_set_probe *probe, struct concurrent_set *set, VALUE hash)
{
    RUBY_ASSERT((set->capacity & (set->capacity - 1)) == 0);
    probe->d = 0;
    probe->mask = set->capacity - 1;
    probe->idx = hash & probe->mask;
    return probe->idx;
}

static int
concurrent_set_probe_next(struct concurrent_set_probe *probe)
{
    probe->d++;
    probe->idx = (probe->idx + probe->d) & probe->mask;
    return probe->idx;
}

static void
concurrent_set_try_resize_locked(VALUE old_set_obj, VALUE *set_obj_ptr, void **set_data_ptr)
{
    // Check if another thread has already resized.
    if (rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE) != old_set_obj) {
        return;
    }

    struct concurrent_set *old_set = RTYPEDDATA_GET_DATA(old_set_obj);

    // This may overcount by up to the number of threads concurrently attempting to insert
    // GC may also happen between now and the set being rebuilt
    int expected_size = rbimpl_atomic_load(&old_set->size, RBIMPL_ATOMIC_RELAXED) - old_set->deleted_entries;

    // NOTE: new capacity must make sense with load factor, don't change one without checking the other.
    struct concurrent_set_entry *old_entries = old_set->entries;
    int old_capacity = old_set->capacity;
    int new_capacity = old_capacity * 2;
    if (new_capacity > expected_size * 8) {
        new_capacity = old_capacity / 2;
    }
    else if (new_capacity > expected_size * 4) {
        new_capacity = old_capacity;
    }

    // May cause GC and therefore deletes, so must happen first.
    VALUE new_set_obj = rb_concurrent_set_new(old_set->funcs, new_capacity);
    struct concurrent_set *new_set = RTYPEDDATA_GET_DATA(new_set_obj);

    for (int i = 0; i < old_capacity; i++) {
        struct concurrent_set_entry *old_entry = &old_entries[i];
        VALUE prev_key_raw = rbimpl_atomic_value_exchange(&old_entry->key, CONCURRENT_SET_MOVED, RBIMPL_ATOMIC_ACQUIRE);
        VALUE prev_key = prev_key_raw & CONCURRENT_SET_KEY_MASK;
        RUBY_ASSERT(prev_key != CONCURRENT_SET_MOVED);

        if (prev_key < CONCURRENT_SET_SPECIAL_VALUE_COUNT) continue;

        if (!RB_SPECIAL_CONST_P(prev_key) && rb_objspace_garbage_object_p(prev_key)) continue;

        VALUE hash = rbimpl_atomic_value_load(&old_entry->hash, RBIMPL_ATOMIC_ACQUIRE);
        if (hash == CONCURRENT_SET_HASH_DELETED) continue;
        RUBY_ASSERT(hash != 0);
        if (concurrent_set_hash(old_set, prev_key) != hash) { // entry was deleted, then hash was changed but key not yet
            continue;
        }

        // Insert key into new_set.
        struct concurrent_set_probe probe;
        int idx = concurrent_set_probe_start(&probe, new_set, hash);

        while (true) {
            struct concurrent_set_entry *entry = &new_set->entries[idx];

            if (entry->hash == 0) {
                RUBY_ASSERT(entry->key == CONCURRENT_SET_EMPTY);

                new_set->size++;
                RUBY_ASSERT(new_set->size <= new_set->capacity / 2);

                entry->key = prev_key; // no continuation bit
                entry->hash = hash;
                break;
            }

            RUBY_ASSERT(entry->key >= CONCURRENT_SET_SPECIAL_VALUE_COUNT);
            entry->key |= CONCURRENT_SET_CONTINUATION_BIT;
            idx = concurrent_set_probe_next(&probe);
        }
    }

    if (set_data_ptr) {
        rbimpl_atomic_value_store((VALUE *)set_data_ptr, (VALUE)new_set, RBIMPL_ATOMIC_RELEASE);
    }
    rbimpl_atomic_value_store(set_obj_ptr, new_set_obj, RBIMPL_ATOMIC_RELEASE);

    RB_GC_GUARD(old_set_obj);
}

// FIXME: cross-platform initializer
static pthread_rwlock_t resize_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_t resize_lock_owner;
static unsigned int resize_lock_lvl;

static inline void
resize_lock_wrlock(bool allow_reentry)
{
    if (allow_reentry && pthread_self() == resize_lock_owner) {
        // Already held by this thread.
    }
    else {
        int r;
        if ((r = pthread_rwlock_wrlock(&resize_lock))) {
            rb_bug_errno("pthread_rwlock_wrlock", r);
        }
        resize_lock_owner = pthread_self();
    }
    resize_lock_lvl++;
}

static inline void
resize_lock_wrunlock(void)
{
    RUBY_ASSERT(resize_lock_lvl > 0);
    resize_lock_lvl--;
    if (resize_lock_lvl == 0) {
        resize_lock_owner = 0;
        int r;
        if ((r = pthread_rwlock_unlock(&resize_lock))) {
            rb_bug_errno("pthread_rwlock_unlock", r);
        }
    }
}

static inline bool
resize_lock_rdlock(void)
{
    if (resize_lock_owner == pthread_self()) { // we have the write lock, don't take it
        return false;
    }
    int r;
    if ((r = pthread_rwlock_rdlock(&resize_lock))) {
        rb_bug_errno("pthread_rwlock_rdlock", r);
    }
    return true;
}

static inline void
resize_lock_rdunlock(void)
{
    int r;
    if ((r = pthread_rwlock_unlock(&resize_lock))) {
        rb_bug_errno("pthread_rwlock_unlock", r);
    }
}

static void
concurrent_set_try_resize(VALUE old_set_obj, VALUE *set_obj_ptr, void **set_data_ptr)
{
    RB_VM_LOCKING() {
        // deletes from sweep thread must not happen during resize and sweep thread can't take VM lock so it takes the resize lock
        resize_lock_wrlock(true);
        {
            concurrent_set_try_resize_locked(old_set_obj, set_obj_ptr, set_data_ptr);
        }
        resize_lock_wrunlock();
    }
}

VALUE
rb_concurrent_set_find(VALUE *set_obj_ptr, VALUE key)
{
    RUBY_ASSERT(key >= CONCURRENT_SET_SPECIAL_VALUE_COUNT);

    VALUE set_obj;
    VALUE hash = 0;
    struct concurrent_set *set;
    struct concurrent_set_probe probe;
    int idx;

  retry:
    set_obj = rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE);
    RUBY_ASSERT(set_obj);
    set = RTYPEDDATA_GET_DATA(set_obj);

    if (hash == 0) {
        // We don't need to recompute the hash on every retry because it should
        // never change.
        hash = concurrent_set_hash(set, key);
    }
    RUBY_ASSERT(hash == concurrent_set_hash(set, key));

    idx = concurrent_set_probe_start(&probe, set, hash);

    while (true) {
        struct concurrent_set_entry *entry = &set->entries[idx];
        VALUE curr_hash = rbimpl_atomic_value_load(&entry->hash, RBIMPL_ATOMIC_ACQUIRE);

        if (curr_hash == 0) {
            return 0;
        }
        else if (curr_hash == CONCURRENT_SET_HASH_DELETED) {
            idx = concurrent_set_probe_next(&probe);
            continue;
        }

        VALUE raw_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_ACQUIRE);
        VALUE curr_key = raw_key & CONCURRENT_SET_KEY_MASK;
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;

        if (curr_hash != hash) {
            if (!continuation) {
                return 0;
            }
            idx = concurrent_set_probe_next(&probe);
            continue;
        }

        switch (curr_key) {
          case CONCURRENT_SET_EMPTY:
            // In-progress insert: hash written but key not yet.
            break;
          case CONCURRENT_SET_DELETED:
            break;
          case CONCURRENT_SET_MOVED:
            // Wait
            RB_VM_LOCKING();

            goto retry;
          default: {
            if (UNLIKELY(!RB_SPECIAL_CONST_P(curr_key) && rb_objspace_garbage_object_p(curr_key))) {
                // This is a weakref set, so after marking but before sweeping is complete we may find a matching garbage object.
                // Skip it and let the GC pass clean it up
                break;
            }

            if (set->funcs->cmp(key, curr_key)) {
                // We've found a match.
                RB_GC_GUARD(set_obj);
                return curr_key;
            }

            if (!continuation) {
                return 0;
            }

            break;
          }
        }

        idx = concurrent_set_probe_next(&probe);
    }
}

VALUE
rb_concurrent_set_find_or_insert(VALUE *set_obj_ptr, VALUE key, void *data, void **set_data_ptr)
{
    RUBY_ASSERT(key >= CONCURRENT_SET_SPECIAL_VALUE_COUNT);

    // First attempt to find
    {
        VALUE result = rb_concurrent_set_find(set_obj_ptr, key);
        if (result) return result;
    }

    // First time we need to call create, and store the hash
    VALUE set_obj = rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE);
    RUBY_ASSERT(set_obj);

    struct concurrent_set *set = RTYPEDDATA_GET_DATA(set_obj);
    key = set->funcs->create(key, data); // this can join GC (takes VM Lock)
    VALUE hash = concurrent_set_hash(set, key);

    struct concurrent_set_probe probe;
    int idx;

    goto start_search;

retry:
    // On retries we only need to load the hash object
    set_obj = rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE);
    RUBY_ASSERT(set_obj);
    set = RTYPEDDATA_GET_DATA(set_obj);

    RUBY_ASSERT(hash == concurrent_set_hash(set, key));

start_search:
    idx = concurrent_set_probe_start(&probe, set, hash);

    while (true) {
        struct concurrent_set_entry *entry = &set->entries[idx];
        bool can_continue_probing;
        VALUE curr_hash = rbimpl_atomic_value_load(&entry->hash, RBIMPL_ATOMIC_ACQUIRE);
        if (curr_hash == 0) {
            // Reserve this slot for our hash value
            curr_hash = rbimpl_atomic_value_cas(&entry->hash, 0, hash, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
            if (curr_hash != 0) {
                // Lost race, retry same slot to check winner's hash
                /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: hash CAS lost at idx=%d (winner hash=%lx)\n",
                        (void *)key, (unsigned long)hash, idx, (unsigned long)curr_hash);*/
                continue;
            }
            /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: claimed empty hash slot at idx=%d\n",
                    (void *)key, (unsigned long)hash, idx);*/
            curr_hash = hash;
            // Fall through to try to claim key
        }

        VALUE raw_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_ACQUIRE);
        VALUE curr_key = raw_key & CONCURRENT_SET_KEY_MASK;
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;

        /*fprintf(stderr, "find_or_insert: curr_key:%p, curr_hash:%p\n", (void*)curr_key, (void*)curr_hash);*/
        switch (curr_key) {
          case CONCURRENT_SET_EMPTY: {
            if (curr_hash != hash && curr_hash != CONCURRENT_SET_HASH_DELETED) { // another thread won and this was a pristine slot
                goto probe_next;
            }
            if (curr_hash == CONCURRENT_SET_HASH_DELETED) {
                // Reserve this slot for our hash value
                VALUE prev_hash = rbimpl_atomic_value_cas(&entry->hash, CONCURRENT_SET_HASH_DELETED, hash, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
                if (prev_hash != CONCURRENT_SET_HASH_DELETED) {
                    // Lost race, retry same slot to check winner's hash
                    /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: HASH_DELETED reclaim CAS lost at idx=%d (prev_hash=%lx)\n",
                            (void *)key, (unsigned long)hash, idx, (unsigned long)prev_hash);*/
                    continue;
                }
                /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: reclaimed HASH_DELETED slot at idx=%d\n",
                        (void *)key, (unsigned long)hash, idx);*/
            }
            rb_atomic_t prev_size = rbimpl_atomic_fetch_add(&set->size, 1, RBIMPL_ATOMIC_RELAXED);

            // Load_factor reached at 75% full. ex: prev_size: 32, capacity: 64, load_factor: 50%.
            bool load_factor_reached = (uint64_t)(prev_size * 4) >= (uint64_t)(set->capacity * 3);

            if (UNLIKELY(load_factor_reached)) {
                concurrent_set_try_resize(set_obj, set_obj_ptr, set_data_ptr);
                goto retry;
            }

            VALUE prev_raw_key = rbimpl_atomic_value_cas(&entry->key, raw_key, key | (continuation ? CONCURRENT_SET_CONTINUATION_BIT : 0), RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
            if (prev_raw_key == raw_key) {
                /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: INSERTED at idx=%d (continuation=%d) capacity=%u size=%u\n",
                        (void *)key, (unsigned long)hash, idx, continuation, set->capacity, (unsigned)set->size);*/
                RB_GC_GUARD(set_obj);
                return key;
            }
            else {
                // Entry was not inserted.
                /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: key CAS LOST at idx=%d (prev_raw_key=%lx), retrying\n",
                        (void *)key, (unsigned long)hash, idx, (unsigned long)prev_raw_key);*/
                rbimpl_atomic_sub(&set->size, 1, RBIMPL_ATOMIC_RELAXED);

                // * Another thread with the same hash won the race, try again at the same location, we might find it.
                // * A resize could also be underway, and `prev_raw_key` could be CONCURRENT_SET_MOVED.
                // * The continuation bit could also have been set on the key just now, in which case we'll retry
                continue;
            }
          }
          case CONCURRENT_SET_DELETED:
            break;
          case CONCURRENT_SET_MOVED:
            // Wait
            RB_VM_LOCKING();
            goto retry;
          default:
            if (curr_hash == CONCURRENT_SET_HASH_DELETED) { // other thread is in the middle of a delete, let's wait
                /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: found HASH_DELETED on live key=%p at idx=%d, waiting for delete to finish\n",
                        (void *)key, (unsigned long)hash, (void *)curr_key, idx);*/
                RB_VM_LOCKING() {
                    resize_lock_wrlock(false);
                    resize_lock_wrunlock();
                }
                continue;
            }
            if (curr_hash != hash) {
                goto probe_next;
            }
            // We're never GC during our search
            // If the continuation bit wasn't set at the start of our search,
            // any concurrent find with the same hash value would also look at
            // this location and try to swap curr_key
            if (UNLIKELY(!RB_SPECIAL_CONST_P(curr_key) && rb_objspace_garbage_object_p(curr_key))) {
                if (continuation) {
                    /*fprintf(stderr, "[INSERT-DEBUG] GARBAGE key=%p at idx=%d (continuation=1, hash=%lx entry_hash=%lx), skipping to probe_next\n",
                            (void *)curr_key, idx, (unsigned long)hash, (unsigned long)curr_hash);*/
                    goto probe_next;
                }
                /*fprintf(stderr, "[INSERT-DEBUG] GARBAGE key=%p at idx=%d (continuation=0, hash=%lx entry_hash=%lx), clearing key to EMPTY (hash NOT cleared)\n",
                        (void *)curr_key, idx, (unsigned long)hash, (unsigned long)curr_hash);*/
                // NOTE: entry->key could be a different key, but we can't call the comparison function because it's a garbage object.
                /*rbimpl_atomic_value_cas(&entry->hash, curr_hash, 0, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);*/
                VALUE prev_garbage_key = rbimpl_atomic_value_cas(&entry->key, raw_key, CONCURRENT_SET_EMPTY, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
                /*fprintf(stderr, "[INSERT-DEBUG] GARBAGE clear CAS: prev_key=%lx (expected raw_key=%lx, success=%d)\n",
                        (unsigned long)prev_garbage_key, (unsigned long)raw_key, prev_garbage_key == raw_key);*/
                // NOTE: hash not updated, so our hash value is stuck to this slot until a resize occurs.
                continue;
            }

            if (set->funcs->cmp(key, curr_key)) {
                // We've found a live match.
                /*fprintf(stderr, "[INSERT-DEBUG] key=%p hash=%lx: FOUND existing match curr_key=%p at idx=%d\n",
                        (void *)key, (unsigned long)hash, (void *)curr_key, idx);*/
                RB_GC_GUARD(set_obj);

                // We created key using set->funcs->create, but we didn't end
                // up inserting it into the set. Free it here to prevent memory
                // leaks.
                if (set->funcs->free) set->funcs->free(key);

                return curr_key;
            }
            break;
        }

      probe_next:
        can_continue_probing =  concurrent_set_mark_continuation(entry, raw_key);
        if (!can_continue_probing) {
            continue;
        }
        idx = concurrent_set_probe_next(&probe);
    }
}

static void
concurrent_set_delete_entry_locked(struct concurrent_set *set, struct concurrent_set_entry *entry)
{
    ASSERT_vm_locking_with_barrier();

    VALUE old_key = entry->key & CONCURRENT_SET_KEY_MASK;
    VALUE old_hash = entry->hash;
    int entry_idx = (int)(entry - set->entries);

    if (entry->key & CONCURRENT_SET_CONTINUATION_BIT) {
        /*fprintf(stderr, "[DELETE-ENTRY-DEBUG] key=%p hash=%lx at idx=%d: setting DELETED (continuation=1)\n",
                (void *)old_key, (unsigned long)old_hash, entry_idx);*/
        entry->key = CONCURRENT_SET_DELETED | CONCURRENT_SET_CONTINUATION_BIT;
        set->deleted_entries++;
    }
    else {
        /*fprintf(stderr, "[DELETE-ENTRY-DEBUG] key=%p hash=%lx at idx=%d: setting EMPTY (continuation=0), clearing hash\n",
                (void *)old_key, (unsigned long)old_hash, entry_idx);*/
        entry->hash = 0;
        entry->key = CONCURRENT_SET_EMPTY;
        set->size--;
    }
}


static VALUE
rb_concurrent_set_delete_by_identity_locked(struct concurrent_set *set, VALUE key)
{

    VALUE hash = concurrent_set_hash(set, key);

    /*fprintf(stderr, "[DELETE-DEBUG] ENTER delete key=%p hash=%lx capacity=%u size=%u deleted=%u\n",
            (void *)key, (unsigned long)hash, set->capacity, (unsigned)set->size, (unsigned)set->deleted_entries);*/

    struct concurrent_set_probe probe;
    int idx = concurrent_set_probe_start(&probe, set, hash);
    bool hash_cleared = false;

    while (true) {
        struct concurrent_set_entry *entry = &set->entries[idx];
        VALUE raw_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_ACQUIRE);
        VALUE loaded_hash = rbimpl_atomic_value_load(&entry->hash, RBIMPL_ATOMIC_ACQUIRE);
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;
        VALUE curr_key = raw_key & CONCURRENT_SET_KEY_MASK;

        switch (curr_key) {
          case CONCURRENT_SET_EMPTY:
            if (!continuation) {
                /*fprintf(stderr, "[DELETE-DEBUG] key=%p hash=%lx: NOT FOUND (EMPTY, no continuation) at idx=%d\n",
                        (void *)key, (unsigned long)hash, idx);*/
                return 0;
            }
            /*fprintf(stderr, "[DELETE-DEBUG] key=%p hash=%lx: EMPTY with continuation at idx=%d, continuing probe\n",
                    (void *)key, (unsigned long)hash, idx);*/
            break;
          case CONCURRENT_SET_DELETED:
            /*fprintf(stderr, "[DELETE-DEBUG] key=%p hash=%lx: DELETED slot at idx=%d (continuation=%d)\n",
                    (void *)key, (unsigned long)hash, idx, continuation);*/
            break;
          case CONCURRENT_SET_MOVED:
            rb_bug("rb_concurrent_set_delete_by_identity: moved entry");
            break;
          default:
            if (key == curr_key) {
                VALUE new_key;
                /*if (hash == CONCURRENT_SET_HASH_DELETED) {
                    fprintf(stderr, "[DELETE-DEBUG] BUG: hash == CONCURRENT_SET_HASH_DELETED! key=%p hash=%lx\n",
                            (void *)key, (unsigned long)hash);
                }
                if (loaded_hash != hash) {
                    fprintf(stderr, "[DELETE-DEBUG] ASSERTION WILL FAIL: loaded_hash != hash\n");
                    fprintf(stderr, "[DELETE-DEBUG]   key=%p (matched curr_key)\n", (void *)key);
                    fprintf(stderr, "[DELETE-DEBUG]   expected hash=%lx, loaded_hash=%lx\n",
                            (unsigned long)hash, (unsigned long)loaded_hash);
                    fprintf(stderr, "[DELETE-DEBUG]   loaded_hash == HASH_DELETED? %d\n",
                            loaded_hash == CONCURRENT_SET_HASH_DELETED);
                    fprintf(stderr, "[DELETE-DEBUG]   idx=%d, capacity=%u, size=%u, deleted=%u\n",
                            idx, set->capacity, (unsigned)set->size, (unsigned)set->deleted_entries);
                    fprintf(stderr, "[DELETE-DEBUG]   raw_key=%lx, continuation=%d\n",
                            (unsigned long)raw_key, continuation);
                    fprintf(stderr, "[DELETE-DEBUG]   hash_cleared=%d\n", hash_cleared);
                    VALUE reread_hash = rbimpl_atomic_value_load(&entry->hash, RBIMPL_ATOMIC_SEQ_CST);
                    VALUE reread_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_SEQ_CST);
                    fprintf(stderr, "[DELETE-DEBUG]   re-read hash=%lx, re-read key=%lx\n",
                            (unsigned long)reread_hash, (unsigned long)reread_key);
                    VALUE recomputed = concurrent_set_hash(set, key);
                    fprintf(stderr, "[DELETE-DEBUG]   recomputed hash=%lx (matches original? %d)\n",
                            (unsigned long)recomputed, recomputed == hash);
                }*/
                RUBY_ASSERT(hash != CONCURRENT_SET_HASH_DELETED);
                RUBY_ASSERT(loaded_hash == hash);
                if (continuation) {
                    new_key = CONCURRENT_SET_DELETED | CONCURRENT_SET_CONTINUATION_BIT;
                }
                else {
                    new_key = CONCURRENT_SET_EMPTY;
                }

                if (!hash_cleared) {
                    // This is safe because we aren't racing against other deletions (only 1 thread can delete a key), so
                    // we know that the key isn't a different key by now and we aren't deleting a hash out from under it.
                    VALUE prev_hash = rbimpl_atomic_value_cas(&entry->hash, hash, CONCURRENT_SET_HASH_DELETED, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_ACQUIRE);
                    if (prev_hash == CONCURRENT_SET_HASH_DELETED) {
                        // Another thread already started deleting this entry?
                        // The key may have already been cleared too.
                        /*fprintf(stderr, "[DELETE-DEBUG] key=%p: hash CAS lost (already HASH_DELETED) at idx=%d, returning\n",
                                (void *)key, idx);*/
                        return curr_key;
                    }
                    RUBY_ASSERT(prev_hash == hash);
                    hash_cleared = true;
                }
                VALUE prev_key = rbimpl_atomic_value_cas(&entry->key, raw_key, new_key, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_ACQUIRE);
                if (prev_key == raw_key) {
                    if (continuation) {
                        rbimpl_atomic_add(&set->deleted_entries, 1, RBIMPL_ATOMIC_RELAXED);
                    }
                    else {
                        rbimpl_atomic_sub(&set->size, 1, RBIMPL_ATOMIC_RELAXED);
                    }
                    /*fprintf(stderr, "[DELETE-DEBUG] key=%p: SUCCESS deleted at idx=%d (continuation=%d)\n",
                            (void *)key, idx, continuation);*/
                    return curr_key;
                }
                else if (!continuation && prev_key == (raw_key | CONCURRENT_SET_CONTINUATION_BIT)) {
                    /*fprintf(stderr, "[DELETE-DEBUG] key=%p: continuation bit set during delete at idx=%d, retrying\n",
                            (void *)key, idx);*/
                    continue; // try again, the continuation bit was just set on this key
                } else if ((prev_key & CONCURRENT_SET_KEY_MASK) == CONCURRENT_SET_EMPTY || (prev_key & CONCURRENT_SET_KEY_MASK) == CONCURRENT_SET_DELETED) {
                    /*fprintf(stderr, "[DELETE-DEBUG] key=%p: key already deleted by another thread at idx=%d (prev_key=%lx)\n",
                            (void *)key, idx, (unsigned long)prev_key);*/
                    return curr_key; // the key was deleted by another thread (currently not possible due to how our GC works?)
                }
                else { // the key was deleted to EMPTY and then a new key put there
                    /*fprintf(stderr, "[DELETE-DEBUG] key=%p: slot reused at idx=%d (prev_key=%lx)\n",
                            (void *)key, idx, (unsigned long)prev_key);*/
                    return curr_key;
                }
            }
            if (!continuation) return 0;
            break;
        }

        idx = concurrent_set_probe_next(&probe);
    }
}

// This can be called concurrently by a ruby GC thread and the sweep thread.
// Uses set_data_ptr (a cached xmalloc'd struct pointer) to avoid dereferencing
// the VALUE set_obj, which may be on an mprotected page during compaction.
VALUE
rb_concurrent_set_delete_by_identity(VALUE *set_obj_ptr, void **set_data_ptr, VALUE key)
{
    VALUE result;
    bool is_sweep_thread_p(void);
    bool in_background_sweep_mode(void);
    struct concurrent_set *set = (struct concurrent_set *)rbimpl_atomic_value_load((VALUE *)set_data_ptr, RBIMPL_ATOMIC_ACQUIRE);

    if (is_sweep_thread_p()) {
        while (1) {
            // this can be called by sweep thread, so we need to make sure no resize or replace is taking place on the object
            // However, if the ruby GC thread is running we can't take this lock because a resize can cause GC.
            bool lock_taken = in_background_sweep_mode() && resize_lock_rdlock();
            {
                struct concurrent_set *current_set = (struct concurrent_set *)rbimpl_atomic_value_load((VALUE *)set_data_ptr, RBIMPL_ATOMIC_ACQUIRE);
                if (current_set != set) {
                    set = current_set;
                    // retry - resize happened
                }
                else {
                    result = rb_concurrent_set_delete_by_identity_locked(set, key);
                    if (lock_taken) resize_lock_rdunlock();
                    break;
                }
            }
            if (lock_taken) resize_lock_rdunlock();
        }
    }
    else {
        struct concurrent_set *set = (struct concurrent_set *)rbimpl_atomic_value_load((VALUE *)set_data_ptr, RBIMPL_ATOMIC_ACQUIRE);
        result = rb_concurrent_set_delete_by_identity_locked(set, key);
    }
    return result;
}

static void
rb_concurrent_set_foreach_with_replace_locked(VALUE set_obj, int (*callback)(VALUE *key, void *data), void *data)
{
    ASSERT_vm_locking_with_barrier();

    struct concurrent_set *set = RTYPEDDATA_GET_DATA(set_obj);

    for (unsigned int i = 0; i < set->capacity; i++) {
        struct concurrent_set_entry *entry = &set->entries[i];
        VALUE raw_key = entry->key;
        VALUE hash = entry->hash;
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;
        VALUE key = raw_key & CONCURRENT_SET_KEY_MASK;

        if (hash == CONCURRENT_SET_HASH_DELETED && key != CONCURRENT_SET_EMPTY) {
            entry->key = CONCURRENT_SET_EMPTY | (continuation ? CONCURRENT_SET_CONTINUATION_BIT : 0);
            key = CONCURRENT_SET_EMPTY;
        }

        switch (key) {
          case CONCURRENT_SET_EMPTY:
          case CONCURRENT_SET_DELETED:
            continue;
          case CONCURRENT_SET_MOVED:
            rb_bug("rb_concurrent_set_foreach_with_replace: moved entry");
            break;
          default: {
            VALUE cb_key = key;
            int ret = callback(&cb_key, data);
            switch (ret) {
              case ST_STOP:
                return;
              case ST_DELETE:
                concurrent_set_delete_entry_locked(set, entry);
                break;
              case ST_CONTINUE:
                if (cb_key != key) {
                    // Key was replaced by callback
                    entry->key = cb_key | (continuation ? CONCURRENT_SET_CONTINUATION_BIT : 0);
                }
                break;
              case ST_REPLACE:
                rb_bug("unexpected concurrent_set callback return value: ST_REPLACE");
            }
            break;
          }
        }
    }
}

void
rb_concurrent_set_foreach_with_replace(VALUE set_obj, VALUE *set_obj_ptr, int (*callback)(VALUE *key, void *data), void *data)
{
    RB_VM_LOCKING() {
        resize_lock_wrlock(true); // don't allow concurrent deletes from sweep thread during this time
        {
            rb_concurrent_set_foreach_with_replace_locked(set_obj, callback, data);
        }
        resize_lock_wrunlock();
    }
}
