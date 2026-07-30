#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_HOSTNAME_CAPACITY 254u
#define NETWORK_URL_CAPACITY      512u
#define NETWORK_HTTP_REDIRECT_LIMIT 5u
#define NETWORK_HTTP_BODY_MAX     8191u
#define NETWORK_HTTP_TIMEOUT_MAX_MS 60000u

enum network_result {
    NETWORK_OK = 0,
    NETWORK_NOT_READY,
    NETWORK_LINK_DOWN,
    NETWORK_BUSY,
    NETWORK_INVALID_ARGUMENT,
    NETWORK_TIMEOUT,
    NETWORK_DNS_FAILED,
    NETWORK_CONNECTION_FAILED,
    NETWORK_PROTOCOL_ERROR,
    NETWORK_RESPONSE_TOO_LARGE,
    NETWORK_UNSUPPORTED,
    NETWORK_IO_ERROR,
    NETWORK_CANCELED,
};

struct network_status {
    bool device_present;
    bool device_ready;
    bool link_up;
    bool configured;
    bool dhcp_in_progress;
    uint8_t mac[6];
    uint32_t ipv4_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint64_t received_packets;
    uint64_t transmitted_packets;
    uint64_t dropped_packets;
};

enum network_http_request_state {
    NETWORK_HTTP_REQUEST_WAITING = 0,
    NETWORK_HTTP_REQUEST_CONFIGURING,
    NETWORK_HTTP_REQUEST_RESOLVING,
    NETWORK_HTTP_REQUEST_CONNECTING,
    NETWORK_HTTP_REQUEST_SENDING,
    NETWORK_HTTP_REQUEST_RECEIVING,
    NETWORK_HTTP_REQUEST_REDIRECTING,
    NETWORK_HTTP_REQUEST_COMPLETE,
    NETWORK_HTTP_REQUEST_FAILED,
    NETWORK_HTTP_REQUEST_CANCELED,
};

struct network_http_request_status {
    enum network_http_request_state state;
    enum network_result result;
    unsigned int http_status;
    unsigned int redirect_count;
    size_t received_bytes;
    size_t total_bytes;
    bool total_known;
};

/*
 * Initializes the RTL8139-backed service and starts DHCP. The service is
 * intentionally polled from foreground kernel loops; no packet parser runs
 * in interrupt context.
 */
void network_init(void);
void network_poll(void);
enum network_result network_renew_dhcp(uint32_t timeout_milliseconds);
void network_get_status(struct network_status* status);

enum network_result network_resolve_ipv4(
    const char* hostname, uint32_t timeout_milliseconds,
    uint32_t* out_address);
enum network_result network_ping(
    const char* host, uint32_t timeout_milliseconds,
    uint32_t* out_round_trip_milliseconds);
enum network_result network_http_get(
    const char* url, char* body, size_t body_capacity,
    size_t* out_body_length, unsigned int* out_status_code,
    uint32_t timeout_milliseconds);

/*
 * Asynchronous HTTP requests are kernel-owned and bounded. Only one request
 * may use the single-connection v1 TCP engine at a time. Callers retain an
 * opaque handle, poll status, read a completed body, then close the handle.
 */
enum network_result network_http_request_start(
    const char* url,
    size_t body_capacity,
    bool follow_redirects,
    uint32_t timeout_milliseconds,
    uint64_t* out_handle);
bool network_http_request_get_status(
    uint64_t handle,
    struct network_http_request_status* out_status);
enum network_result network_http_request_read(
    uint64_t handle,
    void* buffer,
    size_t capacity,
    size_t offset,
    size_t* out_length);
enum network_result network_http_request_cancel(uint64_t handle);
enum network_result network_http_request_close(uint64_t handle);

const char* network_result_text(enum network_result result);
bool network_parse_ipv4(const char* text, uint32_t* out_address);
void network_format_ipv4(uint32_t address, char* buffer, size_t capacity);
void network_format_mac(const uint8_t mac[6], char* buffer, size_t capacity);

#endif /* NETWORK_H */
