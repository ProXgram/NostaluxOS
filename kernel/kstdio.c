#include "kstdio.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "terminal.h"
#include "kstring.h"

static void print_uint(unsigned long long value, unsigned int base,
                       unsigned int min_width, char pad) {
    char buffer[32];
    unsigned int pos = 0;
    const char* digits = "0123456789abcdef";

    if (value == 0) {
        buffer[pos++] = '0';
    } else {
        while (value > 0 && pos < sizeof(buffer)) {
            buffer[pos++] = digits[value % base];
            value /= base;
        }
    }

    while (pos < min_width) {
        terminal_write_char(pad);
        min_width--;
    }
    while (pos > 0) {
        terminal_write_char(buffer[--pos]);
    }
}

static void print_int(long long value, unsigned int min_width, char pad) {
    unsigned long long magnitude;
    if (value < 0) {
        terminal_write_char('-');
        if (min_width > 0) min_width--;
        magnitude = 0ull - (unsigned long long)value;
    } else {
        magnitude = (unsigned long long)value;
    }
    print_uint(magnitude, 10, min_width, pad);
}

void kvprintf(const char* format, va_list args) {
    terminal_begin_batch();

    while (*format != '\0') {
        if (*format != '%') {
            terminal_write_char(*format);
            format++;
            continue;
        }

        format++; // Skip '%'
        if (*format == '\0') break;

        char pad = ' ';
        unsigned int width = 0;
        if (*format == '0') {
            pad = '0';
            format++;
        }
        while (*format >= '0' && *format <= '9') {
            unsigned int digit = (unsigned int)(*format - '0');
            if (width <= 1000u) width = width * 10u + digit;
            format++;
        }

        enum { LEN_DEFAULT, LEN_LONG, LEN_LONG_LONG, LEN_SIZE } length = LEN_DEFAULT;
        if (*format == 'z') {
            length = LEN_SIZE;
            format++;
        } else if (*format == 'l') {
            length = LEN_LONG;
            format++;
            if (*format == 'l') {
                length = LEN_LONG_LONG;
                format++;
            }
        }
        if (*format == '\0') break;

        switch (*format) {
            case 's': {
                const char* s = va_arg(args, const char*);
                if (s == NULL) s = "(null)";
                terminal_writestring(s);
                break;
            }
            case 'c': {
                // char is promoted to int in varargs
                char c = (char)va_arg(args, int);
                terminal_write_char(c);
                break;
            }
            case 'd': {
                long long value;
                if (length == LEN_LONG_LONG) value = va_arg(args, long long);
                else if (length == LEN_LONG) value = (long long)va_arg(args, long);
                else if (length == LEN_SIZE) value = (long long)va_arg(args, intptr_t);
                else value = (long long)va_arg(args, int);
                print_int(value, width, pad);
                break;
            }
            case 'u': {
                unsigned long long value;
                if (length == LEN_LONG_LONG) value = va_arg(args, unsigned long long);
                else if (length == LEN_LONG) value = (unsigned long long)va_arg(args, unsigned long);
                else if (length == LEN_SIZE) value = (unsigned long long)va_arg(args, size_t);
                else value = (unsigned long long)va_arg(args, unsigned int);
                print_uint(value, 10, width, pad);
                break;
            }
            case 'x': {
                unsigned long long value;
                if (length == LEN_LONG_LONG) value = va_arg(args, unsigned long long);
                else if (length == LEN_LONG) value = (unsigned long long)va_arg(args, unsigned long);
                else if (length == LEN_SIZE) value = (unsigned long long)va_arg(args, size_t);
                else value = (unsigned long long)va_arg(args, unsigned int);
                terminal_writestring("0x");
                print_uint(value, 16, width, pad);
                break;
            }
            case 'p': {
                void* ptr = va_arg(args, void*);
                terminal_writestring("0x");
                print_uint((uintptr_t)ptr, 16, 16, '0');
                break;
            }
            case '%': {
                terminal_write_char('%');
                break;
            }
            default: {
                terminal_write_char('%');
                terminal_write_char(*format);
                break;
            }
        }
        format++;
    }

    terminal_end_batch();
}

void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    kvprintf(format, args);
    va_end(args);
}
