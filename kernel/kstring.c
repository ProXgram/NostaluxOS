#include <limits.h>

#include "kstring.h"

int kstrcmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)(*a);
        unsigned char cb = (unsigned char)(*b);
        if (ca != cb) {
            return (int)(ca - cb);
        }
        a++;
        b++;
    }

    return (int)((unsigned char)(*a) - (unsigned char)(*b));
}

int kstrncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)(a[i]);
        unsigned char cb = (unsigned char)(b[i]);

        if (ca != cb) {
            return (int)(ca - cb);
        }

        if (ca == '\0' || cb == '\0') {
            return (int)(ca - cb);
        }
    }

    return 0;
}

size_t kstrlen(const char* str) {
    size_t length = 0;
    while (str[length] != '\0') {
        length++;
    }
    return length;
}

const char* kskip_spaces(const char* str) {
    while (*str == ' ' || *str == '\t') {
        str++;
    }
    return str;
}

bool kparse_uint(const char** str, unsigned int* out_value) {
    const char* cursor = kskip_spaces(*str);
    unsigned int value = 0;
    bool has_digits = false;

    while (*cursor >= '0' && *cursor <= '9') {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (value > (UINT_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        cursor++;
        has_digits = true;
    }

    if (!has_digits) {
        return false;
    }

    *out_value = value;
    *str = cursor;
    return true;
}
