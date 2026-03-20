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
    CONCURRENT_SET_MOVED = 4,
    CONCURRENT_SET_SPECIAL_VALUE_COUNT = 5,
};

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

static void
concurrent_set_mark_continuation(struct concurrent_set_entry *entry, VALUE curr_key)
{
    if (curr_key & CONCURRENT_SET_CONTINUATION_BIT) return;

    while (true) {
        VALUE new_key = curr_key | CONCURRENT_SET_CONTINUATION_BIT;
        VALUE prev_key = rbimpl_atomic_value_cas(&entry->key, curr_key, new_key, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
        if (prev_key == curr_key || (prev_key & CONCURRENT_SET_CONTINUATION_BIT)) return;
        curr_key = prev_key;
    }
}

static VALUE
concurrent_set_hash(const struct concurrent_set *set, VALUE key)
{
    VALUE hash = set->funcs->hash(key);
    if (hash == 0) hash = ~(VALUE)0;
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
    .flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_EMBEDDABLE | RUBY_TYPED_CONCURRENT_FREE_SAFE
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
concurrent_set_try_resize_locked(VALUE old_set_obj, VALUE *set_obj_ptr)
{
    // Check if another thread has already resized.
    if (rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE) != old_set_obj) {
        return;
    }

    struct concurrent_set *old_set = RTYPEDDATA_GET_DATA(old_set_obj);

    // This may overcount by up to the number of threads concurrently attempting to insert
    // GC may also happen between now and the set being rebuilt
    int expected_size = rbimpl_atomic_load(&old_set->size, RBIMPL_ATOMIC_RELAXED) - rbimpl_atomic_load(&old_set->deleted_entries, RBIMPL_ATOMIC_RELAXED);

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
        VALUE raw_key = rbimpl_atomic_value_exchange(&old_entry->key, CONCURRENT_SET_MOVED, RBIMPL_ATOMIC_ACQUIRE);
        VALUE key = raw_key & CONCURRENT_SET_KEY_MASK;
        RUBY_ASSERT(key != CONCURRENT_SET_MOVED);

        if (key < CONCURRENT_SET_SPECIAL_VALUE_COUNT) continue;
        if (!RB_SPECIAL_CONST_P(key) && rb_objspace_garbage_object_p(key)) continue;

        VALUE hash = rbimpl_atomic_value_load(&old_entry->hash, RBIMPL_ATOMIC_RELAXED);
        RUBY_ASSERT(hash != 0);
        RUBY_ASSERT(hash == concurrent_set_hash(old_set, key));

        // Insert key into new_set.
        struct concurrent_set_probe probe;
        int idx = concurrent_set_probe_start(&probe, new_set, hash);

        while (true) {
            struct concurrent_set_entry *entry = &new_set->entries[idx];

            if ((entry->key & CONCURRENT_SET_KEY_MASK) == CONCURRENT_SET_EMPTY) {
                new_set->size++;
                RUBY_ASSERT(new_set->size <= new_set->capacity / 2);

                entry->key = key;
                entry->hash = hash;
                break;
            }

            RUBY_ASSERT((entry->key & CONCURRENT_SET_KEY_MASK) >= CONCURRENT_SET_SPECIAL_VALUE_COUNT);
            entry->key |= CONCURRENT_SET_CONTINUATION_BIT;
            idx = concurrent_set_probe_next(&probe);
        }
    }

    rbimpl_atomic_value_store(set_obj_ptr, new_set_obj, RBIMPL_ATOMIC_RELEASE);

    RB_GC_GUARD(old_set_obj);
}

// FIXME: cross-platform initializer
static rb_nativethread_lock_t resize_lock = PTHREAD_MUTEX_INITIALIZER;

static void
concurrent_set_try_resize(VALUE old_set_obj, VALUE *set_obj_ptr)
{
    rb_native_mutex_lock(&resize_lock);
    {
        concurrent_set_try_resize_locked(old_set_obj, set_obj_ptr);
    }
    rb_native_mutex_unlock(&resize_lock);
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
        VALUE raw_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_ACQUIRE);
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;
        VALUE curr_key = raw_key & CONCURRENT_SET_KEY_MASK;

        if (curr_key == CONCURRENT_SET_EMPTY) {
            if (!continuation) return 0;

            idx = concurrent_set_probe_next(&probe);
            continue;
        }

        if (curr_key == CONCURRENT_SET_DELETED) {
            idx = concurrent_set_probe_next(&probe);
            continue;
        }

        if (curr_key == CONCURRENT_SET_MOVED) {
            // Wait for resize to complete
            rb_native_mutex_lock(&resize_lock);
            rb_native_mutex_unlock(&resize_lock);
            goto retry;
        }

        VALUE curr_hash = rbimpl_atomic_value_load(&entry->hash, RBIMPL_ATOMIC_ACQUIRE);

        if (curr_hash != hash) {
            if (!continuation) {
                return 0;
            }
            idx = concurrent_set_probe_next(&probe);
            continue;
        }

        // same hash, could still be different key

        if (UNLIKELY(!RB_SPECIAL_CONST_P(curr_key) && rb_objspace_garbage_object_p(curr_key))) {
            // This is a weakref set, so after marking but before sweeping is complete we may find a matching garbage object.
            // Skip it and let the GC pass clean it up
            if (!continuation) return 0;

            idx = concurrent_set_probe_next(&probe);
            continue;
        }

        if (set->funcs->cmp(key, curr_key)) {
            // We've found a match.
            RB_GC_GUARD(set_obj);
            return curr_key;
        }

        if (!continuation) {
            return 0;
        }

        idx = concurrent_set_probe_next(&probe);
    }
}

VALUE
rb_concurrent_set_find_or_insert(VALUE *set_obj_ptr, VALUE key, void *data)
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
    key = set->funcs->create(key, data);
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
        VALUE raw_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_ACQUIRE);
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;
        VALUE curr_key = raw_key & CONCURRENT_SET_KEY_MASK;

        if (curr_key == CONCURRENT_SET_MOVED) {
            // Wait for resize to complete
            rb_native_mutex_lock(&resize_lock);
            rb_native_mutex_unlock(&resize_lock);
            goto retry;
        }

        // try an insert
        if (curr_key == CONCURRENT_SET_EMPTY) {
            rb_atomic_t prev_size = rbimpl_atomic_fetch_add(&set->size, 1, RBIMPL_ATOMIC_RELAXED);

            // Load_factor reached at 75% full. ex: prev_size: 32, capacity: 64, load_factor: 50%.
            bool load_factor_reached = (uint64_t)(prev_size * 4) >= (uint64_t)(set->capacity * 3);

            if (UNLIKELY(load_factor_reached)) {
                concurrent_set_try_resize(set_obj, set_obj_ptr);
                goto retry;
            }

            // Claim the slot by setting the key.
            VALUE new_key = key | (raw_key & CONCURRENT_SET_CONTINUATION_BIT);
            VALUE prev_key = rbimpl_atomic_value_cas(&entry->key, raw_key, new_key, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
            if (prev_key == raw_key) {
                // Slot claimed. Now set the hash.
                rbimpl_atomic_value_store(&entry->hash, hash, RBIMPL_ATOMIC_RELEASE);
                RUBY_ASSERT(rb_concurrent_set_find(set_obj_ptr, key) == key);
                RB_GC_GUARD(set_obj);
                return key;
            }
            else {
                // Entry was not inserted.
                rbimpl_atomic_sub(&set->size, 1, RBIMPL_ATOMIC_RELAXED);

                // Another thread won the race, try again at the same location.
                continue;
            }
        }

        if (curr_key == CONCURRENT_SET_DELETED) {
            goto probe_next;
        }

        // curr_key is a real key
        VALUE curr_hash = rbimpl_atomic_value_load(&entry->hash, RBIMPL_ATOMIC_ACQUIRE);
        if (curr_hash != hash) {
            goto probe_next;
        }

        if (UNLIKELY(!RB_SPECIAL_CONST_P(curr_key) && rb_objspace_garbage_object_p(curr_key))) {
            if (continuation) {
                goto probe_next;
            }
            rbimpl_atomic_value_cas(&entry->hash, curr_hash, CONCURRENT_SET_EMPTY, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
            rbimpl_atomic_value_cas(&entry->key, raw_key, CONCURRENT_SET_EMPTY, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
            continue;
        }

        if (set->funcs->cmp(key, curr_key)) {
            // We've found a live match.
            RB_GC_GUARD(set_obj);

            // We created key using set->funcs->create, but we didn't end
            // up inserting it into the set. Free it here to prevent memory
            // leaks.
            if (set->funcs->free) set->funcs->free(key);

            return curr_key;
        }

      probe_next:
        concurrent_set_mark_continuation(entry, raw_key);
        idx = concurrent_set_probe_next(&probe);
    }
}

static void
concurrent_set_delete_entry_locked(struct concurrent_set *set, struct concurrent_set_entry *entry)
{
    ASSERT_vm_locking_with_barrier();

    if (entry->key & CONCURRENT_SET_CONTINUATION_BIT) {
        entry->key = CONCURRENT_SET_DELETED | CONCURRENT_SET_CONTINUATION_BIT;
        set->deleted_entries++;
    }
    else {
        entry->hash = CONCURRENT_SET_EMPTY;
        entry->key = CONCURRENT_SET_EMPTY;
        set->size--;
    }
}


static VALUE
rb_concurrent_set_delete_by_identity_locked(VALUE set_obj, VALUE key)
{
    struct concurrent_set *set = RTYPEDDATA_GET_DATA(set_obj);

    VALUE hash = concurrent_set_hash(set, key);

    struct concurrent_set_probe probe;
    int idx = concurrent_set_probe_start(&probe, set, hash);

    while (true) {
        struct concurrent_set_entry *entry = &set->entries[idx];
        VALUE raw_key = rbimpl_atomic_value_load(&entry->key, RBIMPL_ATOMIC_ACQUIRE);
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;
        VALUE curr_key = raw_key & CONCURRENT_SET_KEY_MASK;

        switch (curr_key) {
          case CONCURRENT_SET_EMPTY:
            if (!continuation) return 0;
            break;
          case CONCURRENT_SET_DELETED:
            break;
          case CONCURRENT_SET_MOVED:
            rb_bug("rb_concurrent_set_delete_by_identity: moved entry");
            break;
          default:
            if (key == curr_key) {
                RUBY_ASSERT(entry->hash == hash);

                if (!continuation) {
                    // Clear hash first so the slot is clean when key becomes EMPTY.
                    rbimpl_atomic_value_cas(&entry->hash, hash, CONCURRENT_SET_EMPTY, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
                }

                VALUE new_key;
                if (continuation) {
                    new_key = CONCURRENT_SET_DELETED | CONCURRENT_SET_CONTINUATION_BIT;
                }
                else {
                    new_key = CONCURRENT_SET_EMPTY;
                }

                VALUE prev_key = rbimpl_atomic_value_cas(&entry->key, raw_key, new_key, RBIMPL_ATOMIC_RELEASE, RBIMPL_ATOMIC_RELAXED);
                if (prev_key == raw_key) {
                    if (continuation) {
                        rbimpl_atomic_add(&set->deleted_entries, 1, RBIMPL_ATOMIC_RELAXED);
                    }
                    else {
                        rbimpl_atomic_sub(&set->size, 1, RBIMPL_ATOMIC_RELAXED);
                    }
                    return curr_key;
                }

                // CAS failed - we can assume key has valid hash now. Retry.
                continue;
            }
            break;
        }

        idx = concurrent_set_probe_next(&probe);
    }
}

VALUE
rb_concurrent_set_delete_by_identity(VALUE set_obj, VALUE *set_obj_ptr, VALUE key)
{
    VALUE result;
    while (1) {
        rb_native_mutex_lock(&resize_lock);
        {
            VALUE new_set_obj;
            if ((new_set_obj = rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE)) != set_obj) {
                set_obj = new_set_obj;
                // retry
            }
            else {
                result = rb_concurrent_set_delete_by_identity_locked(set_obj, key);
                rb_native_mutex_unlock(&resize_lock);
                break;
            }
        }
        rb_native_mutex_unlock(&resize_lock);
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
        bool continuation = raw_key & CONCURRENT_SET_CONTINUATION_BIT;
        VALUE key = raw_key & CONCURRENT_SET_KEY_MASK;

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
              case ST_REPLACE:
                if (cb_key != key) {
                    // Key was replaced by callback
                    entry->key = cb_key | (continuation ? CONCURRENT_SET_CONTINUATION_BIT : 0);
                }
                break;
            }
            break;
          }
        }
    }
}

void
rb_concurrent_set_foreach_with_replace(VALUE set_obj, VALUE *set_obj_ptr, int (*callback)(VALUE *key, void *data), void *data)
{
    while (1) {
        rb_native_mutex_lock(&resize_lock); // TODO: make it take rwlock_wrlock
        {
            VALUE new_set_obj;
            if ((new_set_obj = rbimpl_atomic_value_load(set_obj_ptr, RBIMPL_ATOMIC_ACQUIRE)) != set_obj) {
                set_obj = new_set_obj;
                // retry
            }
            else {
                rb_concurrent_set_foreach_with_replace_locked(set_obj, callback, data);
                rb_native_mutex_unlock(&resize_lock);
                break;
            }
        }
        rb_native_mutex_unlock(&resize_lock);
    }
}
