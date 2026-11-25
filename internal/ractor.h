#ifndef INTERNAL_RACTOR_H                                /*-*-C-*-vi:se ft=c:*/
#define INTERNAL_RACTOR_H

void rb_ractor_ensure_main_ractor(const char *msg);
bool rb_class_field_shareable_p(VALUE klass, VALUE val);

// heir table (ractor.c): rewrite recorded Ractor ids while marking, then prune
void rb_ractor_heirs_rehome_class(VALUE klass);
void rb_ractor_heirs_rehome_id(uint64_t *id);
void rb_ractor_heirs_prune(void);

RUBY_SYMBOL_EXPORT_BEGIN
RUBY_SYMBOL_EXPORT_END

#endif /* INTERNAL_RACTOR_H */
