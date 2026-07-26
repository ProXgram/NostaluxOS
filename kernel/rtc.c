#include "rtc.h"

#include "io.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71
#define RTC_UIP      0x80

static uint8_t rtc_read_register(uint8_t reg) {
    outb(CMOS_ADDRESS, (uint8_t)(0x80u | reg)); /* Keep NMI disabled while sampling. */
    return inb(CMOS_DATA);
}

static bool rtc_wait_ready(void) {
    for (uint32_t timeout = 0; timeout < 100000u; timeout++) {
        if ((rtc_read_register(0x0A) & RTC_UIP) == 0) return true;
    }
    return false;
}

static uint8_t bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0Fu) + ((value >> 4) * 10u));
}

bool rtc_read_time(struct rtc_time* out) {
    if (out == 0) return false;

    for (int attempt = 0; attempt < 5; attempt++) {
        if (!rtc_wait_ready()) break;
        uint8_t second = rtc_read_register(0x00);
        uint8_t minute = rtc_read_register(0x02);
        uint8_t hour = rtc_read_register(0x04);
        uint8_t status_b = rtc_read_register(0x0B);

        if (!rtc_wait_ready()) break;
        uint8_t second_check = rtc_read_register(0x00);
        uint8_t minute_check = rtc_read_register(0x02);
        uint8_t hour_check = rtc_read_register(0x04);
        if (second != second_check || minute != minute_check || hour != hour_check) {
            continue;
        }

        bool pm = (hour & 0x80u) != 0;
        hour &= 0x7Fu;
        if ((status_b & 0x04u) == 0) {
            second = bcd_to_binary(second);
            minute = bcd_to_binary(minute);
            hour = bcd_to_binary(hour);
        }
        if ((status_b & 0x02u) == 0) {
            hour = (uint8_t)(hour % 12u);
            if (pm) hour = (uint8_t)(hour + 12u);
        }

        if (second >= 60u || minute >= 60u || hour >= 24u) break;
        out->second = second;
        out->minute = minute;
        out->hour = hour;
        outb(CMOS_ADDRESS, 0x00); /* Re-enable NMI. */
        return true;
    }

    outb(CMOS_ADDRESS, 0x00);
    return false;
}

bool rtc_format_time(char* buffer, uint32_t capacity) {
    if (buffer == 0 || capacity < 9u) return false;
    struct rtc_time time;
    if (!rtc_read_time(&time)) {
        buffer[0] = '\0';
        return false;
    }

    buffer[0] = (char)('0' + time.hour / 10u);
    buffer[1] = (char)('0' + time.hour % 10u);
    buffer[2] = ':';
    buffer[3] = (char)('0' + time.minute / 10u);
    buffer[4] = (char)('0' + time.minute % 10u);
    buffer[5] = ':';
    buffer[6] = (char)('0' + time.second / 10u);
    buffer[7] = (char)('0' + time.second % 10u);
    buffer[8] = '\0';
    return true;
}
