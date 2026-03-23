#ifndef RUBY_RACTOR_SAFE_TABLE_H
#define RUBY_RACTOR_SAFE_TABLE_H

#include "ruby/atomic.h"
#include "ruby/ruby.h"

struct rb_concurrent_set_funcs {
    VALUE (*hash)(VALUE key);
    bool (*cmp)(VALUE a, VALUE b);
    VALUE (*create)(VALUE key, void *data);
    void (*free)(VALUE key);
};

VALUE rb_concurrent_set_new(const struct rb_concurrent_set_funcs *funcs, int capacity);
void *rb_concurrent_set_get_data(VALUE set_obj);
rb_atomic_t rb_concurrent_set_size(VALUE set_obj);
VALUE rb_concurrent_set_find(VALUE *set_obj_ptr, VALUE key);
VALUE rb_concurrent_set_find_or_insert(VALUE *set_obj_ptr, VALUE key, void *data, void **set_data_ptr);
VALUE rb_concurrent_set_delete_by_identity(VALUE *set_obj_ptr, void **set_data_ptr, VALUE key);
void rb_concurrent_set_foreach_with_replace(VALUE set_obj, VALUE *set_obj_ptr, int (*callback)(VALUE *key, void *data), void *data);

#endif
