#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

struct rtc_time {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

bool rtc_read_time(struct rtc_time* out);
bool rtc_format_time(char* buffer, uint32_t capacity);

#endif /* RTC_H */
