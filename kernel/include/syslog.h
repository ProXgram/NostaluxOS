#ifndef SYSLOG_H
#define SYSLOG_H

#include <stddef.h>

void syslog_init(void);
void syslog_write(const char* message);
size_t syslog_length(void);
const char* syslog_entry(size_t index);
size_t syslog_copy_text(char* buffer, size_t capacity);

#endif /* SYSLOG_H */
