#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Nostalux IPv4 addresses use canonical numeric order:
 * 10.0.2.15 is 0x0A00020F. TCP ports are ordinary host-order integers.
 */
#define TCP_CLIENT_IPV4_PROTOCOL 6u
#define TCP_CLIENT_DEFAULT_MSS 536u
#define TCP_CLIENT_MAX_SEGMENT_DATA TCP_CLIENT_DEFAULT_MSS
#define TCP_CLIENT_DEFAULT_RETRANSMIT_MS 250u
#define TCP_CLIENT_DEFAULT_OPERATION_TIMEOUT_MS 5000u
#define TCP_CLIENT_DEFAULT_MAX_RETRIES 4u
#define TCP_CLIENT_CLOCK_STALL_SERVICE_LIMIT 1000000u

enum tcp_client_state {
    TCP_CLIENT_CLOSED = 0,
    TCP_CLIENT_SYN_SENT,
    TCP_CLIENT_ESTABLISHED,
    TCP_CLIENT_FIN_WAIT_1,
    TCP_CLIENT_FIN_WAIT_2,
    TCP_CLIENT_CLOSING,
    TCP_CLIENT_CLOSE_WAIT,
    TCP_CLIENT_LAST_ACK,
    TCP_CLIENT_FAILED
};

enum tcp_client_error {
    TCP_CLIENT_ERROR_NONE = 0,
    TCP_CLIENT_ERROR_INVALID_ARGUMENT,
    TCP_CLIENT_ERROR_BUSY,
    TCP_CLIENT_ERROR_NOT_CONNECTED,
    TCP_CLIENT_ERROR_SEND_FAILED,
    TCP_CLIENT_ERROR_TIMEOUT,
    TCP_CLIENT_ERROR_RETRIES_EXHAUSTED,
    TCP_CLIENT_ERROR_CLOCK_MOVED_BACKWARD,
    TCP_CLIENT_ERROR_CLOCK_STALLED,
    TCP_CLIENT_ERROR_RESET,
    TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE,
    TCP_CLIENT_ERROR_PROTOCOL
};

enum tcp_client_receive_result {
    TCP_CLIENT_RECEIVE_IGNORED = 0,
    TCP_CLIENT_RECEIVE_ACCEPTED,
    TCP_CLIENT_RECEIVE_BAD_CHECKSUM,
    TCP_CLIENT_RECEIVE_MALFORMED,
    TCP_CLIENT_RECEIVE_RESPONSE_TOO_LARGE
};

enum tcp_client_operation {
    TCP_CLIENT_OPERATION_NONE = 0,
    TCP_CLIENT_OPERATION_CONNECT,
    TCP_CLIENT_OPERATION_SEND,
    TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE,
    TCP_CLIENT_OPERATION_CLOSE
};

enum tcp_client_step_result {
    TCP_CLIENT_STEP_PENDING = 0,
    TCP_CLIENT_STEP_COMPLETE,
    TCP_CLIENT_STEP_FAILED
};

/*
 * send_ipv4 receives a complete TCP segment and must consume or copy it before
 * returning. The surrounding IPv4 layer supplies its own header.
 *
 * poll is optional for the asynchronous API. The blocking compatibility API
 * requires it and uses it to service the NIC/protocol layer; it may
 * synchronously call tcp_client_receive() through the IPv4 protocol-6 handler.
 */
typedef bool (*tcp_client_send_ipv4_fn)(
    void* context,
    uint32_t destination_ip,
    uint8_t protocol,
    const void* payload,
    size_t payload_length);
typedef uint64_t (*tcp_client_now_ms_fn)(void* context);
typedef void (*tcp_client_poll_fn)(void* context);

struct tcp_client_io {
    tcp_client_send_ipv4_fn send_ipv4;
    tcp_client_now_ms_fn now_ms;
    tcp_client_poll_fn poll;
    void* context;
};

/*
 * The caller owns both this object and receive_buffer for the full connection.
 * Received bytes are never truncated: capacity exhaustion fails the connection
 * with TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE.
 */
struct tcp_client {
    struct tcp_client_io io;
    enum tcp_client_state state;
    enum tcp_client_error error;

    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t send_unacknowledged;
    uint32_t send_next;
    uint32_t receive_next;

    uint8_t* receive_buffer;
    size_t receive_capacity;
    size_t receive_length;

    uint32_t retransmit_ms;
    uint32_t operation_timeout_ms;
    unsigned int max_retries;

    enum tcp_client_operation operation;
    enum tcp_client_step_result step_result;
    uint64_t operation_started_at_ms;
    uint64_t operation_previous_now_ms;
    unsigned int operation_stalled_services;

    bool pending_active;
    uint8_t pending_flags;
    uint32_t pending_sequence;
    uint32_t pending_end;
    size_t pending_length;
    uint8_t pending_data[TCP_CLIENT_MAX_SEGMENT_DATA];
    uint64_t pending_sent_at_ms;
    unsigned int pending_retries;

    unsigned int bad_checksum_count;
    unsigned int malformed_segment_count;
};

void tcp_client_init(
    struct tcp_client* client,
    const struct tcp_client_io* io,
    uint32_t local_ip,
    uint16_t local_port,
    void* receive_buffer,
    size_t receive_capacity);

bool tcp_client_set_timeouts(
    struct tcp_client* client,
    uint32_t retransmit_ms,
    uint32_t operation_timeout_ms,
    unsigned int max_retries);

/*
 * Each begin function starts at most one operation and returns immediately.
 * begin_send accepts one segment only (0..TCP_CLIENT_MAX_SEGMENT_DATA bytes).
 * Call tcp_client_service() after the surrounding network stack has polled.
 * tcp_client_service() reads the clock and may retransmit, but never calls
 * io.poll itself.
 */
enum tcp_client_step_result tcp_client_begin_connect(
    struct tcp_client* client,
    uint32_t remote_ip,
    uint16_t remote_port,
    uint32_t initial_sequence);
enum tcp_client_step_result tcp_client_begin_send(
    struct tcp_client* client,
    const void* data,
    size_t length);
enum tcp_client_step_result
tcp_client_begin_wait_for_peer_close(
    struct tcp_client* client);
enum tcp_client_step_result tcp_client_begin_close(
    struct tcp_client* client);
enum tcp_client_step_result tcp_client_service(
    struct tcp_client* client);
enum tcp_client_operation tcp_client_get_operation(
    const struct tcp_client* client);
enum tcp_client_step_result tcp_client_get_step_result(
    const struct tcp_client* client);

/* Blocking compatibility wrappers; these require a non-NULL io.poll. */
bool tcp_client_connect(
    struct tcp_client* client,
    uint32_t remote_ip,
    uint16_t remote_port,
    uint32_t initial_sequence);

bool tcp_client_send(
    struct tcp_client* client,
    const void* data,
    size_t length);

/* Waits for a peer FIN without silently treating a timeout as end-of-stream. */
bool tcp_client_wait_for_peer_close(struct tcp_client* client);

/* Performs the active or passive-side FIN handshake, as appropriate. */
bool tcp_client_close(struct tcp_client* client);

/*
 * Feed an IPv4 protocol-6 payload into the client. src_ip and dst_ip use the
 * same canonical numeric order as local_ip and remote_ip.
 */
enum tcp_client_receive_result tcp_client_receive(
    struct tcp_client* client,
    uint32_t src_ip,
    uint32_t dst_ip,
    const void* segment,
    size_t segment_length);

/*
 * Computes the TCP pseudo-header checksum. To create a packet, zero bytes
 * 16..17 first. A valid received segment produces zero.
 */
uint16_t tcp_client_checksum(
    uint32_t src_ip,
    uint32_t dst_ip,
    const void* segment,
    size_t segment_length);

enum tcp_client_state tcp_client_get_state(const struct tcp_client* client);
enum tcp_client_error tcp_client_get_error(const struct tcp_client* client);
const char* tcp_client_error_string(enum tcp_client_error error);
const uint8_t* tcp_client_received_data(const struct tcp_client* client);
size_t tcp_client_received_length(const struct tcp_client* client);

#endif /* TCP_CLIENT_H */
