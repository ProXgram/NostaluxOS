#include "network.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "http_client.h"
#include "net.h"
#include "rtl8139.h"
#include "syslog.h"
#include "tcp_client.h"
#include "timer.h"

#define NETWORK_DHCP_TIMEOUT_MS 5000u
#define NETWORK_HTTP_REQUEST_CAPACITY 1024u
#define NETWORK_TCP_RESPONSE_ALLOWANCE \
    (HTTP_CLIENT_MAX_HEADER_BYTES + HTTP_CLIENT_MAX_TRAILER_BYTES + \
     HTTP_CLIENT_MAX_CHUNK_FRAMING_BYTES)
#define NETWORK_TCP_EPHEMERAL_FIRST 49152u
#define NETWORK_TCP_EPHEMERAL_LAST 65535u
#define NETWORK_HTTP_VISITED_CAPACITY \
    (NETWORK_HTTP_REDIRECT_LIMIT + 1u)

struct network_url {
    char hostname[NETWORK_HOSTNAME_CAPACITY];
    char host_header[NETWORK_HOSTNAME_CAPACITY + 8u];
    char path[NETWORK_URL_CAPACITY];
    uint16_t port;
};

enum network_http_receive_phase {
    NETWORK_HTTP_WAITING_FOR_PEER = 0,
    NETWORK_HTTP_CLOSING_CONNECTION,
};

struct network_http_request {
    bool allocated;
    bool follow_redirects;
    bool dns_started;
    bool tcp_initialized;
    uint64_t handle;
    uint64_t started_at_ms;
    uint32_t timeout_ms;
    enum network_http_request_state state;
    enum network_result result;
    enum network_http_receive_phase receive_phase;
    size_t body_capacity;
    size_t response_capacity;
    uint8_t* response_bytes;
    size_t body_offset;
    size_t body_length;
    size_t request_length;
    size_t request_sent;
    size_t request_chunk_length;
    uint32_t remote_ip;
    unsigned int http_status;
    unsigned int redirect_count;
    unsigned int visited_count;
    char url[NETWORK_URL_CAPACITY];
    char visited[NETWORK_HTTP_VISITED_CAPACITY][NETWORK_URL_CAPACITY];
    struct network_url parsed;
    uint8_t request_bytes[NETWORK_HTTP_REQUEST_CAPACITY];
    struct tcp_client tcp;
};

static struct net_stack g_stack;
static bool g_stack_ready;
static bool g_operation_busy;
static bool g_dhcp_bound_logged;
static bool g_irq_delivery_logged;
static struct tcp_client* g_active_tcp_client;
static struct network_http_request g_http_request;
static uint64_t g_next_http_handle = 1u;
static uint32_t g_nonce;
static uint16_t g_next_ephemeral_port = NETWORK_TCP_EPHEMERAL_FIRST;

static void network_http_request_drive(void);

static void network_zero(void* destination, size_t count) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t index = 0; index < count; index++) {
        bytes[index] = 0;
    }
}

static void network_copy(
    void* destination, const void* source, size_t count) {
    uint8_t* output = (uint8_t*)destination;
    const uint8_t* input = (const uint8_t*)source;
    for (size_t index = 0; index < count; index++) {
        output[index] = input[index];
    }
}

static size_t network_length(const char* text) {
    size_t length = 0;
    if (text == NULL) return 0;
    while (text[length] != '\0') length++;
    return length;
}

static char network_ascii_lower(char value) {
    return value >= 'A' && value <= 'Z'
         ? (char)(value + ('a' - 'A')) : value;
}

static bool network_starts_with_ci(
    const char* text, const char* prefix) {
    if (text == NULL || prefix == NULL) return false;
    for (size_t index = 0; prefix[index] != '\0'; index++) {
        if (network_ascii_lower(text[index]) !=
            network_ascii_lower(prefix[index])) {
            return false;
        }
    }
    return true;
}

static bool network_text_equal(
    const char* left, const char* right) {
    if (left == NULL || right == NULL) return false;
    size_t index = 0;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return false;
        index++;
    }
    return left[index] == right[index];
}

static bool network_copy_text(
    char* destination, size_t capacity, const char* source) {
    if (destination == NULL || capacity == 0 || source == NULL) {
        return false;
    }
    size_t length = network_length(source);
    if (length >= capacity) return false;
    network_copy(destination, source, length + 1u);
    return true;
}

static bool network_append_text(
    char* destination, size_t capacity, const char* source) {
    if (destination == NULL || source == NULL || capacity == 0) {
        return false;
    }
    const size_t existing = network_length(destination);
    const size_t added = network_length(source);
    if (existing >= capacity || added >= capacity - existing) {
        return false;
    }
    network_copy(destination + existing, source, added + 1u);
    return true;
}

static bool network_link_send(
    void* context, const uint8_t* frame, size_t frame_size) {
    (void)context;
    for (unsigned int attempt = 0; attempt < 4u; attempt++) {
        enum rtl8139_tx_result result =
            rtl8139_send(frame, frame_size);
        if (result == RTL8139_TX_QUEUED) return true;
        if (result != RTL8139_TX_BUSY) return false;
        rtl8139_poll();
    }
    return false;
}

static size_t network_link_poll(
    void* context, uint8_t* frame, size_t frame_capacity) {
    (void)context;
    size_t frame_length = 0;
    rtl8139_poll();
    enum rtl8139_rx_result result = rtl8139_receive(
        frame, frame_capacity, &frame_length);
    return result == RTL8139_RX_RECEIVED ? frame_length : 0;
}

static uint64_t network_link_now(void* context) {
    (void)context;
    return timer_get_milliseconds();
}

static uint32_t network_next_nonce(void) {
    if (g_nonce == 0) {
        struct rtl8139_status status;
        (void)rtl8139_get_status(&status);
        g_nonce =
            ((uint32_t)status.mac[2] << 24) |
            ((uint32_t)status.mac[3] << 16) |
            ((uint32_t)status.mac[4] << 8) |
            status.mac[5];
        g_nonce ^= (uint32_t)timer_get_milliseconds();
        if (g_nonce == 0) g_nonce = 0x4e4f5354u;
    }
    g_nonce += 0x9e3779b9u;
    if (g_nonce == 0) g_nonce = 1u;
    return g_nonce;
}

static uint16_t network_next_port(void) {
    const uint16_t port = g_next_ephemeral_port;
    if (g_next_ephemeral_port == NETWORK_TCP_EPHEMERAL_LAST) {
        g_next_ephemeral_port = NETWORK_TCP_EPHEMERAL_FIRST;
    } else {
        g_next_ephemeral_port++;
    }
    return port;
}

static void network_tcp_input(
    void* context, uint32_t source_ip,
    uint32_t destination_ip, const uint8_t* payload,
    size_t payload_size) {
    (void)context;
    if (g_active_tcp_client == NULL) return;
    (void)tcp_client_receive(
        g_active_tcp_client, source_ip, destination_ip,
        payload, payload_size);
}

static bool network_tcp_send_ipv4(
    void* context, uint32_t destination_ip, uint8_t protocol,
    const void* payload, size_t payload_length) {
    struct net_stack* stack = (struct net_stack*)context;
    enum net_send_result result = net_ipv4_send(
        stack, destination_ip, protocol, payload, payload_length);
    return result != NET_SEND_FAILED;
}

static uint64_t network_tcp_now(void* context) {
    return net_now_ms((const struct net_stack*)context);
}

static void network_tcp_poll(void* context) {
    (void)context;
    network_poll();
}

void network_init(void) {
    uint8_t* previous_response = g_http_request.response_bytes;
    if (previous_response != NULL) kfree(previous_response);
    network_zero(&g_stack, sizeof(g_stack));
    g_stack_ready = false;
    g_operation_busy = false;
    g_dhcp_bound_logged = false;
    g_irq_delivery_logged = false;
    g_active_tcp_client = NULL;
    network_zero(&g_http_request, sizeof(g_http_request));
    g_next_http_handle = 1u;
    g_nonce = 0;
    g_next_ephemeral_port = NETWORK_TCP_EPHEMERAL_FIRST;

    enum rtl8139_init_result driver_result = rtl8139_init();
    if (driver_result != RTL8139_INIT_READY) {
        syslog_write(rtl8139_init_result_text(driver_result));
        return;
    }

    struct rtl8139_status device;
    if (!rtl8139_get_status(&device)) {
        syslog_write("Network: RTL8139 status unavailable");
        return;
    }
    const struct net_link_ops link = {
        .send = network_link_send,
        .poll = network_link_poll,
        .now_ms = network_link_now,
        .context = NULL,
    };
    if (!net_init(&g_stack, &link, device.mac)) {
        syslog_write("Network: protocol stack initialization failed");
        return;
    }
    net_set_tcp_handler(&g_stack, network_tcp_input, NULL);
    g_stack_ready = true;
    if (net_dhcp_start(&g_stack, network_next_nonce())) {
        syslog_write("Network: DHCP discovery started");
    } else {
        syslog_write("Network: DHCP discovery could not start");
    }
}

void network_poll(void) {
    if (!g_stack_ready) {
        rtl8139_poll();
        return;
    }
    net_tick(&g_stack, timer_get_milliseconds());
    (void)net_poll(&g_stack);
    if (!g_irq_delivery_logged) {
        struct rtl8139_status device;
        if (rtl8139_get_status(&device) &&
            device.interrupt_mode &&
            device.interrupts != 0u) {
            g_irq_delivery_logged = true;
            syslog_write(
                "Network: RTL8139 IRQ delivery confirmed");
        }
    }
    if (net_dhcp_get_state(&g_stack) == NET_DHCP_BOUND) {
        if (!g_dhcp_bound_logged) {
            g_dhcp_bound_logged = true;
            syslog_write("Network: DHCP configured");
        }
    } else {
        g_dhcp_bound_logged = false;
    }
    network_http_request_drive();
}

static enum network_result network_require_configuration(
    uint32_t timeout_milliseconds) {
    if (!g_stack_ready) return NETWORK_NOT_READY;
    if (net_is_configured(&g_stack)) return NETWORK_OK;
    if (net_dhcp_get_state(&g_stack) == NET_DHCP_OFF &&
        !net_dhcp_start(&g_stack, network_next_nonce())) {
        return NETWORK_IO_ERROR;
    }

    const uint64_t started = timer_get_milliseconds();
    do {
        network_poll();
        if (net_is_configured(&g_stack)) return NETWORK_OK;
        timer_idle_wait();
    } while (timer_get_milliseconds() - started <
             timeout_milliseconds);
    return NETWORK_TIMEOUT;
}

enum network_result network_renew_dhcp(
    uint32_t timeout_milliseconds) {
    if (timeout_milliseconds == 0) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (g_operation_busy) return NETWORK_BUSY;
    g_operation_busy = true;

    enum network_result result = NETWORK_OK;
    if (!g_stack_ready) {
        result = NETWORK_NOT_READY;
    } else {
        net_dhcp_stop(&g_stack);
        net_clear_ipv4(&g_stack);
        g_dhcp_bound_logged = false;
        if (!net_dhcp_start(&g_stack, network_next_nonce())) {
            result = NETWORK_IO_ERROR;
        } else {
            result = network_require_configuration(
                timeout_milliseconds);
        }
    }
    g_operation_busy = false;
    return result;
}

void network_get_status(struct network_status* status) {
    if (status == NULL) return;
    network_zero(status, sizeof(*status));

    struct rtl8139_status device;
    if (!rtl8139_get_status(&device)) return;
    status->device_present = device.present;
    status->device_ready = device.ready;
    status->link_up = device.link_up;
    for (size_t index = 0; index < sizeof(status->mac); index++) {
        status->mac[index] = device.mac[index];
    }
    status->received_packets = device.rx_packets;
    status->transmitted_packets = device.tx_packets;
    status->dropped_packets =
        device.rx_dropped + device.rx_errors + device.tx_errors;
    if (!g_stack_ready) return;

    const struct net_ipv4_config config =
        net_get_ipv4_config(&g_stack);
    status->configured = net_is_configured(&g_stack);
    status->ipv4_address = config.address;
    status->subnet_mask = config.netmask;
    status->gateway = config.gateway;
    status->dns_server = config.dns;
    const enum net_dhcp_state dhcp =
        net_dhcp_get_state(&g_stack);
    status->dhcp_in_progress =
        dhcp == NET_DHCP_DISCOVERING ||
        dhcp == NET_DHCP_REQUESTING ||
        dhcp == NET_DHCP_RENEWING ||
        dhcp == NET_DHCP_REBINDING;
    const struct net_stats* stack_stats = net_get_stats(&g_stack);
    if (stack_stats != NULL) {
        status->dropped_packets += stack_stats->frames_dropped;
    }
}

bool network_parse_ipv4(
    const char* text, uint32_t* out_address) {
    if (text == NULL || out_address == NULL) return false;
    uint32_t address = 0;
    const char* cursor = text;
    for (unsigned int part = 0; part < 4u; part++) {
        if (*cursor < '0' || *cursor > '9') return false;
        unsigned int value = 0;
        unsigned int digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10u + (unsigned int)(*cursor - '0');
            if (value > 255u || ++digits > 3u) return false;
            cursor++;
        }
        address = (address << 8) | value;
        if (part < 3u) {
            if (*cursor != '.') return false;
            cursor++;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    *out_address = address;
    return true;
}

static enum network_result network_resolve_internal(
    const char* hostname, uint32_t timeout_milliseconds,
    uint32_t* out_address) {
    if (hostname == NULL || out_address == NULL ||
        hostname[0] == '\0' || timeout_milliseconds == 0) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (network_parse_ipv4(hostname, out_address)) {
        return NETWORK_OK;
    }
    if (network_length(hostname) > NET_DNS_NAME_MAX) {
        return NETWORK_INVALID_ARGUMENT;
    }

    enum network_result configured =
        network_require_configuration(NETWORK_DHCP_TIMEOUT_MS);
    if (configured != NETWORK_OK) return configured;
    net_dns_clear_result(&g_stack);
    if (!net_dns_query(
            &g_stack, hostname,
            (uint16_t)network_next_nonce())) {
        return NETWORK_DNS_FAILED;
    }

    const uint64_t started = timer_get_milliseconds();
    do {
        network_poll();
        struct net_dns_result dns = net_dns_get_result(&g_stack);
        if (dns.state == NET_DNS_RESOLVED) {
            *out_address = dns.address;
            net_dns_clear_result(&g_stack);
            return NETWORK_OK;
        }
        if (dns.state == NET_DNS_NOT_FOUND ||
            dns.state == NET_DNS_MALFORMED_RESPONSE ||
            dns.state == NET_DNS_SERVER_ERROR) {
            net_dns_clear_result(&g_stack);
            return NETWORK_DNS_FAILED;
        }
        if (dns.state == NET_DNS_TIMED_OUT) {
            net_dns_clear_result(&g_stack);
            return NETWORK_TIMEOUT;
        }
        timer_idle_wait();
    } while (timer_get_milliseconds() - started <
             timeout_milliseconds);
    net_dns_clear_result(&g_stack);
    return NETWORK_TIMEOUT;
}

enum network_result network_resolve_ipv4(
    const char* hostname, uint32_t timeout_milliseconds,
    uint32_t* out_address) {
    if (g_operation_busy) return NETWORK_BUSY;
    g_operation_busy = true;
    enum network_result result = network_resolve_internal(
        hostname, timeout_milliseconds, out_address);
    g_operation_busy = false;
    return result;
}

enum network_result network_ping(
    const char* host, uint32_t timeout_milliseconds,
    uint32_t* out_round_trip_milliseconds) {
    if (host == NULL || out_round_trip_milliseconds == NULL ||
        timeout_milliseconds == 0) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (g_operation_busy) return NETWORK_BUSY;
    g_operation_busy = true;

    uint32_t address = 0;
    enum network_result result = network_resolve_internal(
        host, timeout_milliseconds, &address);
    if (result != NETWORK_OK) {
        g_operation_busy = false;
        return result;
    }

    net_icmp_clear_echo_reply(&g_stack);
    const uint32_t nonce = network_next_nonce();
    const uint16_t identifier = (uint16_t)(nonce >> 16);
    const uint16_t sequence = (uint16_t)nonce;
    static const uint8_t payload[] = {
        'N', 'o', 's', 't', 'a', 'l', 'u', 'x',
    };
    if (net_icmp_echo_request(
            &g_stack, address, identifier, sequence,
            payload, sizeof(payload)) == NET_SEND_FAILED) {
        g_operation_busy = false;
        return NETWORK_IO_ERROR;
    }

    const uint64_t started = timer_get_milliseconds();
    do {
        network_poll();
        struct net_icmp_echo_reply reply =
            net_icmp_last_echo_reply(&g_stack);
        if (reply.valid &&
            reply.source_ip == address &&
            reply.identifier == identifier &&
            reply.sequence == sequence) {
            *out_round_trip_milliseconds =
                reply.round_trip_ms > UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)reply.round_trip_ms;
            net_icmp_clear_echo_reply(&g_stack);
            g_operation_busy = false;
            return NETWORK_OK;
        }
        timer_idle_wait();
    } while (timer_get_milliseconds() - started <
             timeout_milliseconds);

    net_icmp_clear_echo_reply(&g_stack);
    g_operation_busy = false;
    return NETWORK_TIMEOUT;
}

static bool network_parse_http_url(
    const char* url, struct network_url* parsed) {
    if (url == NULL || parsed == NULL ||
        !network_starts_with_ci(url, "http://") ||
        network_length(url) >= NETWORK_URL_CAPACITY) {
        return false;
    }
    network_zero(parsed, sizeof(*parsed));
    parsed->port = 80u;

    const char* authority = url + 7;
    const char* cursor = authority;
    while (*cursor != '\0' && *cursor != '/' &&
           *cursor != '?' && *cursor != '#') {
        if ((unsigned char)*cursor <= 32u ||
            *cursor == '@' || *cursor == '[' ||
            *cursor == ']') {
            return false;
        }
        cursor++;
    }
    if (cursor == authority) return false;

    const char* colon = NULL;
    for (const char* scan = authority; scan < cursor; scan++) {
        if (*scan == ':') {
            if (colon != NULL) return false;
            colon = scan;
        }
    }
    const char* host_end = colon != NULL ? colon : cursor;
    size_t host_length = (size_t)(host_end - authority);
    if (host_length == 0 ||
        host_length >= sizeof(parsed->hostname)) {
        return false;
    }
    network_copy(parsed->hostname, authority, host_length);
    parsed->hostname[host_length] = '\0';

    if (colon != NULL) {
        const char* port_cursor = colon + 1;
        unsigned int port = 0;
        if (port_cursor == cursor) return false;
        while (port_cursor < cursor) {
            if (*port_cursor < '0' || *port_cursor > '9') {
                return false;
            }
            port = port * 10u +
                   (unsigned int)(*port_cursor - '0');
            if (port > 65535u) return false;
            port_cursor++;
        }
        if (port == 0) return false;
        parsed->port = (uint16_t)port;
    }

    size_t authority_length = (size_t)(cursor - authority);
    if (authority_length >= sizeof(parsed->host_header)) return false;
    network_copy(
        parsed->host_header, authority, authority_length);
    parsed->host_header[authority_length] = '\0';

    const char* path = cursor;
    size_t write = 0;
    bool fragment_seen = false;
    if (*path == '\0' || *path == '#') {
        parsed->path[write++] = '/';
        if (*path == '#') {
            fragment_seen = true;
            path++;
        }
    } else {
        if (*path == '?') parsed->path[write++] = '/';
        while (*path != '\0') {
            unsigned char character = (unsigned char)*path++;
            if (character == '#') {
                fragment_seen = true;
                break;
            }
            if (character <= 32u ||
                character == 127u ||
                write + 1u >= sizeof(parsed->path)) {
                return false;
            }
            parsed->path[write++] = (char)character;
        }
    }
    if (fragment_seen) {
        while (*path != '\0') {
            const unsigned char character =
                (unsigned char)*path++;
            if (character <= 32u || character == 127u) {
                return false;
            }
        }
    }
    parsed->path[write] = '\0';
    return true;
}

static bool network_append_span(
    char* destination, size_t capacity,
    const char* source, size_t count) {
    if (destination == NULL || source == NULL || capacity == 0) {
        return false;
    }
    const size_t existing = network_length(destination);
    if (existing >= capacity || count >= capacity - existing) {
        return false;
    }
    network_copy(destination + existing, source, count);
    destination[existing + count] = '\0';
    return true;
}

static bool network_url_has_explicit_scheme(const char* location) {
    if (location == NULL ||
        !((location[0] >= 'A' && location[0] <= 'Z') ||
          (location[0] >= 'a' && location[0] <= 'z'))) {
        return false;
    }
    for (size_t index = 1; location[index] != '\0'; index++) {
        const char value = location[index];
        if (value == ':') return true;
        if (value == '/' || value == '?' || value == '#') {
            return false;
        }
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') ||
              value == '+' || value == '-' || value == '.')) {
            return false;
        }
    }
    return false;
}

static bool network_path_segment_equals(
    const char* segment, size_t length, const char* expected) {
    if (segment == NULL || expected == NULL) return false;
    size_t expected_length = network_length(expected);
    if (length != expected_length) return false;
    for (size_t index = 0; index < length; index++) {
        if (segment[index] != expected[index]) return false;
    }
    return true;
}

/*
 * Redirect references are merged before they get here. Remove literal dot
 * segments from the resulting absolute HTTP URL without decoding percent
 * escapes, and never allow ".." to climb above the authority root.
 */
static bool network_normalize_http_url_path(
    char url[NETWORK_URL_CAPACITY]) {
    if (url == NULL || !network_starts_with_ci(url, "http://")) {
        return false;
    }

    const char* authority = url + 7;
    const char* path = authority;
    while (*path != '\0' && *path != '/' && *path != '?' &&
           *path != '#') {
        path++;
    }
    if (path == authority || *path == '#') return false;

    const char* query = path;
    if (*path == '/') {
        while (*query != '\0' && *query != '?' && *query != '#') {
            query++;
        }
    }
    if (*query == '#') return false;

    char normalized[NETWORK_URL_CAPACITY];
    size_t write = (size_t)(path - url);
    if (write + 1u >= sizeof(normalized)) return false;
    network_copy(normalized, url, write);
    const size_t path_root = write;

    if (*path != '/') {
        normalized[write++] = '/';
    } else {
        /*
         * Treat the absolute path as slash-prefixed segments. Empty segments
         * are real URI syntax, so /a//b must keep both slashes. Literal "."
         * segments disappear; ".." removes exactly one preceding segment and
         * never climbs above the authority root.
         */
        const char* scan = path + 1u;
        for (;;) {
            const char* segment = scan;
            while (scan < query && *scan != '/') scan++;
            const size_t segment_length =
                (size_t)(scan - segment);
            const bool final_segment = scan == query;

            if (network_path_segment_equals(
                    segment, segment_length, ".")) {
                if (final_segment) {
                    if (write + 1u >= sizeof(normalized)) return false;
                    normalized[write++] = '/';
                }
            } else if (network_path_segment_equals(
                           segment, segment_length, "..")) {
                if (write > path_root) {
                    if (normalized[write - 1u] == '/') {
                        write--;
                    } else {
                        while (write > path_root &&
                               normalized[write - 1u] != '/') {
                            write--;
                        }
                        if (write > path_root) write--;
                    }
                }
                if (final_segment) {
                    if (write + 1u >= sizeof(normalized)) return false;
                    normalized[write++] = '/';
                }
            } else {
                if (write + 1u >= sizeof(normalized)) return false;
                normalized[write++] = '/';
                if (segment_length >= sizeof(normalized) - write) {
                    return false;
                }
                network_copy(
                    &normalized[write], segment, segment_length);
                write += segment_length;
            }

            if (final_segment) break;
            scan++;
        }
    }

    const size_t suffix_length = network_length(query);
    if (suffix_length >= sizeof(normalized) - write) return false;
    network_copy(&normalized[write], query, suffix_length + 1u);
    return network_copy_text(url, NETWORK_URL_CAPACITY, normalized);
}

static enum network_result network_resolve_redirect_url(
    const struct network_http_request* request,
    const char* location,
    char destination[NETWORK_URL_CAPACITY]) {
    if (request == NULL || location == NULL ||
        destination == NULL || location[0] == '\0') {
        return NETWORK_PROTOCOL_ERROR;
    }

    char reference[NETWORK_URL_CAPACITY];
    if (!network_copy_text(reference, sizeof(reference), location)) {
        return NETWORK_PROTOCOL_ERROR;
    }
    for (size_t index = 0; reference[index] != '\0'; index++) {
        if (reference[index] == '#') {
            reference[index] = '\0';
            break;
        }
    }
    if (reference[0] == '\0') return NETWORK_PROTOCOL_ERROR;

    destination[0] = '\0';
    if (network_starts_with_ci(reference, "https://")) {
        return NETWORK_UNSUPPORTED;
    }
    if (network_starts_with_ci(reference, "http://")) {
        if (!network_copy_text(
                destination, NETWORK_URL_CAPACITY, reference)) {
            return NETWORK_PROTOCOL_ERROR;
        }
    } else if (network_url_has_explicit_scheme(reference)) {
        return NETWORK_UNSUPPORTED;
    } else if (reference[0] == '/' && reference[1] == '/') {
        if (!network_copy_text(
                destination, NETWORK_URL_CAPACITY, "http:") ||
            !network_append_text(
                destination, NETWORK_URL_CAPACITY, reference)) {
            return NETWORK_PROTOCOL_ERROR;
        }
    } else {
        if (!network_copy_text(
                destination, NETWORK_URL_CAPACITY, "http://") ||
            !network_append_text(
                destination, NETWORK_URL_CAPACITY,
                request->parsed.host_header)) {
            return NETWORK_PROTOCOL_ERROR;
        }

        if (reference[0] == '/') {
            if (!network_append_text(
                    destination, NETWORK_URL_CAPACITY, reference)) {
                return NETWORK_PROTOCOL_ERROR;
            }
        } else {
            const char* current_path = request->parsed.path;
            size_t path_length = 0;
            while (current_path[path_length] != '\0' &&
                   current_path[path_length] != '?') {
                path_length++;
            }
            if (reference[0] == '?') {
                if (!network_append_span(
                        destination, NETWORK_URL_CAPACITY,
                        current_path, path_length) ||
                    !network_append_text(
                        destination, NETWORK_URL_CAPACITY,
                        reference)) {
                    return NETWORK_PROTOCOL_ERROR;
                }
            } else {
                size_t directory_length = 0;
                for (size_t index = 0; index < path_length; index++) {
                    if (current_path[index] == '/') {
                        directory_length = index + 1u;
                    }
                }
                if (!network_append_span(
                        destination, NETWORK_URL_CAPACITY,
                        current_path, directory_length) ||
                    !network_append_text(
                        destination, NETWORK_URL_CAPACITY,
                        reference)) {
                    return NETWORK_PROTOCOL_ERROR;
                }
            }
        }
    }

    struct network_url parsed;
    return network_normalize_http_url_path(destination) &&
           network_parse_http_url(destination, &parsed)
         ? NETWORK_OK : NETWORK_PROTOCOL_ERROR;
}

static enum network_result network_map_http_error(
    enum http_client_error error,
    enum tcp_client_error tcp_error) {
    if (error == HTTP_CLIENT_ERROR_NONE) return NETWORK_OK;
    if (error == HTTP_CLIENT_ERROR_INVALID_ARGUMENT ||
        error == HTTP_CLIENT_ERROR_INVALID_HOST ||
        error == HTTP_CLIENT_ERROR_INVALID_PATH ||
        error == HTTP_CLIENT_ERROR_REQUEST_TOO_LARGE) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (error == HTTP_CLIENT_ERROR_HEADER_TOO_LARGE ||
        error == HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE ||
        error == HTTP_CLIENT_ERROR_REDIRECT_LOCATION_TOO_LARGE ||
        tcp_error == TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE) {
        return NETWORK_RESPONSE_TOO_LARGE;
    }
    if (error == HTTP_CLIENT_ERROR_TCP) {
        if (tcp_error == TCP_CLIENT_ERROR_TIMEOUT ||
            tcp_error == TCP_CLIENT_ERROR_RETRIES_EXHAUSTED ||
            tcp_error == TCP_CLIENT_ERROR_CLOCK_STALLED) {
            return NETWORK_TIMEOUT;
        }
        return NETWORK_CONNECTION_FAILED;
    }
    if (error == HTTP_CLIENT_ERROR_UNSUPPORTED_TRANSFER_ENCODING) {
        return NETWORK_UNSUPPORTED;
    }
    return NETWORK_PROTOCOL_ERROR;
}

static enum network_result network_map_tcp_error(
    enum tcp_client_error error) {
    switch (error) {
        case TCP_CLIENT_ERROR_TIMEOUT:
        case TCP_CLIENT_ERROR_RETRIES_EXHAUSTED:
        case TCP_CLIENT_ERROR_CLOCK_STALLED:
            return NETWORK_TIMEOUT;
        case TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE:
            return NETWORK_RESPONSE_TOO_LARGE;
        case TCP_CLIENT_ERROR_PROTOCOL:
        case TCP_CLIENT_ERROR_CLOCK_MOVED_BACKWARD:
            return NETWORK_PROTOCOL_ERROR;
        case TCP_CLIENT_ERROR_NONE:
            return NETWORK_OK;
        case TCP_CLIENT_ERROR_INVALID_ARGUMENT:
        case TCP_CLIENT_ERROR_BUSY:
        case TCP_CLIENT_ERROR_NOT_CONNECTED:
        case TCP_CLIENT_ERROR_SEND_FAILED:
        case TCP_CLIENT_ERROR_RESET:
            return NETWORK_CONNECTION_FAILED;
    }
    return NETWORK_CONNECTION_FAILED;
}

static bool network_http_request_active(
    const struct network_http_request* request) {
    if (request == NULL || !request->allocated) return false;
    return request->state != NETWORK_HTTP_REQUEST_COMPLETE &&
           request->state != NETWORK_HTTP_REQUEST_FAILED &&
           request->state != NETWORK_HTTP_REQUEST_CANCELED;
}

static void network_http_request_finish(
    struct network_http_request* request,
    enum network_http_request_state state,
    enum network_result result) {
    if (request == NULL) return;
    if (request->dns_started) {
        (void)net_dns_cancel(&g_stack);
        request->dns_started = false;
    }
    if (g_active_tcp_client == &request->tcp) {
        g_active_tcp_client = NULL;
    }
    request->state = state;
    request->result = result;
    g_operation_busy = false;
}

static void network_http_request_fail(
    struct network_http_request* request,
    enum network_result result) {
    if (result == NETWORK_OK) result = NETWORK_PROTOCOL_ERROR;
    network_http_request_finish(
        request, NETWORK_HTTP_REQUEST_FAILED, result);
}

static bool network_http_request_timed_out(
    const struct network_http_request* request) {
    const uint64_t now = timer_get_milliseconds();
    return now < request->started_at_ms ||
           now - request->started_at_ms >= request->timeout_ms;
}

static bool network_http_url_was_visited(
    const struct network_http_request* request,
    const char* url) {
    for (unsigned int index = 0;
         index < request->visited_count; index++) {
        if (network_text_equal(request->visited[index], url)) {
            return true;
        }
    }
    return false;
}

static void network_http_begin_connect(
    struct network_http_request* request) {
    const struct net_ipv4_config ipv4 =
        net_get_ipv4_config(&g_stack);
    const struct tcp_client_io io = {
        .send_ipv4 = network_tcp_send_ipv4,
        .now_ms = network_tcp_now,
        .poll = NULL,
        .context = &g_stack,
    };
    tcp_client_init(
        &request->tcp, &io, ipv4.address, network_next_port(),
        request->response_bytes, request->response_capacity);
    uint32_t retransmit = 250u;
    if (request->timeout_ms < retransmit) {
        retransmit = request->timeout_ms;
    }
    if (!tcp_client_set_timeouts(
            &request->tcp, retransmit,
            request->timeout_ms, 4u)) {
        network_http_request_fail(
            request,
            network_map_tcp_error(
                tcp_client_get_error(&request->tcp)));
        return;
    }
    request->tcp_initialized = true;
    request->request_length = 0;
    request->request_sent = 0;
    request->request_chunk_length = 0;
    request->body_offset = 0;
    request->body_length = 0;
    g_active_tcp_client = &request->tcp;
    request->state = NETWORK_HTTP_REQUEST_CONNECTING;
    const enum tcp_client_step_result step =
        tcp_client_begin_connect(
            &request->tcp, request->remote_ip,
            request->parsed.port, network_next_nonce());
    if (step == TCP_CLIENT_STEP_FAILED) {
        network_http_request_fail(
            request,
            network_map_tcp_error(
                tcp_client_get_error(&request->tcp)));
    }
}

static void network_http_begin_resolution(
    struct network_http_request* request) {
    if (!network_parse_http_url(request->url, &request->parsed)) {
        network_http_request_fail(
            request,
            network_starts_with_ci(request->url, "https://")
                ? NETWORK_UNSUPPORTED
                : NETWORK_INVALID_ARGUMENT);
        return;
    }

    request->remote_ip = 0;
    request->dns_started = false;
    request->tcp_initialized = false;
    request->http_status = 0;
    request->state = NETWORK_HTTP_REQUEST_RESOLVING;
    if (network_parse_ipv4(
            request->parsed.hostname, &request->remote_ip)) {
        network_http_begin_connect(request);
        return;
    }

    net_dns_clear_result(&g_stack);
    if (!net_dns_query(
            &g_stack, request->parsed.hostname,
            (uint16_t)network_next_nonce())) {
        network_http_request_fail(request, NETWORK_DNS_FAILED);
        return;
    }
    request->dns_started = true;
}

static void network_http_begin_send(
    struct network_http_request* request) {
    if (request->request_length == 0) {
        enum http_client_error build = http_client_build_get(
            request->parsed.host_header,
            request->parsed.path,
            request->request_bytes,
            sizeof(request->request_bytes),
            &request->request_length);
        if (build != HTTP_CLIENT_ERROR_NONE) {
            network_http_request_fail(
                request,
                network_map_http_error(
                    build,
                    tcp_client_get_error(&request->tcp)));
            return;
        }
    }
    if (request->request_sent >= request->request_length) {
        request->receive_phase = NETWORK_HTTP_WAITING_FOR_PEER;
        request->state = NETWORK_HTTP_REQUEST_RECEIVING;
        const enum tcp_client_step_result step =
            tcp_client_begin_wait_for_peer_close(&request->tcp);
        if (step == TCP_CLIENT_STEP_FAILED) {
            network_http_request_fail(
                request,
                network_map_tcp_error(
                    tcp_client_get_error(&request->tcp)));
        }
        return;
    }

    size_t chunk =
        request->request_length - request->request_sent;
    if (chunk > TCP_CLIENT_MAX_SEGMENT_DATA) {
        chunk = TCP_CLIENT_MAX_SEGMENT_DATA;
    }
    request->request_chunk_length = chunk;
    request->state = NETWORK_HTTP_REQUEST_SENDING;
    const enum tcp_client_step_result step =
        tcp_client_begin_send(
            &request->tcp,
            request->request_bytes + request->request_sent,
            chunk);
    if (step == TCP_CLIENT_STEP_FAILED) {
        network_http_request_fail(
            request,
            network_map_tcp_error(
                tcp_client_get_error(&request->tcp)));
    }
}

static void network_http_parse_completed_response(
    struct network_http_request* request) {
    struct http_response response;
    const enum http_client_error parsed =
        http_client_parse_response(
            request->response_bytes,
            tcp_client_received_length(&request->tcp),
            &response);
    if (parsed != HTTP_CLIENT_ERROR_NONE) {
        network_http_request_fail(
            request,
            network_map_http_error(
                parsed, tcp_client_get_error(&request->tcp)));
        return;
    }

    request->http_status = response.status_code;
    if (request->follow_redirects &&
        http_response_is_redirect(&response)) {
        if (request->redirect_count >=
            NETWORK_HTTP_REDIRECT_LIMIT) {
            network_http_request_fail(
                request, NETWORK_PROTOCOL_ERROR);
            return;
        }
        char location[NETWORK_URL_CAPACITY];
        size_t location_length = 0;
        const enum http_client_error copied =
            http_response_copy_redirect_location(
                &response, location, sizeof(location),
                &location_length);
        if (copied != HTTP_CLIENT_ERROR_NONE ||
            location_length == 0) {
            network_http_request_fail(
                request,
                network_map_http_error(
                    copied, tcp_client_get_error(&request->tcp)));
            return;
        }
        char redirected[NETWORK_URL_CAPACITY];
        const enum network_result resolved =
            network_resolve_redirect_url(
                request, location, redirected);
        if (resolved != NETWORK_OK) {
            network_http_request_fail(request, resolved);
            return;
        }
        if (network_http_url_was_visited(
                request, redirected) ||
            request->visited_count >=
                NETWORK_HTTP_VISITED_CAPACITY) {
            network_http_request_fail(
                request, NETWORK_PROTOCOL_ERROR);
            return;
        }
        if (!network_copy_text(
                request->url, sizeof(request->url),
                redirected) ||
            !network_copy_text(
                request->visited[request->visited_count],
                sizeof(request->visited[0]), redirected)) {
            network_http_request_fail(
                request, NETWORK_PROTOCOL_ERROR);
            return;
        }
        request->visited_count++;
        request->redirect_count++;
        request->state = NETWORK_HTTP_REQUEST_REDIRECTING;
        request->result = NETWORK_OK;
        request->dns_started = false;
        request->tcp_initialized = false;
        g_active_tcp_client = NULL;
        return;
    }

    const uintptr_t response_start =
        (uintptr_t)request->response_bytes;
    const uintptr_t body_start =
        (uintptr_t)response.body;
    if (body_start < response_start ||
        body_start - response_start > request->response_capacity ||
        response.body_length >
            request->response_capacity -
                (size_t)(body_start - response_start) ||
        response.body_length > request->body_capacity) {
        network_http_request_fail(
            request, NETWORK_RESPONSE_TOO_LARGE);
        return;
    }
    request->body_offset =
        (size_t)(body_start - response_start);
    request->body_length = response.body_length;
    network_http_request_finish(
        request, NETWORK_HTTP_REQUEST_COMPLETE, NETWORK_OK);
}

static void network_http_drive_receiving(
    struct network_http_request* request) {
    const enum tcp_client_step_result step =
        tcp_client_service(&request->tcp);
    if (step == TCP_CLIENT_STEP_FAILED) {
        network_http_request_fail(
            request,
            network_map_tcp_error(
                tcp_client_get_error(&request->tcp)));
        return;
    }
    if (step != TCP_CLIENT_STEP_COMPLETE) return;

    if (request->receive_phase ==
        NETWORK_HTTP_WAITING_FOR_PEER) {
        request->receive_phase =
            NETWORK_HTTP_CLOSING_CONNECTION;
        const enum tcp_client_step_result close_step =
            tcp_client_begin_close(&request->tcp);
        if (close_step == TCP_CLIENT_STEP_FAILED) {
            network_http_request_fail(
                request,
                network_map_tcp_error(
                    tcp_client_get_error(&request->tcp)));
            return;
        }
        if (close_step != TCP_CLIENT_STEP_COMPLETE) return;
    }
    network_http_parse_completed_response(request);
}

static void network_http_request_drive(void) {
    struct network_http_request* request = &g_http_request;
    if (!network_http_request_active(request)) return;
    if (network_http_request_timed_out(request)) {
        network_http_request_fail(request, NETWORK_TIMEOUT);
        return;
    }

    struct rtl8139_status device;
    if (!rtl8139_get_status(&device) || !device.ready) {
        network_http_request_fail(request, NETWORK_NOT_READY);
        return;
    }
    if (!device.link_up) {
        network_http_request_fail(request, NETWORK_LINK_DOWN);
        return;
    }

    switch (request->state) {
        case NETWORK_HTTP_REQUEST_WAITING:
        case NETWORK_HTTP_REQUEST_CONFIGURING:
            if (!net_is_configured(&g_stack)) {
                if (net_dhcp_get_state(&g_stack) ==
                        NET_DHCP_OFF &&
                    !net_dhcp_start(
                        &g_stack, network_next_nonce())) {
                    network_http_request_fail(
                        request, NETWORK_IO_ERROR);
                } else {
                    request->state =
                        NETWORK_HTTP_REQUEST_CONFIGURING;
                }
                return;
            }
            network_http_begin_resolution(request);
            return;
        case NETWORK_HTTP_REQUEST_RESOLVING: {
            const struct net_dns_result dns =
                net_dns_get_result(&g_stack);
            if (dns.state == NET_DNS_RESOLVED) {
                request->remote_ip = dns.address;
                net_dns_clear_result(&g_stack);
                request->dns_started = false;
                network_http_begin_connect(request);
            } else if (dns.state == NET_DNS_NOT_FOUND ||
                       dns.state ==
                           NET_DNS_MALFORMED_RESPONSE ||
                       dns.state == NET_DNS_SERVER_ERROR) {
                net_dns_clear_result(&g_stack);
                request->dns_started = false;
                network_http_request_fail(
                    request, NETWORK_DNS_FAILED);
            } else if (dns.state == NET_DNS_TIMED_OUT) {
                net_dns_clear_result(&g_stack);
                request->dns_started = false;
                network_http_request_fail(
                    request, NETWORK_TIMEOUT);
            }
            return;
        }
        case NETWORK_HTTP_REQUEST_CONNECTING: {
            const enum tcp_client_step_result step =
                tcp_client_service(&request->tcp);
            if (step == TCP_CLIENT_STEP_FAILED) {
                network_http_request_fail(
                    request,
                    network_map_tcp_error(
                        tcp_client_get_error(&request->tcp)));
            } else if (step == TCP_CLIENT_STEP_COMPLETE) {
                network_http_begin_send(request);
            }
            return;
        }
        case NETWORK_HTTP_REQUEST_SENDING: {
            const enum tcp_client_step_result step =
                tcp_client_service(&request->tcp);
            if (step == TCP_CLIENT_STEP_FAILED) {
                network_http_request_fail(
                    request,
                    network_map_tcp_error(
                        tcp_client_get_error(&request->tcp)));
            } else if (step == TCP_CLIENT_STEP_COMPLETE) {
                request->request_sent +=
                    request->request_chunk_length;
                request->request_chunk_length = 0;
                network_http_begin_send(request);
            }
            return;
        }
        case NETWORK_HTTP_REQUEST_RECEIVING:
            network_http_drive_receiving(request);
            return;
        case NETWORK_HTTP_REQUEST_REDIRECTING:
            network_http_begin_resolution(request);
            return;
        case NETWORK_HTTP_REQUEST_COMPLETE:
        case NETWORK_HTTP_REQUEST_FAILED:
        case NETWORK_HTTP_REQUEST_CANCELED:
            return;
    }
}

static uint64_t network_http_next_handle(void) {
    if (g_next_http_handle == 0) g_next_http_handle = 1u;
    const uint64_t handle = g_next_http_handle++;
    if (g_next_http_handle == 0) g_next_http_handle = 1u;
    return handle;
}

enum network_result network_http_request_start(
    const char* url,
    size_t body_capacity,
    bool follow_redirects,
    uint32_t timeout_milliseconds,
    uint64_t* out_handle) {
    if (out_handle != NULL) *out_handle = 0;
    if (url == NULL || out_handle == NULL ||
        body_capacity == 0 ||
        body_capacity > NETWORK_HTTP_BODY_MAX ||
        timeout_milliseconds == 0 ||
        timeout_milliseconds > NETWORK_HTTP_TIMEOUT_MAX_MS) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (network_starts_with_ci(url, "https://")) {
        return NETWORK_UNSUPPORTED;
    }
    struct network_url parsed;
    if (!network_parse_http_url(url, &parsed)) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (!g_stack_ready) return NETWORK_NOT_READY;
    struct rtl8139_status device;
    if (!rtl8139_get_status(&device) || !device.ready) {
        return NETWORK_NOT_READY;
    }
    if (!device.link_up) return NETWORK_LINK_DOWN;
    if (g_operation_busy || g_http_request.allocated) {
        return NETWORK_BUSY;
    }
    if (body_capacity >
        SIZE_MAX - NETWORK_TCP_RESPONSE_ALLOWANCE) {
        return NETWORK_INVALID_ARGUMENT;
    }

    const size_t response_capacity =
        body_capacity + NETWORK_TCP_RESPONSE_ALLOWANCE;
    uint8_t* response =
        (uint8_t*)kmalloc(response_capacity);
    if (response == NULL) return NETWORK_IO_ERROR;

    network_zero(&g_http_request, sizeof(g_http_request));
    g_http_request.allocated = true;
    g_http_request.follow_redirects = follow_redirects;
    g_http_request.handle = network_http_next_handle();
    g_http_request.started_at_ms = timer_get_milliseconds();
    g_http_request.timeout_ms = timeout_milliseconds;
    g_http_request.state =
        NETWORK_HTTP_REQUEST_CONFIGURING;
    g_http_request.result = NETWORK_OK;
    g_http_request.body_capacity = body_capacity;
    g_http_request.response_capacity = response_capacity;
    g_http_request.response_bytes = response;

    size_t url_length = 0;
    while (url[url_length] != '\0' &&
           url[url_length] != '#') {
        if (url_length + 1u >=
            sizeof(g_http_request.url)) {
            kfree(response);
            network_zero(
                &g_http_request, sizeof(g_http_request));
            return NETWORK_INVALID_ARGUMENT;
        }
        g_http_request.url[url_length] = url[url_length];
        url_length++;
    }
    g_http_request.url[url_length] = '\0';
    if (!network_normalize_http_url_path(
            g_http_request.url) ||
        !network_copy_text(
            g_http_request.visited[0],
            sizeof(g_http_request.visited[0]),
            g_http_request.url)) {
        kfree(response);
        network_zero(
            &g_http_request, sizeof(g_http_request));
        return NETWORK_INVALID_ARGUMENT;
    }
    g_http_request.visited_count = 1u;
    g_operation_busy = true;
    *out_handle = g_http_request.handle;
    return NETWORK_OK;
}

bool network_http_request_get_status(
    uint64_t handle,
    struct network_http_request_status* out_status) {
    if (out_status == NULL || handle == 0 ||
        !g_http_request.allocated ||
        g_http_request.handle != handle) {
        return false;
    }
    network_zero(out_status, sizeof(*out_status));
    out_status->state = g_http_request.state;
    out_status->result = g_http_request.result;
    out_status->http_status = g_http_request.http_status;
    out_status->redirect_count =
        g_http_request.redirect_count;
    if (g_http_request.state ==
        NETWORK_HTTP_REQUEST_COMPLETE) {
        out_status->received_bytes =
            g_http_request.body_length;
        out_status->total_bytes =
            g_http_request.body_length;
        out_status->total_known = true;
    } else if (g_http_request.tcp_initialized) {
        const size_t received =
            tcp_client_received_length(&g_http_request.tcp);
        out_status->received_bytes =
            http_client_response_body_progress(
                g_http_request.response_bytes,
                received,
                g_http_request.body_capacity);
    }
    return true;
}

enum network_result network_http_request_read(
    uint64_t handle,
    void* buffer,
    size_t capacity,
    size_t offset,
    size_t* out_length) {
    if (out_length != NULL) *out_length = 0;
    if (handle == 0 || out_length == NULL ||
        (buffer == NULL && capacity != 0) ||
        !g_http_request.allocated ||
        g_http_request.handle != handle) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (g_http_request.state !=
        NETWORK_HTTP_REQUEST_COMPLETE) {
        if (g_http_request.state ==
            NETWORK_HTTP_REQUEST_FAILED) {
            return g_http_request.result;
        }
        if (g_http_request.state ==
            NETWORK_HTTP_REQUEST_CANCELED) {
            return NETWORK_CANCELED;
        }
        return NETWORK_BUSY;
    }
    if (capacity == 0 ||
        offset >= g_http_request.body_length) {
        return NETWORK_OK;
    }
    size_t copied =
        g_http_request.body_length - offset;
    if (copied > capacity) copied = capacity;
    network_copy(
        buffer,
        g_http_request.response_bytes +
            g_http_request.body_offset + offset,
        copied);
    *out_length = copied;
    return NETWORK_OK;
}

enum network_result network_http_request_cancel(
    uint64_t handle) {
    if (handle == 0 || !g_http_request.allocated ||
        g_http_request.handle != handle) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (!network_http_request_active(&g_http_request)) {
        return NETWORK_OK;
    }
    network_http_request_finish(
        &g_http_request,
        NETWORK_HTTP_REQUEST_CANCELED,
        NETWORK_CANCELED);
    return NETWORK_OK;
}

enum network_result network_http_request_close(
    uint64_t handle) {
    if (handle == 0 || !g_http_request.allocated ||
        g_http_request.handle != handle) {
        return NETWORK_INVALID_ARGUMENT;
    }
    if (network_http_request_active(&g_http_request)) {
        (void)network_http_request_cancel(handle);
    }
    uint8_t* response = g_http_request.response_bytes;
    network_zero(&g_http_request, sizeof(g_http_request));
    if (response != NULL) kfree(response);
    return NETWORK_OK;
}

enum network_result network_http_get(
    const char* url, char* body, size_t body_capacity,
    size_t* out_body_length, unsigned int* out_status_code,
    uint32_t timeout_milliseconds) {
    if (out_body_length != NULL) *out_body_length = 0;
    if (out_status_code != NULL) *out_status_code = 0;
    if (url == NULL || body == NULL || body_capacity == 0 ||
        out_body_length == NULL || out_status_code == NULL ||
        timeout_milliseconds == 0) {
        return NETWORK_INVALID_ARGUMENT;
    }
    body[0] = '\0';
    if (g_operation_busy) return NETWORK_BUSY;
    g_operation_busy = true;

    struct network_url parsed;
    if (!network_parse_http_url(url, &parsed)) {
        g_operation_busy = false;
        return network_starts_with_ci(url, "https://")
             ? NETWORK_UNSUPPORTED : NETWORK_INVALID_ARGUMENT;
    }
    enum network_result configured =
        network_require_configuration(NETWORK_DHCP_TIMEOUT_MS);
    if (configured != NETWORK_OK) {
        g_operation_busy = false;
        return configured;
    }

    uint32_t remote_ip = 0;
    enum network_result resolved = network_resolve_internal(
        parsed.hostname, timeout_milliseconds, &remote_ip);
    if (resolved != NETWORK_OK) {
        g_operation_busy = false;
        return resolved;
    }

    if (body_capacity - 1u >
        SIZE_MAX - NETWORK_TCP_RESPONSE_ALLOWANCE) {
        g_operation_busy = false;
        return NETWORK_INVALID_ARGUMENT;
    }
    const size_t response_capacity =
        body_capacity - 1u + NETWORK_TCP_RESPONSE_ALLOWANCE;
    uint8_t* response_bytes =
        (uint8_t*)kmalloc(response_capacity);
    if (response_bytes == NULL) {
        g_operation_busy = false;
        return NETWORK_IO_ERROR;
    }
    uint8_t* request_bytes =
        (uint8_t*)kmalloc(NETWORK_HTTP_REQUEST_CAPACITY);
    if (request_bytes == NULL) {
        kfree(response_bytes);
        g_operation_busy = false;
        return NETWORK_IO_ERROR;
    }

    const struct net_ipv4_config ipv4 =
        net_get_ipv4_config(&g_stack);
    const struct tcp_client_io io = {
        .send_ipv4 = network_tcp_send_ipv4,
        .now_ms = network_tcp_now,
        .poll = network_tcp_poll,
        .context = &g_stack,
    };
    struct tcp_client tcp;
    tcp_client_init(
        &tcp, &io, ipv4.address, network_next_port(),
        response_bytes, response_capacity);
    uint32_t retransmit = 250u;
    if (timeout_milliseconds < retransmit) {
        retransmit = timeout_milliseconds;
    }
    (void)tcp_client_set_timeouts(
        &tcp, retransmit, timeout_milliseconds, 4u);
    g_active_tcp_client = &tcp;

    struct http_response response;
    enum http_client_error http_result = http_client_get(
        &tcp, remote_ip, parsed.port, network_next_nonce(),
        parsed.host_header, parsed.path, request_bytes,
        NETWORK_HTTP_REQUEST_CAPACITY, &response);
    g_active_tcp_client = NULL;

    enum network_result result = network_map_http_error(
        http_result, tcp_client_get_error(&tcp));
    if (result == NETWORK_OK) {
        if (response.body_length >= body_capacity) {
            result = NETWORK_RESPONSE_TOO_LARGE;
        } else {
            network_copy(
                body, response.body, response.body_length);
            body[response.body_length] = '\0';
            *out_body_length = response.body_length;
            *out_status_code = response.status_code;
        }
    }

    kfree(request_bytes);
    kfree(response_bytes);
    g_operation_busy = false;
    return result;
}

const char* network_result_text(enum network_result result) {
    switch (result) {
        case NETWORK_OK:
            return "request completed";
        case NETWORK_NOT_READY:
            return "network device is unavailable";
        case NETWORK_LINK_DOWN:
            return "network link is down";
        case NETWORK_BUSY:
            return "another network operation is active";
        case NETWORK_INVALID_ARGUMENT:
            return "invalid network address or request";
        case NETWORK_TIMEOUT:
            return "network operation timed out";
        case NETWORK_DNS_FAILED:
            return "DNS lookup failed";
        case NETWORK_CONNECTION_FAILED:
            return "TCP connection failed";
        case NETWORK_PROTOCOL_ERROR:
            return "peer returned a malformed response";
        case NETWORK_RESPONSE_TOO_LARGE:
            return "response exceeds the available buffer";
        case NETWORK_UNSUPPORTED:
            return "protocol feature is not supported";
        case NETWORK_IO_ERROR:
            return "network device rejected the operation";
        case NETWORK_CANCELED:
            return "network operation was canceled";
    }
    return "unknown network error";
}

static char network_hex_digit(uint8_t value) {
    if (value < 10u) return (char)('0' + (char)value);
    return (char)('a' + (char)(value - 10u));
}

void network_format_mac(
    const uint8_t mac[6], char* buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;
    buffer[0] = '\0';
    if (mac == NULL || capacity < 18u) return;
    size_t output = 0;
    for (size_t index = 0; index < 6u; index++) {
        if (index != 0) buffer[output++] = ':';
        buffer[output++] = network_hex_digit(
            (uint8_t)(mac[index] >> 4));
        buffer[output++] = network_hex_digit(
            (uint8_t)(mac[index] & 0x0fu));
    }
    buffer[output] = '\0';
}

static size_t network_write_decimal(
    uint8_t value, char* buffer, size_t offset) {
    if (value >= 100u) {
        buffer[offset++] = (char)('0' + value / 100u);
    }
    if (value >= 10u) {
        buffer[offset++] =
            (char)('0' + (value / 10u) % 10u);
    }
    buffer[offset++] = (char)('0' + value % 10u);
    return offset;
}

void network_format_ipv4(
    uint32_t address, char* buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;
    buffer[0] = '\0';
    if (capacity < 16u) return;
    size_t output = 0;
    for (unsigned int index = 0; index < 4u; index++) {
        if (index != 0) buffer[output++] = '.';
        const unsigned int shift = 24u - index * 8u;
        output = network_write_decimal(
            (uint8_t)(address >> shift), buffer, output);
    }
    buffer[output] = '\0';
}
