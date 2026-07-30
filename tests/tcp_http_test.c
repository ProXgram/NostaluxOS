#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "http_client.h"
#include "tcp_client.h"

#define LOCAL_IP  0x0A00020Fu
#define SERVER_IP 0x5DB8D822u
#define LOCAL_PORT 49152u
#define SERVER_PORT 80u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

enum fake_action {
    FAKE_ACTION_NONE = 0,
    FAKE_ACTION_SYN_ACK,
    FAKE_ACTION_DATA_ACK,
    FAKE_ACTION_FIN_ACK
};

struct fake_network {
    struct tcp_client* client;
    uint64_t now_ms;
    enum fake_action action;
    uint32_t client_next;
    uint32_t server_next;

    bool drop_all_syn;
    bool drop_first_syn;
    bool drop_first_data_ack;
    bool corrupt_response_once;
    bool corrupted_response_sent;
    bool send_out_of_order_once;
    bool out_of_order_sent;
    bool piggyback_response_on_ack;
    bool reset_on_data;
    bool peer_fin_on_active_close;
    bool request_acked;
    bool response_sent;

    const uint8_t* response;
    size_t response_length;
    uint8_t request[2048];
    size_t request_length;

    unsigned int syn_count;
    unsigned int data_count;
    unsigned int fin_count;
    unsigned int ack_count;
    unsigned int outbound_bad_checksum_count;
    unsigned int poll_count;
};

static uint16_t get_u16(const uint8_t* bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t get_u32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void put_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static bool fake_send_ipv4(
    void* context,
    uint32_t destination_ip,
    uint8_t protocol,
    const void* payload,
    size_t payload_length) {
    struct fake_network* network =
        (struct fake_network*)context;
    const uint8_t* packet = (const uint8_t*)payload;
    size_t header_length;
    size_t data_length;
    uint32_t sequence;
    uint8_t flags;

    assert(destination_ip == SERVER_IP);
    assert(protocol == TCP_CLIENT_IPV4_PROTOCOL);
    assert(payload != NULL);
    assert(payload_length >= 20);
    assert(get_u16(&packet[0]) == LOCAL_PORT);
    assert(get_u16(&packet[2]) == SERVER_PORT);
    if (tcp_client_checksum(
            LOCAL_IP,
            SERVER_IP,
            payload,
            payload_length) != 0) {
        network->outbound_bad_checksum_count++;
    }

    header_length = (size_t)(packet[12] >> 4) * 4u;
    assert(header_length >= 20);
    assert(header_length <= payload_length);
    data_length = payload_length - header_length;
    sequence = get_u32(&packet[4]);
    flags = packet[13];

    if ((flags & TCP_SYN) != 0) {
        network->syn_count++;
        if (network->syn_count == 1) {
            network->client_next = sequence + 1u;
        } else {
            assert(sequence + 1u == network->client_next);
        }
        if (!network->drop_all_syn &&
            !(network->drop_first_syn &&
              network->syn_count == 1)) {
            network->action = FAKE_ACTION_SYN_ACK;
        }
    } else if (data_length != 0) {
        network->data_count++;
        if (sequence == network->client_next) {
            assert(data_length <=
                   sizeof(network->request) -
                       network->request_length);
            memcpy(
                &network->request[network->request_length],
                &packet[header_length],
                data_length);
            network->request_length += data_length;
            network->client_next += (uint32_t)data_length;
        } else {
            assert(sequence != network->client_next);
        }
        if (!(network->drop_first_data_ack &&
              network->data_count == 1)) {
            network->action = FAKE_ACTION_DATA_ACK;
        }
    } else if ((flags & TCP_FIN) != 0) {
        network->fin_count++;
        if (sequence == network->client_next) {
            network->client_next++;
        }
        network->action = FAKE_ACTION_FIN_ACK;
    } else if ((flags & TCP_ACK) != 0) {
        network->ack_count++;
    }

    return true;
}

static uint64_t fake_now_ms(void* context) {
    return ((struct fake_network*)context)->now_ms;
}

static enum tcp_client_receive_result fake_emit(
    struct fake_network* network,
    uint32_t sequence,
    uint32_t acknowledgment,
    uint8_t flags,
    const uint8_t* data,
    size_t data_length,
    bool corrupt) {
    uint8_t packet[20 + 1024];
    uint16_t checksum;

    assert(data_length <= 1024);
    memset(packet, 0, 20);
    put_u16(&packet[0], SERVER_PORT);
    put_u16(&packet[2], LOCAL_PORT);
    put_u32(&packet[4], sequence);
    put_u32(&packet[8], acknowledgment);
    packet[12] = 5u << 4;
    packet[13] = flags;
    put_u16(&packet[14], 65535);
    if (data_length != 0) {
        memcpy(&packet[20], data, data_length);
    }
    checksum = tcp_client_checksum(
        SERVER_IP,
        LOCAL_IP,
        packet,
        20 + data_length);
    put_u16(&packet[16], checksum);
    if (corrupt) {
        packet[data_length != 0 ? 20 : 8] ^= 0x01u;
    }

    return tcp_client_receive(
        network->client,
        SERVER_IP,
        LOCAL_IP,
        packet,
        20 + data_length);
}

static void fake_poll(void* context) {
    struct fake_network* network =
        (struct fake_network*)context;
    enum fake_action action = network->action;

    network->poll_count++;
    network->now_ms += 10;
    network->action = FAKE_ACTION_NONE;
    if (action == FAKE_ACTION_SYN_ACK) {
        enum tcp_client_receive_result result = fake_emit(
            network,
            network->server_next,
            network->client_next,
            TCP_SYN | TCP_ACK,
            NULL,
            0,
            false);

        assert(result == TCP_CLIENT_RECEIVE_ACCEPTED);
        network->server_next++;
        return;
    }
    if (action == FAKE_ACTION_DATA_ACK) {
        enum tcp_client_receive_result result;

        if (network->reset_on_data) {
            result = fake_emit(
                network,
                network->server_next,
                network->client_next,
                TCP_RST | TCP_ACK,
                NULL,
                0,
                false);
            assert(result == TCP_CLIENT_RECEIVE_ACCEPTED);
            return;
        }
        if (network->piggyback_response_on_ack) {
            result = fake_emit(
                network,
                network->server_next,
                network->client_next,
                TCP_PSH | TCP_ACK | TCP_FIN,
                network->response,
                network->response_length,
                false);
            assert(result == TCP_CLIENT_RECEIVE_ACCEPTED);
            network->server_next +=
                (uint32_t)network->response_length + 1u;
            network->request_acked = true;
            network->response_sent = true;
            return;
        }
        result = fake_emit(
            network,
            network->server_next,
            network->client_next,
            TCP_ACK,
            NULL,
            0,
            false);
        assert(result == TCP_CLIENT_RECEIVE_ACCEPTED);
        network->request_acked = true;
        return;
    }
    if (action == FAKE_ACTION_FIN_ACK) {
        enum tcp_client_receive_result result = fake_emit(
            network,
            network->server_next,
            network->client_next,
            network->peer_fin_on_active_close
                ? TCP_FIN | TCP_ACK
                : TCP_ACK,
            NULL,
            0,
            false);

        assert(result == TCP_CLIENT_RECEIVE_ACCEPTED);
        if (network->peer_fin_on_active_close) {
            network->server_next++;
        }
        return;
    }

    if (network->request_acked && !network->response_sent) {
        enum tcp_client_receive_result result;

        if (network->send_out_of_order_once &&
            !network->out_of_order_sent) {
            result = fake_emit(
                network,
                network->server_next + 7u,
                network->client_next,
                TCP_PSH | TCP_ACK,
                network->response,
                network->response_length,
                false);
            assert(result == TCP_CLIENT_RECEIVE_ACCEPTED);
            network->out_of_order_sent = true;
            return;
        }
        if (network->corrupt_response_once &&
            !network->corrupted_response_sent) {
            result = fake_emit(
                network,
                network->server_next,
                network->client_next,
                TCP_PSH | TCP_ACK | TCP_FIN,
                network->response,
                network->response_length,
                true);
            assert(result == TCP_CLIENT_RECEIVE_BAD_CHECKSUM);
            network->corrupted_response_sent = true;
            return;
        }

        result = fake_emit(
            network,
            network->server_next,
            network->client_next,
            TCP_PSH | TCP_ACK | TCP_FIN,
            network->response,
            network->response_length,
            false);
        if (result == TCP_CLIENT_RECEIVE_ACCEPTED) {
            network->server_next +=
                (uint32_t)network->response_length + 1u;
        }
        network->response_sent = true;
    }
}

static void initialize_client(
    struct tcp_client* client,
    struct fake_network* network,
    uint8_t* response_buffer,
    size_t response_capacity) {
    const struct tcp_client_io io = {
        .send_ipv4 = fake_send_ipv4,
        .now_ms = fake_now_ms,
        .poll = fake_poll,
        .context = network,
    };

    memset(network, 0, sizeof(*network));
    network->client = client;
    network->server_next = 700000u;
    tcp_client_init(
        client,
        &io,
        LOCAL_IP,
        LOCAL_PORT,
        response_buffer,
        response_capacity);
    assert(tcp_client_set_timeouts(client, 20, 200, 3));
}

static bool contains_bytes(
    const uint8_t* haystack,
    size_t haystack_length,
    const char* needle) {
    size_t needle_length = strlen(needle);

    if (needle_length > haystack_length) {
        return false;
    }
    for (size_t i = 0;
         i <= haystack_length - needle_length;
         i++) {
        if (memcmp(&haystack[i], needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void test_http_get_with_retransmission_and_corruption(void) {
    static const uint8_t wire_response[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "X-Test: yes\r\n\r\n"
        "hello";
    struct tcp_client client;
    struct fake_network network;
    struct http_response response;
    uint8_t receive_buffer[512];
    uint8_t request_buffer[256];
    const uint8_t* header_value;
    size_t header_length;
    enum http_client_error result;

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    network.drop_first_syn = true;
    network.drop_first_data_ack = true;
    network.corrupt_response_once = true;
    network.send_out_of_order_once = true;
    network.response = wire_response;
    network.response_length = sizeof(wire_response) - 1u;

    result = http_client_get(
        &client,
        SERVER_IP,
        SERVER_PORT,
        0xFFFFFFFEu,
        "example.com",
        "/index.html",
        request_buffer,
        sizeof(request_buffer),
        &response);
    assert(result == HTTP_CLIENT_ERROR_NONE);
    assert(tcp_client_get_state(&client) == TCP_CLIENT_CLOSED);
    assert(client.bad_checksum_count == 1);
    assert(network.outbound_bad_checksum_count == 0);
    assert(network.syn_count == 2);
    assert(network.data_count == 2);
    assert(network.fin_count == 1);
    assert(network.out_of_order_sent);
    assert(response.status_code == 200);
    assert(response.body_length == 5);
    assert(memcmp(response.body, "hello", 5) == 0);
    assert(response.has_content_length);
    assert(response.content_length == 5);
    assert(response.connection_close);
    assert(http_response_find_header(
        &response,
        "x-test",
        &header_value,
        &header_length));
    assert(header_length == 3);
    assert(memcmp(header_value, "yes", 3) == 0);
    assert(contains_bytes(
        network.request,
        network.request_length,
        "GET /index.html HTTP/1.1\r\n"));
    assert(contains_bytes(
        network.request,
        network.request_length,
        "Host: example.com\r\n"));
    assert(contains_bytes(
        network.request,
        network.request_length,
        "Connection: close\r\n"));
}

static void test_checksum_known_vector(void) {
    static const uint8_t header_without_checksum[20] = {
        0xC0,0x00, 0x00,0x50, 0x11,0x22,0x33,0x44,
        0x00,0x00,0x00,0x00, 0x50,0x02, 0xFF,0xFF,
        0x00,0x00, 0x00,0x00
    };
    uint8_t packet[sizeof(header_without_checksum)];
    uint16_t checksum;

    memcpy(packet, header_without_checksum, sizeof(packet));
    checksum = tcp_client_checksum(
        LOCAL_IP,
        SERVER_IP,
        packet,
        sizeof(packet));
    assert(checksum == 0x6942u);
    put_u16(&packet[16], checksum);
    assert(tcp_client_checksum(
        LOCAL_IP,
        SERVER_IP,
        packet,
        sizeof(packet)) == 0);
    packet[4] ^= 0x01u;
    assert(tcp_client_checksum(
        LOCAL_IP,
        SERVER_IP,
        packet,
        sizeof(packet)) != 0);
}

static void test_retries_exhausted(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[32];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    assert(tcp_client_set_timeouts(&client, 20, 200, 2));
    network.drop_all_syn = true;
    assert(!tcp_client_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        123u));
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_RETRIES_EXHAUSTED);
    assert(network.syn_count == 3);
}

static void test_response_can_piggyback_final_send_ack(void) {
    static const uint8_t wire_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "2\r\nok\r\n0\r\n\r\n";
    struct tcp_client client;
    struct fake_network network;
    struct http_response response;
    uint8_t receive_buffer[128];
    uint8_t request_buffer[128];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    network.response = wire_response;
    network.response_length = sizeof(wire_response) - 1u;
    network.piggyback_response_on_ack = true;

    assert(http_client_get(
        &client,
        SERVER_IP,
        SERVER_PORT,
        456u,
        "example.com",
        "/",
        request_buffer,
        sizeof(request_buffer),
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.body_length == 2);
    assert(memcmp(response.body, "ok", 2) == 0);
    assert(response.chunked_transfer);
}

static void test_reset_is_not_mistaken_for_an_ack(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[64];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    network.reset_on_data = true;
    assert(tcp_client_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        321u));
    assert(!tcp_client_send(&client, "x", 1));
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_RESET);
    assert(tcp_client_get_state(&client) ==
           TCP_CLIENT_FAILED);
}

static void test_active_fin_handshake(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[64];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    network.peer_fin_on_active_close = true;
    assert(tcp_client_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        999u));
    assert(tcp_client_close(&client));
    assert(tcp_client_get_state(&client) == TCP_CLIENT_CLOSED);
    assert(network.fin_count == 1);
}

static void test_async_lifecycle_never_polls(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[128];
    enum tcp_client_step_result result;
    unsigned int polls;

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    client.io.poll = NULL;
    assert(tcp_client_get_operation(&client) ==
           TCP_CLIENT_OPERATION_NONE);
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(network.poll_count == 0);
    assert(tcp_client_begin_connect(
        &client,
        0,
        SERVER_PORT,
        0x12345678u) == TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_INVALID_ARGUMENT);
    assert(tcp_client_begin_send(&client, "x", 1) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_NOT_CONNECTED);

    result = tcp_client_begin_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        0x12345678u);
    assert(result == TCP_CLIENT_STEP_PENDING);
    assert(tcp_client_get_operation(&client) ==
           TCP_CLIENT_OPERATION_CONNECT);
    assert(tcp_client_get_step_result(&client) ==
           TCP_CLIENT_STEP_PENDING);
    assert(network.syn_count == 1);
    assert(tcp_client_begin_send(&client, "busy", 4) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_BUSY);
    assert(tcp_client_get_operation(&client) ==
           TCP_CLIENT_OPERATION_CONNECT);
    assert(tcp_client_get_step_result(&client) ==
           TCP_CLIENT_STEP_PENDING);

    polls = network.poll_count;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_PENDING);
    assert(network.poll_count == polls);
    fake_poll(&network);
    polls = network.poll_count;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(network.poll_count == polls);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_NONE);
    assert(tcp_client_get_state(&client) ==
           TCP_CLIENT_ESTABLISHED);

    assert(tcp_client_begin_send(
        &client,
        receive_buffer,
        TCP_CLIENT_MAX_SEGMENT_DATA + 1u) ==
        TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_INVALID_ARGUMENT);
    assert(tcp_client_begin_send(&client, NULL, 0) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(tcp_client_begin_send(&client, "abc", 3) ==
           TCP_CLIENT_STEP_PENDING);
    assert(network.data_count == 1);
    assert(tcp_client_begin_wait_for_peer_close(&client) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_BUSY);
    assert(tcp_client_get_operation(&client) ==
           TCP_CLIENT_OPERATION_SEND);

    fake_poll(&network);
    polls = network.poll_count;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(network.poll_count == polls);
    assert(network.request_length == 3);
    assert(memcmp(network.request, "abc", 3) == 0);

    assert(tcp_client_begin_wait_for_peer_close(&client) ==
           TCP_CLIENT_STEP_PENDING);
    assert(fake_emit(
        &network,
        network.server_next,
        network.client_next,
        TCP_FIN | TCP_ACK,
        NULL,
        0,
        false) == TCP_CLIENT_RECEIVE_ACCEPTED);
    network.server_next++;
    polls = network.poll_count;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(network.poll_count == polls);
    assert(tcp_client_get_state(&client) ==
           TCP_CLIENT_CLOSE_WAIT);

    assert(tcp_client_begin_close(&client) ==
           TCP_CLIENT_STEP_PENDING);
    assert(tcp_client_get_operation(&client) ==
           TCP_CLIENT_OPERATION_CLOSE);
    fake_poll(&network);
    polls = network.poll_count;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(network.poll_count == polls);
    assert(tcp_client_get_state(&client) ==
           TCP_CLIENT_CLOSED);
    assert(tcp_client_begin_close(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
}

static void test_wrong_sequence_ack_does_not_complete_send(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[64];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    assert(tcp_client_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        0xABCDEF01u));
    assert(tcp_client_begin_send(&client, "x", 1) ==
           TCP_CLIENT_STEP_PENDING);
    assert(client.pending_active);

    /*
     * The ACK number covers the pending byte, but the peer sequence is
     * outside the one-segment receive window. It must not mutate send state.
     */
    assert(fake_emit(
        &network,
        network.server_next + 7u,
        network.client_next,
        TCP_ACK,
        NULL,
        0,
        false) == TCP_CLIENT_RECEIVE_ACCEPTED);
    assert(client.pending_active);
    assert(tcp_client_get_step_result(&client) ==
           TCP_CLIENT_STEP_PENDING);

    fake_poll(&network);
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_COMPLETE);
    assert(!client.pending_active);
}

static void test_async_retransmit_and_retry_limit(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[32];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    client.io.poll = NULL;
    assert(tcp_client_set_timeouts(&client, 20, 200, 1));
    network.drop_all_syn = true;
    assert(tcp_client_begin_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        123u) == TCP_CLIENT_STEP_PENDING);
    network.now_ms = 19;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_PENDING);
    assert(network.syn_count == 1);
    network.now_ms = 20;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_PENDING);
    assert(network.syn_count == 2);
    network.now_ms = 40;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_RETRIES_EXHAUSTED);
    assert(network.poll_count == 0);
}

static void test_async_timeout_and_clock_errors(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[32];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    client.io.poll = NULL;
    assert(tcp_client_set_timeouts(&client, 100, 100, 4));
    network.drop_all_syn = true;
    assert(tcp_client_begin_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        1u) == TCP_CLIENT_STEP_PENDING);
    network.now_ms = 100;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_TIMEOUT);

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    client.io.poll = NULL;
    network.now_ms = 100;
    network.drop_all_syn = true;
    assert(tcp_client_begin_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        2u) == TCP_CLIENT_STEP_PENDING);
    network.now_ms = 99;
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_CLOCK_MOVED_BACKWARD);
}

static void test_async_clock_stall(void) {
    struct tcp_client client;
    struct fake_network network;
    uint8_t receive_buffer[32];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    client.io.poll = NULL;
    network.drop_all_syn = true;
    assert(tcp_client_begin_connect(
        &client,
        SERVER_IP,
        SERVER_PORT,
        3u) == TCP_CLIENT_STEP_PENDING);

    for (unsigned int i = 1;
         i < TCP_CLIENT_CLOCK_STALL_SERVICE_LIMIT;
         i++) {
        assert(tcp_client_service(&client) ==
               TCP_CLIENT_STEP_PENDING);
    }
    assert(tcp_client_service(&client) ==
           TCP_CLIENT_STEP_FAILED);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_CLOCK_STALLED);
    assert(network.poll_count == 0);
}

static void test_response_overflow_is_not_truncated(void) {
    static const uint8_t wire_response[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\ndata";
    struct tcp_client client;
    struct fake_network network;
    struct http_response response;
    uint8_t receive_buffer[16];
    uint8_t request_buffer[128];

    initialize_client(
        &client,
        &network,
        receive_buffer,
        sizeof(receive_buffer));
    network.response = wire_response;
    network.response_length = sizeof(wire_response) - 1u;

    assert(http_client_get(
        &client,
        SERVER_IP,
        SERVER_PORT,
        777u,
        "example.com",
        "/",
        request_buffer,
        sizeof(request_buffer),
        &response) == HTTP_CLIENT_ERROR_TCP);
    assert(tcp_client_get_error(&client) ==
           TCP_CLIENT_ERROR_RESPONSE_TOO_LARGE);
    assert(tcp_client_received_length(&client) == 0);
}

static void test_request_builder_rejects_injection(void) {
    static const char expected_request[] =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "User-Agent: Nostalux/1.0\r\n"
        "Accept: */*\r\n\r\n";
    uint8_t request[256];
    size_t length = 123;

    assert(http_client_build_get(
        "example.com",
        "/",
        request,
        sizeof(request),
        &length) == HTTP_CLIENT_ERROR_NONE);
    assert(length == sizeof(expected_request) - 1u);
    assert(memcmp(
        request,
        expected_request,
        sizeof(expected_request) - 1u) == 0);

    assert(http_client_build_get(
        "example.com\r\nX-Evil: yes",
        "/",
        request,
        sizeof(request),
        &length) == HTTP_CLIENT_ERROR_INVALID_HOST);
    assert(length == 0);
    assert(http_client_build_get(
        "example.com",
        "/\r\nX-Evil: yes",
        request,
        sizeof(request),
        &length) == HTTP_CLIENT_ERROR_INVALID_PATH);
    assert(http_client_build_get(
        "example.com",
        "/",
        request,
        4,
        &length) == HTTP_CLIENT_ERROR_REQUEST_TOO_LARGE);
}

static void test_chunked_decoding(void) {
    struct http_response response;
    uint8_t chunked[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "X-Test: retained\r\n\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";
    uint8_t extensions_and_trailer[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: ChUnKeD\r\n\r\n"
        "4 ; foo=bar; flag; quoted=\"a b\"\r\nWiki\r\n"
        "5;second=yes\r\npedia\r\n"
        "0;done=yes\r\nX-Trace: accepted\r\n\r\n";
    uint8_t identity[] =
        "HTTP/1.0 200 OK\r\n"
        "Transfer-Encoding: identity\r\n\r\nplain";
    uint8_t binary_chunked[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3\r\n"
        "\x00\x01\xff"
        "\r\n0\r\n\r\n";
    const uint8_t* value;
    size_t value_length;

    assert(http_client_parse_response(
        chunked,
        sizeof(chunked) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.has_transfer_encoding);
    assert(response.chunked_transfer);
    assert(response.body_length == 9);
    assert(memcmp(response.body, "Wikipedia", 9) == 0);
    assert(http_response_find_header(
        &response, "x-test", &value, &value_length));
    assert(value_length == 8);
    assert(memcmp(value, "retained", 8) == 0);

    assert(http_client_parse_response(
        extensions_and_trailer,
        sizeof(extensions_and_trailer) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.chunked_transfer);
    assert(response.body_length == 9);
    assert(memcmp(response.body, "Wikipedia", 9) == 0);

    assert(http_client_parse_response(
        identity,
        sizeof(identity) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.has_transfer_encoding);
    assert(!response.chunked_transfer);
    assert(response.body_length == 5);
    assert(memcmp(response.body, "plain", 5) == 0);

    assert(http_client_parse_response(
        binary_chunked,
        sizeof(binary_chunked) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.body_length == 3);
    assert(response.body[0] == 0);
    assert(response.body[1] == 1);
    assert(response.body[2] == 0xFF);
}

static void test_chunked_framing_failures(void) {
    struct http_response response;
    uint8_t content_length_and_chunked[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\ntest\r\n0\r\n\r\n";
    uint8_t unsupported_coding[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "0\r\n\r\n";
    uint8_t chunked_not_final[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked, gzip\r\n\r\n"
        "0\r\n\r\n";
    uint8_t duplicate_chunked[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n";
    uint8_t parameterized_chunked[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked; mode=strict\r\n\r\n"
        "0\r\n\r\n";
    uint8_t malformed_extension[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4;=bad\r\ntest\r\n0\r\n\r\n";
    uint8_t trailing_chunk_whitespace[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4 \r\ntest\r\n0\r\n\r\n";
    uint8_t overflowing_chunk_size[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\n";
    uint8_t truncated_data[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5\r\ntest";
    uint8_t bad_data_terminator[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\ntestXX0\r\n\r\n";
    uint8_t forbidden_trailer[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\nContent-Length: 1\r\n\r\n";
    uint8_t extra_after_end[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\nextra";
    uint8_t original[sizeof(malformed_extension)];

    assert(http_client_parse_response(
        content_length_and_chunked,
        sizeof(content_length_and_chunked) - 1u,
        &response) == HTTP_CLIENT_ERROR_CONFLICTING_FRAMING);
    assert(http_client_parse_response(
        unsupported_coding,
        sizeof(unsupported_coding) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_UNSUPPORTED_TRANSFER_ENCODING);
    assert(http_client_parse_response(
        chunked_not_final,
        sizeof(chunked_not_final) - 1u,
        &response) == HTTP_CLIENT_ERROR_MALFORMED_HEADER);
    assert(http_client_parse_response(
        duplicate_chunked,
        sizeof(duplicate_chunked) - 1u,
        &response) == HTTP_CLIENT_ERROR_MALFORMED_HEADER);
    assert(http_client_parse_response(
        parameterized_chunked,
        sizeof(parameterized_chunked) - 1u,
        &response) == HTTP_CLIENT_ERROR_MALFORMED_HEADER);

    memcpy(original, malformed_extension, sizeof(original));
    assert(http_client_parse_response(
        malformed_extension,
        sizeof(malformed_extension) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY);
    assert(memcmp(
        malformed_extension,
        original,
        sizeof(original)) == 0);
    assert(http_client_parse_response(
        trailing_chunk_whitespace,
        sizeof(trailing_chunk_whitespace) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY);
    assert(http_client_parse_response(
        overflowing_chunk_size,
        sizeof(overflowing_chunk_size) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY);
    assert(http_client_parse_response(
        truncated_data,
        sizeof(truncated_data) - 1u,
        &response) == HTTP_CLIENT_ERROR_TRUNCATED_BODY);
    assert(http_client_parse_response(
        bad_data_terminator,
        sizeof(bad_data_terminator) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY);
    assert(http_client_parse_response(
        forbidden_trailer,
        sizeof(forbidden_trailer) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_CHUNKED_BODY);
    assert(http_client_parse_response(
        extra_after_end,
        sizeof(extra_after_end) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA);
}

static void test_chunk_metadata_bounds(void) {
    static const char prefix[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    enum {
        EXCESSIVE_CHUNK_COUNT =
            HTTP_CLIENT_MAX_CHUNK_FRAMING_BYTES / 5u + 1u,
    };
    struct http_response response;
    uint8_t long_chunk_line[
        sizeof(prefix) - 1u +
        HTTP_CLIENT_MAX_CHUNK_LINE_BYTES + 32u];
    uint8_t long_trailer[
        sizeof(prefix) - 1u + 3u +
        HTTP_CLIENT_MAX_TRAILER_BYTES + 8u];
    uint8_t excessive_framing[
        sizeof(prefix) - 1u + EXCESSIVE_CHUNK_COUNT * 6u + 5u];
    size_t cursor = 0;

    memcpy(long_chunk_line, prefix, sizeof(prefix) - 1u);
    cursor = sizeof(prefix) - 1u;
    long_chunk_line[cursor++] = '1';
    long_chunk_line[cursor++] = ';';
    memset(
        &long_chunk_line[cursor],
        'a',
        HTTP_CLIENT_MAX_CHUNK_LINE_BYTES + 1u);
    cursor += HTTP_CLIENT_MAX_CHUNK_LINE_BYTES + 1u;
    memcpy(&long_chunk_line[cursor], "\r\nx\r\n0\r\n\r\n", 10);
    cursor += 10;
    assert(http_client_parse_response(
        long_chunk_line,
        cursor,
        &response) ==
        HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE);

    memcpy(long_trailer, prefix, sizeof(prefix) - 1u);
    cursor = sizeof(prefix) - 1u;
    memcpy(&long_trailer[cursor], "0\r\nX:", 5);
    cursor += 5;
    memset(
        &long_trailer[cursor],
        'a',
        HTTP_CLIENT_MAX_TRAILER_BYTES);
    cursor += HTTP_CLIENT_MAX_TRAILER_BYTES;
    memcpy(&long_trailer[cursor], "\r\n\r\n", 4);
    cursor += 4;
    assert(http_client_parse_response(
        long_trailer,
        cursor,
        &response) ==
        HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE);

    memcpy(excessive_framing, prefix, sizeof(prefix) - 1u);
    cursor = sizeof(prefix) - 1u;
    for (size_t index = 0;
         index < EXCESSIVE_CHUNK_COUNT; index++) {
        memcpy(&excessive_framing[cursor], "1\r\nx\r\n", 6u);
        cursor += 6u;
    }
    memcpy(&excessive_framing[cursor], "0\r\n\r\n", 5u);
    cursor += 5u;
    assert(cursor == sizeof(excessive_framing));
    assert(http_client_parse_response(
        excessive_framing,
        cursor,
        &response) ==
        HTTP_CLIENT_ERROR_CHUNK_METADATA_TOO_LARGE);
}

static void test_redirect_metadata(void) {
    struct http_response response;
    uint8_t absolute[] =
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://example.com/new%20path?q=1\r\n"
        "Content-Length: 0\r\n\r\n";
    uint8_t relative[] =
        "HTTP/1.1 307 Temporary Redirect\r\n"
        "Location: ../next?x=1#part\r\n"
        "Content-Length: 0\r\n\r\n";
    uint8_t missing[] =
        "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n";
    uint8_t non_redirect[] =
        "HTTP/1.1 200 OK\r\n"
        "Location: /metadata-only\r\n"
        "Content-Length: 0\r\n\r\n";
    uint8_t invalid[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: /bad%2\r\n"
        "Content-Length: 0\r\n\r\n";
    uint8_t duplicate[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: /one\r\n"
        "Location: /two\r\n"
        "Content-Length: 0\r\n\r\n";
    const uint8_t* location;
    size_t location_length;
    char copied[64];

    assert(http_client_parse_response(
        absolute,
        sizeof(absolute) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(http_response_is_redirect(&response));
    assert(http_response_redirect_location(
        &response, &location, &location_length));
    assert(location_length ==
           strlen("https://example.com/new%20path?q=1"));
    assert(memcmp(
        location,
        "https://example.com/new%20path?q=1",
        location_length) == 0);
    assert(http_response_copy_redirect_location(
        &response,
        copied,
        sizeof(copied),
        &location_length) == HTTP_CLIENT_ERROR_NONE);
    assert(strcmp(
        copied,
        "https://example.com/new%20path?q=1") == 0);
    assert(http_response_copy_redirect_location(
        &response,
        copied,
        8,
        &location_length) ==
        HTTP_CLIENT_ERROR_REDIRECT_LOCATION_TOO_LARGE);
    assert(copied[0] == '\0');

    assert(http_client_parse_response(
        relative,
        sizeof(relative) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(http_response_redirect_location(
        &response, &location, &location_length));
    assert(location_length == strlen("../next?x=1#part"));

    assert(http_client_parse_response(
        missing,
        sizeof(missing) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(http_response_is_redirect(&response));
    assert(!http_response_redirect_location(
        &response, &location, &location_length));
    assert(http_response_copy_redirect_location(
        &response,
        copied,
        sizeof(copied),
        &location_length) ==
        HTTP_CLIENT_ERROR_REDIRECT_LOCATION_MISSING);

    assert(http_client_parse_response(
        non_redirect,
        sizeof(non_redirect) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(!http_response_is_redirect(&response));
    assert(response.has_location);
    assert(!http_response_redirect_location(
        &response, &location, &location_length));
    assert(http_client_parse_response(
        invalid,
        sizeof(invalid) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_INVALID_REDIRECT_LOCATION);
    assert(http_client_parse_response(
        duplicate,
        sizeof(duplicate) - 1u,
        &response) == HTTP_CLIENT_ERROR_MALFORMED_HEADER);
}

static void test_informational_response_chains(void) {
    struct http_response response;
    const uint8_t* value;
    size_t value_length;
    uint8_t continue_then_final[] =
        "HTTP/1.1 100 Continue\r\n"
        "X-Interim: ignored\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n"
        "X-Final: retained\r\n\r\n"
        "data";
    uint8_t early_hints_then_continue[] =
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </style.css>; rel=preload\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\ntest\r\n0\r\n\r\n";
    uint8_t protocol_switch[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n\r\n";
    uint8_t content_length_on_interim[] =
        "HTTP/1.1 100 Continue\r\n"
        "Content-Length: 0\r\n\r\n"
        "HTTP/1.1 204 No Content\r\n\r\n";
    uint8_t transfer_encoding_on_interim[] =
        "HTTP/1.1 103 Early Hints\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    uint8_t incomplete_interim[] =
        "HTTP/1.1 103 Early Hints\r\n\r\n";
    uint8_t body_before_final[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "not-a-response"
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    uint8_t malformed_interim_header[] =
        "HTTP/1.1 103 Early Hints\r\n"
        "Bad Header: rejected\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    static const uint8_t interim[] =
        "HTTP/1.1 103 Early Hints\r\n\r\n";
    static const uint8_t final[] =
        "HTTP/1.1 204 No Content\r\n\r\n";
    uint8_t too_many[
        (HTTP_CLIENT_MAX_INFORMATIONAL_RESPONSES + 1u) *
            (sizeof(interim) - 1u) +
        sizeof(final) - 1u];
    size_t cursor = 0;

    assert(http_client_parse_response(
        continue_then_final,
        sizeof(continue_then_final) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.status_code == 200u);
    assert(response.body_length == 4u);
    assert(memcmp(response.body, "data", 4u) == 0);
    assert(http_response_find_header(
        &response, "x-final", &value, &value_length));
    assert(value_length == 8u);
    assert(memcmp(value, "retained", 8u) == 0);
    assert(!http_response_find_header(
        &response, "x-interim", &value, &value_length));

    assert(http_client_parse_response(
        early_hints_then_continue,
        sizeof(early_hints_then_continue) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.status_code == 200u);
    assert(response.chunked_transfer);
    assert(response.body_length == 4u);
    assert(memcmp(response.body, "test", 4u) == 0);

    assert(http_client_parse_response(
        protocol_switch,
        sizeof(protocol_switch) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_UNSUPPORTED_PROTOCOL_SWITCH);
    assert(http_client_parse_response(
        content_length_on_interim,
        sizeof(content_length_on_interim) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_INVALID_INFORMATIONAL_FRAMING);
    assert(http_client_parse_response(
        transfer_encoding_on_interim,
        sizeof(transfer_encoding_on_interim) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_INVALID_INFORMATIONAL_FRAMING);
    assert(http_client_parse_response(
        incomplete_interim,
        sizeof(incomplete_interim) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_INFORMATIONAL_RESPONSE_WITHOUT_FINAL);
    assert(http_client_parse_response(
        body_before_final,
        sizeof(body_before_final) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE);
    assert(http_client_parse_response(
        malformed_interim_header,
        sizeof(malformed_interim_header) - 1u,
        &response) == HTTP_CLIENT_ERROR_MALFORMED_HEADER);

    for (size_t index = 0;
         index < HTTP_CLIENT_MAX_INFORMATIONAL_RESPONSES + 1u;
         index++) {
        memcpy(
            &too_many[cursor], interim, sizeof(interim) - 1u);
        cursor += sizeof(interim) - 1u;
    }
    memcpy(&too_many[cursor], final, sizeof(final) - 1u);
    cursor += sizeof(final) - 1u;
    assert(cursor == sizeof(too_many));
    assert(http_client_parse_response(
        too_many,
        sizeof(too_many),
        &response) ==
        HTTP_CLIENT_ERROR_TOO_MANY_INFORMATIONAL_RESPONSES);
}

static void test_response_body_progress(void) {
    static const uint8_t partial_plain[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n\r\n"
        "da";
    static const uint8_t informational_then_partial[] =
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </style.css>; rel=preload\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n\r\n"
        "da";
    static const uint8_t chunk_metadata_without_body[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4;progress-metadata-that-is-not-body";
    static const uint8_t identity_partial[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: identity\r\n\r\n"
        "abc";
    static const uint8_t content_length_overrun[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n\r\n"
        "abcd";
    static const uint8_t no_content_with_extra_bytes[] =
        "HTTP/1.1 204 No Content\r\n\r\n"
        "not-body";
    static const uint8_t invalid_informational_framing[] =
        "HTTP/1.1 100 Continue\r\n"
        "Content-Length: 0\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n\r\n"
        "data";
    static const uint8_t incomplete_final_header[] =
        "HTTP/1.1 103 Early Hints\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n";

    assert(http_client_response_body_progress(
        partial_plain, sizeof(partial_plain) - 1u, 64u) == 2u);
    assert(http_client_response_body_progress(
        partial_plain, sizeof(partial_plain) - 1u, 1u) == 1u);
    assert(http_client_response_body_progress(
        informational_then_partial,
        sizeof(informational_then_partial) - 1u,
        64u) == 2u);

    /*
     * Chunk framing can be much larger than the decoded body. It must never
     * be presented to applications as received body-byte progress.
     */
    assert(http_client_response_body_progress(
        chunk_metadata_without_body,
        sizeof(chunk_metadata_without_body) - 1u,
        64u) == 0u);
    assert(http_client_response_body_progress(
        identity_partial,
        sizeof(identity_partial) - 1u,
        64u) == 3u);
    assert(http_client_response_body_progress(
        content_length_overrun,
        sizeof(content_length_overrun) - 1u,
        64u) == 2u);
    assert(http_client_response_body_progress(
        no_content_with_extra_bytes,
        sizeof(no_content_with_extra_bytes) - 1u,
        64u) == 0u);
    assert(http_client_response_body_progress(
        invalid_informational_framing,
        sizeof(invalid_informational_framing) - 1u,
        64u) == 0u);
    assert(http_client_response_body_progress(
        incomplete_final_header,
        sizeof(incomplete_final_header) - 1u,
        64u) == 0u);
    assert(http_client_response_body_progress(
        NULL, 0u, 64u) == 0u);
    assert(http_client_response_body_progress(
        partial_plain, sizeof(partial_plain) - 1u, 0u) == 0u);
}

static void test_http_parser_failures(void) {
    struct http_response response;
    uint8_t conflicting_length[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Content-Length: 3\r\n\r\nabc";
    uint8_t truncated[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nabc";
    uint8_t extra[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nabc";
    uint8_t close_delimited[] =
        "HTTP/1.0 404 Not Found\r\n\r\nmissing";
    uint8_t invalid_status[] =
        "HTTP/1.0 700 Impossible\r\n\r\n";
    uint8_t binary[] = {
        'H','T','T','P','/','1','.','0',' ','2','0','0',' ','O','K',
        '\r','\n','\r','\n',0x00,0x01,0xFF
    };

    assert(http_client_parse_response(
        conflicting_length,
        sizeof(conflicting_length) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_INVALID_CONTENT_LENGTH);
    assert(http_client_parse_response(
        truncated,
        sizeof(truncated) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_TRUNCATED_BODY);
    assert(http_client_parse_response(
        extra,
        sizeof(extra) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_UNEXPECTED_EXTRA_DATA);
    assert(http_client_parse_response(
        close_delimited,
        sizeof(close_delimited) - 1u,
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.status_code == 404);
    assert(response.body_length == 7);
    assert(memcmp(response.body, "missing", 7) == 0);
    assert(http_client_parse_response(
        invalid_status,
        sizeof(invalid_status) - 1u,
        &response) ==
        HTTP_CLIENT_ERROR_MALFORMED_STATUS_LINE);
    assert(http_client_parse_response(
        binary,
        sizeof(binary),
        &response) == HTTP_CLIENT_ERROR_NONE);
    assert(response.body_length == 3);
    assert(response.body[0] == 0);
    assert(response.body[2] == 0xFF);
}

static void test_parser_random_inputs_are_bounded(void) {
    struct http_response response;
    uint8_t bytes[256];
    static const uint8_t structured_seed[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3;name=value\r\nabc\r\n"
        "2\r\nde\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n";
    static const uint8_t informational_seed[] =
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </one>; rel=preload\r\n\r\n"
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n\r\n"
        "data";
    uint8_t structured[sizeof(structured_seed)];
    uint8_t informational[sizeof(informational_seed)];
    uint32_t random = 0xC0FFEE11u;

    for (size_t attempt = 0; attempt < 5000; attempt++) {
        size_t length;

        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        length = random % sizeof(bytes);
        for (size_t i = 0; i < length; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            bytes[i] = (uint8_t)random;
        }
        (void)http_client_parse_response(
            bytes,
            length,
            &response);
    }

    for (size_t length = 1;
         length < sizeof(structured_seed);
         length++) {
        memcpy(structured, structured_seed, sizeof(structured));
        (void)http_client_parse_response(
            structured,
            length,
            &response);
    }
    for (size_t attempt = 0; attempt < 5000; attempt++) {
        memcpy(structured, structured_seed, sizeof(structured));
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        size_t mutations = 1u + random % 4u;

        for (size_t i = 0; i < mutations; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            size_t offset =
                random % (sizeof(structured_seed) - 1u);
            structured[offset] ^= (uint8_t)(1u << (random & 7u));
        }
        (void)http_client_parse_response(
            structured,
            sizeof(structured_seed) - 1u,
            &response);
    }

    for (size_t length = 1;
         length < sizeof(informational_seed);
         length++) {
        memcpy(
            informational,
            informational_seed,
            sizeof(informational));
        (void)http_client_parse_response(
            informational,
            length,
            &response);
    }
    for (size_t attempt = 0; attempt < 2500; attempt++) {
        memcpy(
            informational,
            informational_seed,
            sizeof(informational));
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        size_t mutations = 1u + random % 4u;

        for (size_t i = 0; i < mutations; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            size_t offset =
                random % (sizeof(informational_seed) - 1u);
            informational[offset] ^=
                (uint8_t)(1u << (random & 7u));
        }
        (void)http_client_parse_response(
            informational,
            sizeof(informational_seed) - 1u,
            &response);
    }
}

int main(void) {
    test_checksum_known_vector();
    test_http_get_with_retransmission_and_corruption();
    test_retries_exhausted();
    test_response_can_piggyback_final_send_ack();
    test_reset_is_not_mistaken_for_an_ack();
    test_active_fin_handshake();
    test_async_lifecycle_never_polls();
    test_wrong_sequence_ack_does_not_complete_send();
    test_async_retransmit_and_retry_limit();
    test_async_timeout_and_clock_errors();
    test_async_clock_stall();
    test_response_overflow_is_not_truncated();
    test_request_builder_rejects_injection();
    test_chunked_decoding();
    test_chunked_framing_failures();
    test_chunk_metadata_bounds();
    test_redirect_metadata();
    test_informational_response_chains();
    test_response_body_progress();
    test_http_parser_failures();
    test_parser_random_inputs_are_bounded();
    puts("tcp_http_test: all tests passed");
    return 0;
}
