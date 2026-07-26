#ifndef KSTRING_H
#define KSTRING_H

#include <stdbool.h>
#include <stddef.h>

int kstrcmp(const char* a, const char* b);
int kstrncmp(const char* a, const char* b, size_t n);
size_t kstrlen(const char* str);
const char* kskip_spaces(const char* str);
bool kparse_uint(const char** str, unsigned int* out_value);

#endif /* KSTRING_H */
