#include "tcp_client.h"

#define TCP_HEADER_LENGTH 20u
#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u
#define TCP_FLAG_URG 0x20u

static void copy_bytes(void* destination, const void* source, size_t length) {
    uint8_t* out = (uint8_t*)destination;
    const uint8_t* in = (const uint8_t*)source;

    for (size_t i = 0; i < length; i++) {
        out[i] = in[i];
    }
}

static void zero_bytes(void* destination, size_t length) {
    uint8_t* out = (uint8_t*)destination;

    for (size_t i = 0; i < length; i++) {
        out[i] = 0;
    }
}

static uint16_t read_u16(const uint8_t* bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_u32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void write_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static bool sequence_before(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) < 0;
}

static bool sequence_after(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) > 0;
}

static uint16_t advertised_window(const struct tcp_client* client) {
    size_t remaining = client->receive_capacity - client->receive_length;

    if (remaining > 65535u) {
        remaining = 65535u;
    }
    return (uint16_t)remaining;
}

static void fail_client(
    struct tcp_client* client,
    enum tcp_client_error error) {
    client->error = error;
    client->state = TCP_CLIENT_FAILED;
    client->pending_active = false;
    client->step_result = TCP_CLIENT_STEP_FAILED;
}

uint16_t tcp_client_checksum(
    uint32_t src_ip,
    uint32_t dst_ip,
    const void* segment,
    size_t segment_length) {
    const uint8_t* bytes = (const uint8_t*)segment;
    uint64_t sum = 0;

    if (segment == NULL && segment_length != 0) {
        return 0;
    }
    if (segment_length > 65535u) {
        return 0;
    }

    sum += (uint16_t)(src_ip >> 16);
    sum += (uint16_t)src_ip;
    sum += (uint16_t)(dst_ip >> 16);
    sum += (uint16_t)dst_ip;
    sum += TCP_CLIENT_IPV4_PROTOCOL;
    sum += (uint16_t)segment_length;

    for (size_t i = 0; i + 1 < segment_length; i += 2) {
        sum += (uint16_t)(((uint16_t)bytes[i] << 8) | bytes[i + 1]);
    }
    if ((segment_length & 1u) != 0) {
        sum += (uint16_t)((uint16_t)bytes[segment_length - 1] << 8);
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static bool send_segment(
    struct tcp_client* client,
    uint32_t sequence,
    uint8_t flags,
    const uint8_t* data,
    size_t data_length) {
    uint8_t packet[TCP_HEADER_LENGTH + TCP_CLIENT_MAX_SEGMENT_DATA];
    size_t packet_length = TCP_HEADER_LENGTH + data_length;
    uint16_t checksum;

    if (data_length > TCP_CLIENT_MAX_SEGMENT_DATA) {
        fail_client(client, TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return false;
    }

    zero_bytes(packet, TCP_HEADER_LENGTH);
    write_u16(&packet[0], client->local_port);
    write_u16(&packet[2], client->remote_port);
    write_u32(&packet[4], sequence);
    if ((flags & TCP_FLAG_ACK) != 0) {
        write_u32(&packet[8], client->receive_next);
    }
    packet[12] = (uint8_t)(5u << 4);
    packet[13] = flags;
    write_u16(&packet[14], advertised_window(client));
    if (data_length != 0) {
        copy_bytes(&packet[TCP_HEADER_LENGTH], data, data_length);
    }

    checksum = tcp_client_checksum(
        client->local_ip,
        client->remote_ip,
        packet,
        packet_length);
    write_u16(&packet[16], checksum);

    if (!client->io.send_ipv4(
            client->io.context,
            client->remote_ip,
            TCP_CLIENT_IPV4_PROTOCOL,
            packet,
            packet_length)) {
        fail_client(client, TCP_CLIENT_ERROR_SEND_FAILED);
        return false;
    }
    return true;
}

static bool send_ack(struct tcp_client* client) {
    return send_segment(
        client,
        client->send_next,
        TCP_FLAG_ACK,
        NULL,
        0);
}

static bool start_pending_segment(
    struct tcp_client* client,
    uint8_t flags,
    const uint8_t* data,
    size_t data_length) {
    uint32_t sequence_length = (uint32_t)data_length;

    if (client->pending_active || data_length > TCP_CLIENT_MAX_SEGMENT_DATA) {
        fail_client(client, TCP_CLIENT_ERROR_PROTOCOL);
        return false;
    }
    if ((flags & TCP_FLAG_SYN) != 0) {
        sequence_length++;
    }
    if ((flags & TCP_FLAG_FIN) != 0) {
        sequence_length++;
    }
    if (data_length != 0) {
        copy_bytes(client->pending_data, data, data_length);
    }

    client->pending_active = true;
    client->pending_flags = flags;
    client->pending_sequence = client->send_next;
    client->pending_end = client->send_next + sequence_length;
    client->pending_length = data_length;
    client->pending_retries = 0;
    client->pending_sent_at_ms =
        client->operation_previous_now_ms;
    client->send_next = client->pending_end;

    return send_segment(
        client,
        client->pending_sequence,
        client->pending_flags,
        client->pending_data,
        client->pending_length);
}

static bool retransmit_pending(struct tcp_client* client, uint64_t now_ms) {
    if (!client->pending_active) {
        return true;
    }
    if (client->pending_retries >= client->max_retries) {
        fail_client(client, TCP_CLIENT_ERROR_RETRIES_EXHAUSTED);
        return false;
    }

    client->pending_retries++;
    client->pending_sent_at_ms = now_ms;
    return send_segment(
        client,
        client->pending_sequence,
        client->pending_flags,
        client->pending_data,
        client->pending_length);
}

static bool operation_is_busy(
    const struct tcp_client* client) {
    return client->operation != TCP_CLIENT_OPERATION_NONE &&
           client->step_result == TCP_CLIENT_STEP_PENDING;
}

static bool operation_goal_reached(
    const struct tcp_client* client) {
    switch (client->operation) {
        case TCP_CLIENT_OPERATION_NONE:
            return true;
        case TCP_CLIENT_OPERATION_CONNECT:
            return client->state == TCP_CLIENT_ESTABLISHED;
        case TCP_CLIENT_OPERATION_SEND:
            return !client->pending_active;
        case TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE:
            return client->state == TCP_CLIENT_CLOSE_WAIT ||
                   client->state == TCP_CLIENT_CLOSED;
        case TCP_CLIENT_OPERATION_CLOSE:
            return client->state == TCP_CLIENT_CLOSED;
    }
    return false;
}

static enum tcp_client_step_result complete_if_reached(
    struct tcp_client* client) {
    if (client->state == TCP_CLIENT_FAILED) {
        client->step_result = TCP_CLIENT_STEP_FAILED;
    } else if (operation_goal_reached(client)) {
        client->step_result = TCP_CLIENT_STEP_COMPLETE;
        client->error = TCP_CLIENT_ERROR_NONE;
    }
    return client->step_result;
}

static enum tcp_client_step_result reject_operation(
    struct tcp_client* client,
    enum tcp_client_operation operation,
    enum tcp_client_error error) {
    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if (operation_is_busy(client)) {
        client->error = TCP_CLIENT_ERROR_BUSY;
        return TCP_CLIENT_STEP_FAILED;
    }
    client->operation = operation;
    client->step_result = TCP_CLIENT_STEP_FAILED;
    client->error = error;
    return TCP_CLIENT_STEP_FAILED;
}

static void begin_operation(
    struct tcp_client* client,
    enum tcp_client_operation operation) {
    uint64_t now =
        client->io.now_ms(client->io.context);

    client->operation = operation;
    client->step_result = TCP_CLIENT_STEP_PENDING;
    client->operation_started_at_ms = now;
    client->operation_previous_now_ms = now;
    client->operation_stalled_services = 0;
    client->error = TCP_CLIENT_ERROR_NONE;
}

enum tcp_client_step_result tcp_client_service(
    struct tcp_client* client) {
    uint64_t now;

    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->state == TCP_CLIENT_FAILED) {
        client->step_result = TCP_CLIENT_STEP_FAILED;
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->step_result != TCP_CLIENT_STEP_PENDING) {
        return client->step_result;
    }
    if (complete_if_reached(client) !=
        TCP_CLIENT_STEP_PENDING) {
        return client->step_result;
    }
    if (client->operation ==
            TCP_CLIENT_OPERATION_NONE ||
        client->io.now_ms == NULL) {
        fail_client(
            client,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return TCP_CLIENT_STEP_FAILED;
    }

    now = client->io.now_ms(client->io.context);
    if (now < client->operation_previous_now_ms) {
        fail_client(
            client,
            TCP_CLIENT_ERROR_CLOCK_MOVED_BACKWARD);
        return TCP_CLIENT_STEP_FAILED;
    }
    if (now == client->operation_previous_now_ms) {
        client->operation_stalled_services++;
        if (client->operation_stalled_services >=
            TCP_CLIENT_CLOCK_STALL_SERVICE_LIMIT) {
            fail_client(
                client,
                TCP_CLIENT_ERROR_CLOCK_STALLED);
            return TCP_CLIENT_STEP_FAILED;
        }
    } else {
        client->operation_stalled_services = 0;
        client->operation_previous_now_ms = now;
    }

    if (now - client->operation_started_at_ms >=
        client->operation_timeout_ms) {
        fail_client(client, TCP_CLIENT_ERROR_TIMEOUT);
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->pending_active &&
        now - client->pending_sent_at_ms >=
            client->retransmit_ms &&
        !retransmit_pending(client, now)) {
        return TCP_CLIENT_STEP_FAILED;
    }
    return complete_if_reached(client);
}

static bool drive_blocking_operation(
    struct tcp_client* client,
    enum tcp_client_step_result result) {
    while (result == TCP_CLIENT_STEP_PENDING) {
        client->io.poll(client->io.context);
        result = tcp_client_service(client);
    }
    return result == TCP_CLIENT_STEP_COMPLETE;
}

void tcp_client_init(
    struct tcp_client* client,
    const struct tcp_client_io* io,
    uint32_t local_ip,
    uint16_t local_port,
    void* receive_buffer,
    size_t receive_capacity) {
    if (client == NULL) {
        return;
    }

    zero_bytes(client, sizeof(*client));
    if (io != NULL) {
        client->io = *io;
    }
    client->state = TCP_CLIENT_CLOSED;
    client->local_ip = local_ip;
    client->local_port = local_port;
    client->receive_buffer = (uint8_t*)receive_buffer;
    client->receive_capacity = receive_capacity;
    client->retransmit_ms = TCP_CLIENT_DEFAULT_RETRANSMIT_MS;
    client->operation_timeout_ms =
        TCP_CLIENT_DEFAULT_OPERATION_TIMEOUT_MS;
    client->max_retries = TCP_CLIENT_DEFAULT_MAX_RETRIES;
    client->operation = TCP_CLIENT_OPERATION_NONE;
    client->step_result = TCP_CLIENT_STEP_COMPLETE;

    if ((receive_buffer == NULL && receive_capacity != 0) ||
        io == NULL ||
        io->send_ipv4 == NULL ||
        io->now_ms == NULL ||
        local_port == 0) {
        client->error = TCP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
}

bool tcp_client_set_timeouts(
    struct tcp_client* client,
    uint32_t retransmit_ms,
    uint32_t operation_timeout_ms,
    unsigned int max_retries) {
    if (client == NULL ||
        retransmit_ms == 0 ||
        operation_timeout_ms < retransmit_ms) {
        if (client != NULL) {
            client->error = TCP_CLIENT_ERROR_INVALID_ARGUMENT;
        }
        return false;
    }
    if (client->state != TCP_CLIENT_CLOSED) {
        client->error = TCP_CLIENT_ERROR_BUSY;
        return false;
    }

    client->retransmit_ms = retransmit_ms;
    client->operation_timeout_ms = operation_timeout_ms;
    client->max_retries = max_retries;
    client->error = TCP_CLIENT_ERROR_NONE;
    return true;
}

enum tcp_client_step_result tcp_client_begin_connect(
    struct tcp_client* client,
    uint32_t remote_ip,
    uint16_t remote_port,
    uint32_t initial_sequence) {
    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->io.send_ipv4 == NULL ||
        client->io.now_ms == NULL ||
        client->local_port == 0 ||
        (client->receive_buffer == NULL &&
         client->receive_capacity != 0) ||
        remote_ip == 0 ||
        remote_port == 0) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_CONNECT,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
    }
    if (operation_is_busy(client)) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_CONNECT,
            TCP_CLIENT_ERROR_BUSY);
    }
    if (client->state == TCP_CLIENT_FAILED) {
        client->operation = TCP_CLIENT_OPERATION_CONNECT;
        client->step_result = TCP_CLIENT_STEP_FAILED;
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->state != TCP_CLIENT_CLOSED) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_CONNECT,
            TCP_CLIENT_ERROR_BUSY);
    }

    client->remote_ip = remote_ip;
    client->remote_port = remote_port;
    client->receive_length = 0;
    client->send_unacknowledged = initial_sequence;
    client->send_next = initial_sequence;
    client->receive_next = 0;
    client->pending_active = false;
    client->bad_checksum_count = 0;
    client->malformed_segment_count = 0;
    begin_operation(
        client, TCP_CLIENT_OPERATION_CONNECT);
    client->state = TCP_CLIENT_SYN_SENT;

    if (!start_pending_segment(client, TCP_FLAG_SYN, NULL, 0)) {
        return TCP_CLIENT_STEP_FAILED;
    }
    return complete_if_reached(client);
}

enum tcp_client_step_result tcp_client_begin_send(
    struct tcp_client* client,
    const void* data,
    size_t length) {
    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if ((data == NULL && length != 0) ||
        length > TCP_CLIENT_MAX_SEGMENT_DATA) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_SEND,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
    }
    if (operation_is_busy(client)) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_SEND,
            TCP_CLIENT_ERROR_BUSY);
    }
    if (client->state == TCP_CLIENT_FAILED) {
        client->operation = TCP_CLIENT_OPERATION_SEND;
        client->step_result = TCP_CLIENT_STEP_FAILED;
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->state != TCP_CLIENT_ESTABLISHED) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_SEND,
            TCP_CLIENT_ERROR_NOT_CONNECTED);
    }

    begin_operation(client, TCP_CLIENT_OPERATION_SEND);
    if (length == 0) {
        return complete_if_reached(client);
    }
    if (!start_pending_segment(
            client,
            TCP_FLAG_ACK | TCP_FLAG_PSH,
            (const uint8_t*)data,
            length)) {
        return TCP_CLIENT_STEP_FAILED;
    }
    return complete_if_reached(client);
}

enum tcp_client_step_result
tcp_client_begin_wait_for_peer_close(
    struct tcp_client* client) {
    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->io.now_ms == NULL) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
    }
    if (operation_is_busy(client)) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE,
            TCP_CLIENT_ERROR_BUSY);
    }
    if (client->state == TCP_CLIENT_FAILED) {
        client->operation =
            TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE;
        client->step_result = TCP_CLIENT_STEP_FAILED;
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->state == TCP_CLIENT_CLOSE_WAIT ||
        client->state == TCP_CLIENT_CLOSED) {
        begin_operation(
            client,
            TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE);
        return complete_if_reached(client);
    }
    if (client->state != TCP_CLIENT_ESTABLISHED) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE,
            TCP_CLIENT_ERROR_NOT_CONNECTED);
    }

    begin_operation(
        client,
        TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE);
    return TCP_CLIENT_STEP_PENDING;
}

enum tcp_client_step_result tcp_client_begin_close(
    struct tcp_client* client) {
    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->io.send_ipv4 == NULL ||
        client->io.now_ms == NULL) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_CLOSE,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
    }
    if (operation_is_busy(client)) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_CLOSE,
            TCP_CLIENT_ERROR_BUSY);
    }
    if (client->state == TCP_CLIENT_CLOSED) {
        begin_operation(
            client, TCP_CLIENT_OPERATION_CLOSE);
        return complete_if_reached(client);
    }
    if (client->state == TCP_CLIENT_FAILED) {
        client->operation = TCP_CLIENT_OPERATION_CLOSE;
        client->step_result = TCP_CLIENT_STEP_FAILED;
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->pending_active) {
        return reject_operation(
            client,
            TCP_CLIENT_OPERATION_CLOSE,
            TCP_CLIENT_ERROR_BUSY);
    }

    begin_operation(client, TCP_CLIENT_OPERATION_CLOSE);
    if (client->state == TCP_CLIENT_ESTABLISHED) {
        client->state = TCP_CLIENT_FIN_WAIT_1;
        if (!start_pending_segment(
                client,
                TCP_FLAG_FIN | TCP_FLAG_ACK,
                NULL,
                0)) {
            return TCP_CLIENT_STEP_FAILED;
        }
    } else if (client->state == TCP_CLIENT_CLOSE_WAIT) {
        client->state = TCP_CLIENT_LAST_ACK;
        if (!start_pending_segment(
                client,
                TCP_FLAG_FIN | TCP_FLAG_ACK,
                NULL,
                0)) {
            return TCP_CLIENT_STEP_FAILED;
        }
    } else if (client->state != TCP_CLIENT_FIN_WAIT_1 &&
               client->state != TCP_CLIENT_FIN_WAIT_2 &&
               client->state != TCP_CLIENT_CLOSING &&
               client->state != TCP_CLIENT_LAST_ACK) {
        client->step_result = TCP_CLIENT_STEP_FAILED;
        client->error = TCP_CLIENT_ERROR_NOT_CONNECTED;
        return TCP_CLIENT_STEP_FAILED;
    }

    return complete_if_reached(client);
}

bool tcp_client_connect(
    struct tcp_client* client,
    uint32_t remote_ip,
    uint16_t remote_port,
    uint32_t initial_sequence) {
    enum tcp_client_step_result result;

    if (client == NULL) {
        return false;
    }
    if (client->io.poll == NULL) {
        (void)reject_operation(
            client,
            TCP_CLIENT_OPERATION_CONNECT,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return false;
    }
    result = tcp_client_begin_connect(
        client, remote_ip, remote_port, initial_sequence);
    return drive_blocking_operation(client, result);
}

bool tcp_client_send(
    struct tcp_client* client,
    const void* data,
    size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    size_t sent = 0;

    if (client == NULL) {
        return false;
    }
    if (client->io.poll == NULL) {
        (void)reject_operation(
            client,
            TCP_CLIENT_OPERATION_SEND,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return false;
    }
    if (data == NULL && length != 0) {
        (void)reject_operation(
            client,
            TCP_CLIENT_OPERATION_SEND,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return false;
    }
    if (length == 0) {
        return drive_blocking_operation(
            client,
            tcp_client_begin_send(client, NULL, 0));
    }

    while (sent < length) {
        size_t chunk = length - sent;
        enum tcp_client_step_result result;

        if (chunk > TCP_CLIENT_MAX_SEGMENT_DATA) {
            chunk = TCP_CLIENT_MAX_SEGMENT_DATA;
        }
        result = tcp_client_begin_send(
            client, &bytes[sent], chunk);
        if (!drive_blocking_operation(client, result)) {
            return false;
        }
        sent += chunk;
        if (sent < length &&
            client->state != TCP_CLIENT_ESTABLISHED) {
            client->error =
                TCP_CLIENT_ERROR_NOT_CONNECTED;
            return false;
        }
    }
    client->error = TCP_CLIENT_ERROR_NONE;
    return true;
}

bool tcp_client_wait_for_peer_close(
    struct tcp_client* client) {
    enum tcp_client_step_result result;

    if (client == NULL) {
        return false;
    }
    if (client->io.poll == NULL) {
        (void)reject_operation(
            client,
            TCP_CLIENT_OPERATION_WAIT_FOR_PEER_CLOSE,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return false;
    }
    result =
        tcp_client_begin_wait_for_peer_close(client);
    return drive_blocking_operation(client, result);
}

bool tcp_client_close(struct tcp_client* client) {
    enum tcp_client_step_result result;

    if (client == NULL) {
        return false;
    }
    if (client->io.poll == NULL) {
        (void)reject_operation(
            client,
            TCP_CLIENT_OPERATION_CLOSE,
            TCP_CLIENT_ERROR_INVALID_ARGUMENT);
        return false;
    }
    result = tcp_client_begin_close(client);
    return drive_blocking_operation(client, result);
}

static bool acknowledge_pending(
    struct tcp_client* client,
    uint32_t acknowledgment) {
    uint8_t acknowledged_flags;

    if (sequence_after(acknowledgment, client->send_next)) {
        client->malformed_segment_count++;
        return false;
    }
    if (sequence_before(
            acknowledgment,
            client->send_unacknowledged)) {
        return true;
    }

    client->send_unacknowledged = acknowledgment;
    if (!client->pending_active ||
        acknowledgment != client->pending_end) {
        return true;
    }

    acknowledged_flags = client->pending_flags;
    client->pending_active = false;
    if ((acknowledged_flags & TCP_FLAG_FIN) != 0) {
        if (client->state == TCP_CLIENT_FIN_WAIT_1) {
            client->state = TCP_CLIENT_FIN_WAIT_2;
        } else if (client->state == TCP_CLIENT_CLOSING ||
                   client->state == TCP_CLIENT_LAST_ACK) {
            client->state = TCP_CLIENT_CLOSED;
        }
    }
    return true;
}

enum tcp_client_receive_result tcp_client_receive(
    struct tcp_client* client,
    uint32_t src_ip,
    uint32_t dst_ip,
    const void* segment,
    size_t segment_length) {
    const uint8_t* packet = (const uint8_t*)segment;
    size_t header_length;
    size_t data_length;
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgment;
    uint8_t flags;

    if (client == NULL || segment == NULL) {
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }
    if (client->state == TCP_CLIENT_CLOSED ||
        client->state == TCP_CLIENT_FAILED ||
        src_ip != client->remote_ip ||
        dst_ip != client->local_ip) {
        return TCP_CLIENT_RECEIVE_IGNORED;
    }
    if (segment_length < TCP_HEADER_LENGTH ||
        segment_length > 65535u) {
        client->malformed_segment_count++;
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }

    source_port = read_u16(&packet[0]);
    destination_port = read_u16(&packet[2]);
    if (source_port != client->remote_port ||
        destination_port != client->local_port) {
        return TCP_CLIENT_RECEIVE_IGNORED;
    }
    if (tcp_client_checksum(
            src_ip,
            dst_ip,
            segment,
            segment_length) != 0) {
        client->bad_checksum_count++;
        return TCP_CLIENT_RECEIVE_BAD_CHECKSUM;
    }

    header_length = (size_t)(packet[12] >> 4) * 4u;
    if (header_length < TCP_HEADER_LENGTH ||
        header_length > segment_length ||
        (packet[13] & TCP_FLAG_URG) != 0) {
        client->malformed_segment_count++;
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }

    flags = packet[13];
    sequence = read_u32(&packet[4]);
    acknowledgment = read_u32(&packet[8]);
    data_length = segment_length - header_length;

    if ((flags & TCP_FLAG_RST) != 0) {
        bool acceptable_reset;

        if (client->state == TCP_CLIENT_SYN_SENT) {
            acceptable_reset =
                (flags & TCP_FLAG_ACK) != 0 &&
                client->pending_active &&
                acknowledgment == client->pending_end;
        } else {
            acceptable_reset = sequence == client->receive_next;
        }
        if (acceptable_reset) {
            fail_client(client, TCP_CLIENT_ERROR_RESET);
            return TCP_CLIENT_RECEIVE_ACCEPTED;
        }
        client->malformed_segment_count++;
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }

    if (client->state == TCP_CLIENT_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) !=
                (TCP_FLAG_SYN | TCP_FLAG_ACK) ||
            (flags & TCP_FLAG_FIN) != 0 ||
            data_length != 0 ||
            !client->pending_active ||
            acknowledgment != client->pending_end) {
            client->malformed_segment_count++;
            return TCP_CLIENT_RECEIVE_MALFORMED;
        }

        client->send_unacknowledged = acknowledgment;
        client->pending_active = false;
        client->receive_next = sequence + 1u;
        client->state = TCP_CLIENT_ESTABLISHED;
        if (!send_ack(client)) {
            return TCP_CLIENT_RECEIVE_ACCEPTED;
        }
        return TCP_CLIENT_RECEIVE_ACCEPTED;
    }

    if ((flags & TCP_FLAG_SYN) != 0) {
        /*
         * This can be a retransmitted SYN+ACK after our final ACK was lost.
         * Re-ACK it, but never let it change the established sequence space.
         */
        client->malformed_segment_count++;
        (void)send_ack(client);
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }
    if ((flags & TCP_FLAG_ACK) == 0) {
        client->malformed_segment_count++;
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }
    if (sequence != client->receive_next) {
        /*
         * Segment acceptability is checked before its acknowledgment can
         * mutate send state. A spoofed or stale pure ACK with the right ACK
         * number but the wrong peer sequence must not complete an operation.
         * This one-segment client has no reassembly queue, so duplicate and
         * out-of-order data are discarded and ACKed at receive_next.
         */
        if (data_length != 0 || (flags & TCP_FLAG_FIN) != 0) {
            (void)send_ack(client);
        }
        return TCP_CLIENT_RECEIVE_ACCEPTED;
    }
    if (!acknowledge_pending(client, acknowledgment)) {
        (void)send_ack(client);
        return TCP_CLIENT_RECEIVE_MALFORMED;
    }
    if (client->state == TCP_CLIENT_CLOSED) {
        return TCP_CLIENT_RECEIVE_ACCEPTED;
    }

    if (data_length != 0) {
        size_t remaining =
            client->receive_capacity - client->receive_length;

        if (client->state == TCP_CLIENT_CLOSE_WAIT ||
            client->state == TCP_CLIENT_LAST_ACK ||
            data_length > remaining) {
            if (data_length > remaining) {
                fail_client(
                    client,
                    TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE);
                return TCP_CLIENT_RECEIVE_RESPONSE_TOO_LARGE;
            }
            client->malformed_segment_count++;
            return TCP_CLIENT_RECEIVE_MALFORMED;
        }
        copy_bytes(
            &client->receive_buffer[client->receive_length],
            &packet[header_length],
            data_length);
        client->receive_length += data_length;
        client->receive_next += (uint32_t)data_length;
        if ((flags & TCP_FLAG_FIN) == 0 &&
            !send_ack(client)) {
            return TCP_CLIENT_RECEIVE_ACCEPTED;
        }
    }

    if ((flags & TCP_FLAG_FIN) != 0) {
        client->receive_next++;
        if (!send_ack(client)) {
            return TCP_CLIENT_RECEIVE_ACCEPTED;
        }

        if (client->state == TCP_CLIENT_ESTABLISHED) {
            client->state = TCP_CLIENT_CLOSE_WAIT;
        } else if (client->state == TCP_CLIENT_FIN_WAIT_1) {
            client->state = TCP_CLIENT_CLOSING;
        } else if (client->state == TCP_CLIENT_FIN_WAIT_2) {
            client->state = TCP_CLIENT_CLOSED;
        }
    }

    return TCP_CLIENT_RECEIVE_ACCEPTED;
}

enum tcp_client_state tcp_client_get_state(
    const struct tcp_client* client) {
    if (client == NULL) {
        return TCP_CLIENT_FAILED;
    }
    return client->state;
}

enum tcp_client_error tcp_client_get_error(
    const struct tcp_client* client) {
    if (client == NULL) {
        return TCP_CLIENT_ERROR_INVALID_ARGUMENT;
    }
    return client->error;
}

enum tcp_client_operation tcp_client_get_operation(
    const struct tcp_client* client) {
    if (client == NULL) {
        return TCP_CLIENT_OPERATION_NONE;
    }
    return client->operation;
}

enum tcp_client_step_result tcp_client_get_step_result(
    const struct tcp_client* client) {
    if (client == NULL) {
        return TCP_CLIENT_STEP_FAILED;
    }
    if (client->state == TCP_CLIENT_FAILED) {
        return TCP_CLIENT_STEP_FAILED;
    }
    return client->step_result;
}

const char* tcp_client_error_string(enum tcp_client_error error) {
    switch (error) {
        case TCP_CLIENT_ERROR_NONE:
            return "no error";
        case TCP_CLIENT_ERROR_INVALID_ARGUMENT:
            return "invalid TCP client argument";
        case TCP_CLIENT_ERROR_BUSY:
            return "TCP client already has a connection";
        case TCP_CLIENT_ERROR_NOT_CONNECTED:
            return "TCP client is not connected";
        case TCP_CLIENT_ERROR_SEND_FAILED:
            return "IPv4 layer rejected a TCP segment";
        case TCP_CLIENT_ERROR_TIMEOUT:
            return "TCP operation timed out";
        case TCP_CLIENT_ERROR_RETRIES_EXHAUSTED:
            return "TCP retransmission limit reached";
        case TCP_CLIENT_ERROR_CLOCK_MOVED_BACKWARD:
            return "network clock moved backward";
        case TCP_CLIENT_ERROR_CLOCK_STALLED:
            return "network clock did not advance";
        case TCP_CLIENT_ERROR_RESET:
            return "peer reset the TCP connection";
        case TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE:
            return "TCP receive buffer is full";
        case TCP_CLIENT_ERROR_PROTOCOL:
            return "unsupported or inconsistent TCP state";
    }
    return "unknown TCP client error";
}

const uint8_t* tcp_client_received_data(
    const struct tcp_client* client) {
    if (client == NULL) {
        return NULL;
    }
    return client->receive_buffer;
}

size_t tcp_client_received_length(
    const struct tcp_client* client) {
    if (client == NULL) {
        return 0;
    }
    return client->receive_length;
}
