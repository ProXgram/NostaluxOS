#ifndef KSTDIO_H
#define KSTDIO_H

#include <stdarg.h>

/*
 * Standard formatted printing.
 * Supported formats:
 *   %s - String
 *   %c - Character
 *   %d - Signed decimal (int)
 *   %u - Unsigned decimal (unsigned int)
 *   %x - Hexadecimal (lowercase, 32/64 bit friendly)
 *   %p - Pointer address
 *   %% - Literal %
 */
void kprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/*
 * Variant taking a va_list, useful for wrapping kprintf
 * in other logging functions.
 */
void kvprintf(const char* format, va_list args) __attribute__((format(printf, 1, 0)));

#endif /* KSTDIO_H */
