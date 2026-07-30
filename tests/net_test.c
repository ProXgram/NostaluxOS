#include "net.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_FRAME_COUNT 16u

struct test_frame {
    size_t size;
    uint8_t bytes[NET_ETHERNET_FRAME_MAX];
};

struct fake_link {
    struct test_frame transmitted[TEST_FRAME_COUNT];
    size_t transmitted_count;
    size_t send_attempt_count;
    uint16_t last_attempt_ethertype;
    struct test_frame received[TEST_FRAME_COUNT];
    size_t received_count;
    size_t received_index;
    uint64_t now_ms;
    bool reject_send;
};

struct udp_capture {
    unsigned int calls;
    uint32_t source_ip;
    uint16_t source_port;
    size_t data_size;
    uint8_t data[64];
};

struct ipv4_capture {
    unsigned int calls;
    uint32_t source_ip;
    uint32_t destination_ip;
    size_t payload_size;
    uint8_t payload[64];
};

static const uint8_t LOCAL_MAC[NET_ETHERNET_ADDRESS_SIZE] = {
    0x52u, 0x54u, 0x00u, 0x12u, 0x34u, 0x56u,
};
static const uint8_t PEER_MAC[NET_ETHERNET_ADDRESS_SIZE] = {
    0x52u, 0x54u, 0x00u, 0xABu, 0xCDu, 0xEFu,
};
static const uint8_t DNS_MAC[NET_ETHERNET_ADDRESS_SIZE] = {
    0x52u, 0x54u, 0x00u, 0x00u, 0x00u, 0x03u,
};

static uint16_t read_u16_be(const uint8_t* bytes) {
    return (uint16_t)((uint16_t)bytes[0] << 8) | bytes[1];
}

static uint32_t read_u32_be(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void write_u16_be(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t* data, size_t size) {
    while (size >= 2u) {
        sum += read_u16_be(data);
        data += 2u;
        size -= 2u;
    }
    if (size != 0u) sum += (uint32_t)data[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while ((sum >> 16) != 0u) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t checksum(const uint8_t* data, size_t size) {
    return checksum_finish(checksum_add(0u, data, size));
}

static uint16_t transport_checksum(uint32_t source_ip,
                                   uint32_t destination_ip, uint8_t protocol,
                                   const uint8_t* data, size_t size) {
    uint8_t pseudo[12];
    write_u32_be(pseudo, source_ip);
    write_u32_be(pseudo + 4u, destination_ip);
    pseudo[8] = 0u;
    pseudo[9] = protocol;
    write_u16_be(pseudo + 10u, (uint16_t)size);
    uint32_t sum = checksum_add(0u, pseudo, sizeof(pseudo));
    return checksum_finish(checksum_add(sum, data, size));
}

static bool fake_send(void* context, const uint8_t* frame,
                      size_t frame_size) {
    struct fake_link* link = (struct fake_link*)context;
    link->send_attempt_count++;
    if (frame_size >= NET_ETHERNET_HEADER_SIZE) {
        link->last_attempt_ethertype = read_u16_be(frame + 12u);
    }
    if (link->reject_send || link->transmitted_count >= TEST_FRAME_COUNT ||
        frame_size > NET_ETHERNET_FRAME_MAX) {
        return false;
    }
    struct test_frame* output =
        &link->transmitted[link->transmitted_count++];
    output->size = frame_size;
    memcpy(output->bytes, frame, frame_size);
    return true;
}

static size_t fake_poll(void* context, uint8_t* frame,
                        size_t frame_capacity) {
    struct fake_link* link = (struct fake_link*)context;
    if (link->received_index >= link->received_count) return 0u;
    const struct test_frame* input =
        &link->received[link->received_index++];
    assert(input->size <= frame_capacity);
    memcpy(frame, input->bytes, input->size);
    return input->size;
}

static uint64_t fake_now_ms(void* context) {
    return ((const struct fake_link*)context)->now_ms;
}

static void queue_frame(struct fake_link* link, const uint8_t* frame,
                        size_t frame_size) {
    assert(link->received_count < TEST_FRAME_COUNT);
    assert(frame_size <= NET_ETHERNET_FRAME_MAX);
    struct test_frame* queued = &link->received[link->received_count++];
    queued->size = frame_size;
    memcpy(queued->bytes, frame, frame_size);
}

static void init_test_stack(struct net_stack* stack, struct fake_link* link) {
    memset(link, 0, sizeof(*link));
    link->now_ms = 100u;
    struct net_link_ops ops = {
        .send = fake_send,
        .poll = fake_poll,
        .now_ms = fake_now_ms,
        .context = link,
    };
    assert(net_init(stack, &ops, LOCAL_MAC));
}

static void configure_test_ipv4(struct net_stack* stack) {
    const struct net_ipv4_config config = {
        .address = NET_IPV4(10, 0, 2, 15),
        .netmask = NET_IPV4(255, 255, 255, 0),
        .gateway = NET_IPV4(10, 0, 2, 2),
        .dns = NET_IPV4(10, 0, 2, 3),
    };
    net_configure_ipv4(stack, &config);
}

static size_t build_ipv4_frame(
    uint8_t* frame, const uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE],
    const uint8_t source_mac[NET_ETHERNET_ADDRESS_SIZE], uint32_t source_ip,
    uint32_t destination_ip, uint8_t protocol, const uint8_t* payload,
    size_t payload_size) {
    assert(payload_size <= NET_IPV4_PAYLOAD_MAX);
    memcpy(frame, destination_mac, NET_ETHERNET_ADDRESS_SIZE);
    memcpy(frame + 6u, source_mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u16_be(frame + 12u, 0x0800u);
    uint8_t* ip = frame + NET_ETHERNET_HEADER_SIZE;
    memset(ip, 0, NET_IPV4_HEADER_MIN_SIZE);
    ip[0] = 0x45u;
    write_u16_be(ip + 2u,
                 (uint16_t)(NET_IPV4_HEADER_MIN_SIZE + payload_size));
    write_u16_be(ip + 4u, 0x1234u);
    write_u16_be(ip + 6u, 0x4000u);
    ip[8] = 64u;
    ip[9] = protocol;
    write_u32_be(ip + 12u, source_ip);
    write_u32_be(ip + 16u, destination_ip);
    write_u16_be(ip + 10u, checksum(ip, NET_IPV4_HEADER_MIN_SIZE));
    memcpy(ip + NET_IPV4_HEADER_MIN_SIZE, payload, payload_size);
    return NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_MIN_SIZE + payload_size;
}

static size_t build_udp_frame(
    uint8_t* frame, const uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE],
    const uint8_t source_mac[NET_ETHERNET_ADDRESS_SIZE], uint32_t source_ip,
    uint32_t destination_ip, uint16_t source_port, uint16_t destination_port,
    const uint8_t* data, size_t data_size, bool include_checksum) {
    uint8_t udp[NET_IPV4_PAYLOAD_MAX];
    assert(data_size <= NET_UDP_PAYLOAD_MAX);
    write_u16_be(udp, source_port);
    write_u16_be(udp + 2u, destination_port);
    write_u16_be(udp + 4u, (uint16_t)(8u + data_size));
    write_u16_be(udp + 6u, 0u);
    memcpy(udp + 8u, data, data_size);
    if (include_checksum) {
        uint16_t value =
            transport_checksum(source_ip, destination_ip,
                               NET_IPV4_PROTOCOL_UDP, udp, 8u + data_size);
        write_u16_be(udp + 6u, value == 0u ? 0xFFFFu : value);
    }
    return build_ipv4_frame(frame, destination_mac, source_mac, source_ip,
                            destination_ip, NET_IPV4_PROTOCOL_UDP, udp,
                            8u + data_size);
}

static size_t build_arp_reply(
    uint8_t* frame, const uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE],
    const uint8_t source_mac[NET_ETHERNET_ADDRESS_SIZE], uint32_t source_ip,
    uint32_t destination_ip) {
    memcpy(frame, destination_mac, NET_ETHERNET_ADDRESS_SIZE);
    memcpy(frame + 6u, source_mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u16_be(frame + 12u, 0x0806u);
    uint8_t* arp = frame + NET_ETHERNET_HEADER_SIZE;
    write_u16_be(arp, 1u);
    write_u16_be(arp + 2u, 0x0800u);
    arp[4] = 6u;
    arp[5] = 4u;
    write_u16_be(arp + 6u, 2u);
    memcpy(arp + 8u, source_mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(arp + 14u, source_ip);
    memcpy(arp + 18u, destination_mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(arp + 24u, destination_ip);
    return NET_ETHERNET_HEADER_SIZE + 28u;
}

static size_t build_icmp_frame(
    uint8_t* frame, const uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE],
    const uint8_t source_mac[NET_ETHERNET_ADDRESS_SIZE], uint32_t source_ip,
    uint32_t destination_ip, uint8_t type, uint16_t identifier,
    uint16_t sequence, const uint8_t* data, size_t data_size) {
    uint8_t icmp[NET_IPV4_PAYLOAD_MAX];
    assert(data_size <= sizeof(icmp) - 8u);
    icmp[0] = type;
    icmp[1] = 0u;
    write_u16_be(icmp + 2u, 0u);
    write_u16_be(icmp + 4u, identifier);
    write_u16_be(icmp + 6u, sequence);
    memcpy(icmp + 8u, data, data_size);
    write_u16_be(icmp + 2u, checksum(icmp, 8u + data_size));
    return build_ipv4_frame(frame, destination_mac, source_mac, source_ip,
                            destination_ip, NET_IPV4_PROTOCOL_ICMP, icmp,
                            8u + data_size);
}

static void udp_handler(void* context,
                        const struct net_udp_datagram* datagram) {
    struct udp_capture* capture = (struct udp_capture*)context;
    capture->calls++;
    capture->source_ip = datagram->source_ip;
    capture->source_port = datagram->source_port;
    capture->data_size = datagram->data_size;
    assert(datagram->data_size <= sizeof(capture->data));
    memcpy(capture->data, datagram->data, datagram->data_size);
}

static void ipv4_handler(void* context, uint32_t source_ip,
                         uint32_t destination_ip, const uint8_t* payload,
                         size_t payload_size) {
    struct ipv4_capture* capture = (struct ipv4_capture*)context;
    capture->calls++;
    capture->source_ip = source_ip;
    capture->destination_ip = destination_ip;
    capture->payload_size = payload_size;
    assert(payload_size <= sizeof(capture->payload));
    memcpy(capture->payload, payload, payload_size);
}

static uint8_t dhcp_message_type(const struct test_frame* frame) {
    assert(read_u16_be(frame->bytes + 12u) == 0x0800u);
    const uint8_t* ip = frame->bytes + NET_ETHERNET_HEADER_SIZE;
    size_t ip_size = (size_t)(ip[0] & 0x0Fu) * 4u;
    const uint8_t* udp = ip + ip_size;
    const uint8_t* dhcp = udp + 8u;
    size_t dhcp_size = read_u16_be(udp + 4u) - 8u;
    assert(dhcp_size >= 240u);
    assert(read_u32_be(dhcp + 236u) == 0x63825363u);
    size_t offset = 240u;
    while (offset < dhcp_size) {
        uint8_t code = dhcp[offset++];
        if (code == 0u) continue;
        assert(code != 255u);
        assert(offset < dhcp_size);
        uint8_t length = dhcp[offset++];
        assert((size_t)length <= dhcp_size - offset);
        if (code == 53u) {
            assert(length == 1u);
            return dhcp[offset];
        }
        offset += length;
    }
    assert(!"DHCP message type missing");
    return 0u;
}

static bool dhcp_has_option(const struct test_frame* frame,
                            uint8_t wanted_code) {
    const uint8_t* ip = frame->bytes + NET_ETHERNET_HEADER_SIZE;
    const uint8_t* udp = ip + (size_t)(ip[0] & 0x0Fu) * 4u;
    const uint8_t* dhcp = udp + 8u;
    size_t dhcp_size = read_u16_be(udp + 4u) - 8u;
    size_t offset = 240u;
    while (offset < dhcp_size) {
        uint8_t code = dhcp[offset++];
        if (code == 0u) continue;
        if (code == 255u) return false;
        if (offset >= dhcp_size) return false;
        uint8_t length = dhcp[offset++];
        if ((size_t)length > dhcp_size - offset) return false;
        if (code == wanted_code) return true;
        offset += length;
    }
    return false;
}

static size_t build_dhcp_response(uint8_t* packet, uint8_t message_type,
                                  uint32_t transaction_id,
                                  uint32_t offered_ip,
                                  const uint8_t client_mac[6]) {
    memset(packet, 0, 576u);
    packet[0] = 2u;
    packet[1] = 1u;
    packet[2] = 6u;
    write_u32_be(packet + 4u, transaction_id);
    write_u32_be(packet + 16u, offered_ip);
    memcpy(packet + 28u, client_mac, 6u);
    write_u32_be(packet + 236u, 0x63825363u);

    size_t offset = 240u;
    packet[offset++] = 53u;
    packet[offset++] = 1u;
    packet[offset++] = message_type;
#define APPEND_IPV4_OPTION(code, value)          \
    do {                                         \
        packet[offset++] = (code);               \
        packet[offset++] = 4u;                   \
        write_u32_be(packet + offset, (value));  \
        offset += 4u;                            \
    } while (0)
    APPEND_IPV4_OPTION(54u, NET_IPV4(10, 0, 2, 2));
    APPEND_IPV4_OPTION(1u, NET_IPV4(255, 255, 255, 0));
    APPEND_IPV4_OPTION(3u, NET_IPV4(10, 0, 2, 2));
    APPEND_IPV4_OPTION(6u, NET_IPV4(10, 0, 2, 3));
    APPEND_IPV4_OPTION(51u, 100u);
    APPEND_IPV4_OPTION(58u, 50u);
    APPEND_IPV4_OPTION(59u, 87u);
#undef APPEND_IPV4_OPTION
    packet[offset++] = 255u;
    return offset;
}

static size_t encode_dns_name(uint8_t* output, const char* name) {
    size_t output_offset = 0u;
    const char* label = name;
    const char* cursor = name;
    while (true) {
        if (*cursor == '.' || *cursor == '\0') {
            size_t length = (size_t)(cursor - label);
            assert(length > 0u && length <= 63u);
            output[output_offset++] = (uint8_t)length;
            memcpy(output + output_offset, label, length);
            output_offset += length;
            if (*cursor == '\0') break;
            label = cursor + 1;
        }
        cursor++;
    }
    output[output_offset++] = 0u;
    return output_offset;
}

static size_t build_dns_response(uint8_t* message, uint16_t transaction_id,
                                 const char* name, uint32_t address,
                                 uint32_t ttl_seconds) {
    memset(message, 0, 512u);
    write_u16_be(message, transaction_id);
    write_u16_be(message + 2u, 0x8180u);
    write_u16_be(message + 4u, 1u);
    write_u16_be(message + 6u, 1u);
    size_t offset = 12u;
    offset += encode_dns_name(message + offset, name);
    write_u16_be(message + offset, 1u);
    write_u16_be(message + offset + 2u, 1u);
    offset += 4u;
    write_u16_be(message + offset, 0xC00Cu);
    write_u16_be(message + offset + 2u, 1u);
    write_u16_be(message + offset + 4u, 1u);
    write_u32_be(message + offset + 6u, ttl_seconds);
    write_u16_be(message + offset + 10u, 4u);
    write_u32_be(message + offset + 12u, address);
    return offset + 16u;
}

static size_t build_dns_multi_a_response(
    uint8_t* message, uint16_t transaction_id, const char* name,
    const uint32_t* addresses, const uint32_t* ttl_seconds,
    size_t address_count) {
    assert(addresses != NULL);
    assert(ttl_seconds != NULL);
    assert(address_count > 0u && address_count <= 16u);
    memset(message, 0, 512u);
    write_u16_be(message, transaction_id);
    write_u16_be(message + 2u, 0x8180u);
    write_u16_be(message + 4u, 1u);
    write_u16_be(message + 6u, (uint16_t)address_count);
    size_t offset = 12u;
    offset += encode_dns_name(message + offset, name);
    write_u16_be(message + offset, 1u);
    write_u16_be(message + offset + 2u, 1u);
    offset += 4u;

    for (size_t index = 0u; index < address_count; index++) {
        write_u16_be(message + offset, 0xC00Cu);
        write_u16_be(message + offset + 2u, 1u);
        write_u16_be(message + offset + 4u, 1u);
        write_u32_be(message + offset + 6u, ttl_seconds[index]);
        write_u16_be(message + offset + 10u, 4u);
        write_u32_be(message + offset + 12u, addresses[index]);
        offset += 16u;
    }
    return offset;
}

static size_t build_dns_cname_response(
    uint8_t* message, uint16_t transaction_id, const char* query_name,
    const char* target_name, uint32_t address, bool make_loop) {
    memset(message, 0, 512u);
    write_u16_be(message, transaction_id);
    write_u16_be(message + 2u, 0x8180u);
    write_u16_be(message + 4u, 1u);
    write_u16_be(message + 6u, make_loop ? 2u : 3u);
    size_t offset = 12u;
    offset += encode_dns_name(message + offset, query_name);
    write_u16_be(message + offset, 1u);
    write_u16_be(message + offset + 2u, 1u);
    offset += 4u;

    if (!make_loop) {
        offset += encode_dns_name(message + offset,
                                  "unrelated.example.com");
        write_u16_be(message + offset, 1u);
        write_u16_be(message + offset + 2u, 1u);
        write_u32_be(message + offset + 4u, 999u);
        write_u16_be(message + offset + 8u, 4u);
        write_u32_be(message + offset + 10u, NET_IPV4(203, 0, 113, 9));
        offset += 14u;
    }

    write_u16_be(message + offset, 0xC00Cu);
    write_u16_be(message + offset + 2u, 5u);
    write_u16_be(message + offset + 4u, 1u);
    write_u32_be(message + offset + 6u, 120u);
    size_t cname_size_offset = offset + 10u;
    offset += 12u;
    size_t target_size = encode_dns_name(message + offset, target_name);
    write_u16_be(message + cname_size_offset, (uint16_t)target_size);
    offset += target_size;

    offset += encode_dns_name(message + offset, target_name);
    if (make_loop) {
        write_u16_be(message + offset, 5u);
        write_u16_be(message + offset + 2u, 1u);
        write_u32_be(message + offset + 4u, 90u);
        write_u16_be(message + offset + 8u, 2u);
        write_u16_be(message + offset + 10u, 0xC00Cu);
        offset += 12u;
    } else {
        write_u16_be(message + offset, 1u);
        write_u16_be(message + offset + 2u, 1u);
        write_u32_be(message + offset + 4u, 60u);
        write_u16_be(message + offset + 8u, 4u);
        write_u32_be(message + offset + 10u, address);
        offset += 14u;
    }
    return offset;
}

static size_t build_dns_negative_response(
    uint8_t* message, uint16_t transaction_id, const char* query_name,
    uint8_t response_code, bool include_soa, uint32_t soa_ttl,
    uint32_t minimum_ttl) {
    memset(message, 0, 512u);
    write_u16_be(message, transaction_id);
    write_u16_be(message + 2u, (uint16_t)(0x8180u | response_code));
    write_u16_be(message + 4u, 1u);
    write_u16_be(message + 8u, include_soa ? 1u : 0u);
    size_t offset = 12u;
    offset += encode_dns_name(message + offset, query_name);
    write_u16_be(message + offset, 1u);
    write_u16_be(message + offset + 2u, 1u);
    offset += 4u;
    if (!include_soa) return offset;

    offset += encode_dns_name(message + offset, "example.com");
    write_u16_be(message + offset, 6u);
    write_u16_be(message + offset + 2u, 1u);
    write_u32_be(message + offset + 4u, soa_ttl);
    size_t data_size_offset = offset + 8u;
    offset += 10u;
    size_t data_start = offset;
    offset += encode_dns_name(message + offset, "ns.example.com");
    offset += encode_dns_name(message + offset,
                              "hostmaster.example.com");
    write_u32_be(message + offset, 1u);
    write_u32_be(message + offset + 4u, 3600u);
    write_u32_be(message + offset + 8u, 600u);
    write_u32_be(message + offset + 12u, 86400u);
    write_u32_be(message + offset + 16u, minimum_ttl);
    offset += 20u;
    write_u16_be(message + data_size_offset,
                 (uint16_t)(offset - data_start));
    return offset;
}

static size_t build_dns_cname_negative_response(
    uint8_t* message, uint16_t transaction_id, const char* query_name,
    const char* target_name, uint8_t response_code, uint32_t cname_ttl,
    uint32_t soa_ttl, uint32_t minimum_ttl) {
    memset(message, 0, 512u);
    write_u16_be(message, transaction_id);
    write_u16_be(message + 2u, (uint16_t)(0x8180u | response_code));
    write_u16_be(message + 4u, 1u);
    write_u16_be(message + 6u, 1u);
    write_u16_be(message + 8u, 1u);
    size_t offset = 12u;
    offset += encode_dns_name(message + offset, query_name);
    write_u16_be(message + offset, 1u);
    write_u16_be(message + offset + 2u, 1u);
    offset += 4u;

    write_u16_be(message + offset, 0xC00Cu);
    write_u16_be(message + offset + 2u, 5u);
    write_u16_be(message + offset + 4u, 1u);
    write_u32_be(message + offset + 6u, cname_ttl);
    size_t cname_size_offset = offset + 10u;
    offset += 12u;
    size_t target_size = encode_dns_name(message + offset, target_name);
    write_u16_be(message + cname_size_offset, (uint16_t)target_size);
    offset += target_size;

    offset += encode_dns_name(message + offset, "example.com");
    write_u16_be(message + offset, 6u);
    write_u16_be(message + offset + 2u, 1u);
    write_u32_be(message + offset + 4u, soa_ttl);
    size_t soa_size_offset = offset + 8u;
    offset += 10u;
    size_t soa_start = offset;
    offset += encode_dns_name(message + offset, "ns.example.com");
    offset += encode_dns_name(message + offset,
                              "hostmaster.example.com");
    write_u32_be(message + offset, 1u);
    write_u32_be(message + offset + 4u, 3600u);
    write_u32_be(message + offset + 8u, 600u);
    write_u32_be(message + offset + 12u, 86400u);
    write_u32_be(message + offset + 16u, minimum_ttl);
    offset += 20u;
    write_u16_be(message + soa_size_offset,
                 (uint16_t)(offset - soa_start));
    return offset;
}

static void provide_dns_arp(struct net_stack* stack,
                            struct fake_link* link) {
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t frame_size =
        build_arp_reply(frame, LOCAL_MAC, DNS_MAC, NET_IPV4(10, 0, 2, 3),
                        NET_IPV4(10, 0, 2, 15));
    assert(net_input(stack, frame, frame_size));
    assert(link->transmitted_count >= 2u);
}

static void test_initialization_and_configuration(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    assert(net_now_ms(&stack) == 100u);
    assert(!net_is_configured(&stack));
    assert(net_local_ip(&stack) == 0u);
    configure_test_ipv4(&stack);
    assert(net_is_configured(&stack));
    assert(net_local_ip(&stack) == NET_IPV4(10, 0, 2, 15));
    assert(net_gateway_ip(&stack) == NET_IPV4(10, 0, 2, 2));
    assert(net_dns_ip(&stack) == NET_IPV4(10, 0, 2, 3));

    const uint8_t invalid_mac[6] = {0};
    struct net_link_ops ops = {
        .send = fake_send,
        .context = &link,
    };
    assert(!net_init(&stack, &ops, invalid_mac));
    ops.send = NULL;
    assert(!net_init(&stack, &ops, LOCAL_MAC));
}

static void test_arp_and_udp_output(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    const uint8_t message[] = {'h', 'e', 'l', 'l', 'o'};
    assert(net_udp_send(&stack, NET_IPV4(10, 0, 2, 2), 9000u, 4000u,
                        message, sizeof(message)) == NET_SEND_QUEUED);
    assert(link.transmitted_count == 1u);
    const struct test_frame* arp_request = &link.transmitted[0];
    assert(read_u16_be(arp_request->bytes + 12u) == 0x0806u);
    assert(read_u16_be(arp_request->bytes + 20u) == 1u);
    assert(read_u32_be(arp_request->bytes + 38u) ==
           NET_IPV4(10, 0, 2, 2));

    uint8_t reply[NET_ETHERNET_FRAME_MAX];
    size_t reply_size =
        build_arp_reply(reply, LOCAL_MAC, PEER_MAC, NET_IPV4(10, 0, 2, 2),
                        NET_IPV4(10, 0, 2, 15));
    assert(net_input(&stack, reply, reply_size));
    assert(link.transmitted_count == 2u);
    assert(!stack.pending_ipv4.valid);

    const struct test_frame* udp_frame = &link.transmitted[1];
    assert(memcmp(udp_frame->bytes, PEER_MAC, 6u) == 0);
    assert(read_u16_be(udp_frame->bytes + 12u) == 0x0800u);
    const uint8_t* ip = udp_frame->bytes + 14u;
    assert(checksum(ip, 20u) == 0u);
    assert(read_u32_be(ip + 12u) == NET_IPV4(10, 0, 2, 15));
    assert(read_u32_be(ip + 16u) == NET_IPV4(10, 0, 2, 2));
    const uint8_t* udp = ip + 20u;
    assert(read_u16_be(udp) == 4000u);
    assert(read_u16_be(udp + 2u) == 9000u);
    assert(transport_checksum(NET_IPV4(10, 0, 2, 15),
                              NET_IPV4(10, 0, 2, 2),
                              NET_IPV4_PROTOCOL_UDP, udp,
                              read_u16_be(udp + 4u)) == 0u);
    assert(memcmp(udp + 8u, message, sizeof(message)) == 0);

    uint8_t resolved_mac[6];
    assert(net_arp_lookup(&stack, NET_IPV4(10, 0, 2, 2), resolved_mac));
    assert(memcmp(resolved_mac, PEER_MAC, 6u) == 0);
    net_tick(&stack, 120101u);
    assert(!net_arp_lookup(&stack, NET_IPV4(10, 0, 2, 2), resolved_mac));
}

static void test_resolved_arp_retries_ipv4_not_arp(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);
    link.reject_send = true;

    const uint8_t message[] = {0xAAu};
    assert(net_udp_send(&stack, NET_IPV4(10, 0, 2, 2), 9000u, 4000u,
                        message, sizeof(message)) == NET_SEND_QUEUED);
    assert(stack.pending_ipv4.valid);
    assert(link.send_attempt_count == 1u);
    assert(link.last_attempt_ethertype == 0x0806u);

    uint8_t reply[NET_ETHERNET_FRAME_MAX];
    size_t reply_size =
        build_arp_reply(reply, LOCAL_MAC, PEER_MAC, NET_IPV4(10, 0, 2, 2),
                        NET_IPV4(10, 0, 2, 15));
    assert(net_input(&stack, reply, reply_size));
    assert(stack.pending_ipv4.valid);
    assert(link.send_attempt_count == 2u);
    assert(link.last_attempt_ethertype == 0x0800u);

    net_tick(&stack, 1100u);
    assert(stack.pending_ipv4.valid);
    assert(link.send_attempt_count == 3u);
    assert(link.last_attempt_ethertype == 0x0800u);

    link.reject_send = false;
    net_tick(&stack, 2100u);
    assert(!stack.pending_ipv4.valid);
    assert(link.transmitted_count == 1u);
    assert(read_u16_be(link.transmitted[0].bytes + 12u) == 0x0800u);
    assert(net_get_stats(&stack)->udp_datagrams_transmitted == 1u);
}

static void test_udp_poll_and_tcp_dispatch(void) {
    struct net_stack stack;
    struct fake_link link;
    struct udp_capture udp_capture = {0};
    struct ipv4_capture tcp_capture = {0};
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);
    assert(net_udp_bind(&stack, 4321u, udp_handler, &udp_capture));
    assert(!net_udp_bind(&stack, 4321u, udp_handler, &udp_capture));
    assert(!net_udp_bind(&stack, 68u, udp_handler, &udp_capture));
    net_set_tcp_handler(&stack, ipv4_handler, &tcp_capture);

    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    const uint8_t data[] = {1u, 2u, 3u, 4u};
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                        1234u, 4321u, data, sizeof(data), true);
    queue_frame(&link, frame, frame_size);
    assert(net_poll(&stack) == 1u);
    assert(udp_capture.calls == 1u);
    assert(udp_capture.source_ip == NET_IPV4(10, 0, 2, 2));
    assert(udp_capture.source_port == 1234u);
    assert(udp_capture.data_size == sizeof(data));
    assert(memcmp(udp_capture.data, data, sizeof(data)) == 0);

    frame[frame_size - 1u] ^= 1u;
    assert(!net_input(&stack, frame, frame_size));
    assert(udp_capture.calls == 1u);

    const uint8_t tcp_segment[] = {0x12u, 0x34u, 0x56u, 0x78u};
    frame_size =
        build_ipv4_frame(frame, LOCAL_MAC, PEER_MAC,
                         NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                         NET_IPV4_PROTOCOL_TCP, tcp_segment,
                         sizeof(tcp_segment));
    assert(net_input(&stack, frame, frame_size));
    assert(tcp_capture.calls == 1u);
    assert(tcp_capture.source_ip == NET_IPV4(10, 0, 2, 2));
    assert(tcp_capture.destination_ip == NET_IPV4(10, 0, 2, 15));
    assert(tcp_capture.payload_size == sizeof(tcp_segment));
    assert(memcmp(tcp_capture.payload, tcp_segment, sizeof(tcp_segment)) ==
           0);

    net_udp_unbind(&stack, 4321u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                        1234u, 4321u, data, sizeof(data), false);
    assert(!net_input(&stack, frame, frame_size));
}

static void test_icmp_echo(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    size_t frame_size =
        build_icmp_frame(frame, LOCAL_MAC, PEER_MAC,
                         NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15), 8u,
                         0x1234u, 7u, payload, sizeof(payload));
    assert(net_input(&stack, frame, frame_size));
    assert(link.transmitted_count == 1u);
    const uint8_t* reply_ip = link.transmitted[0].bytes + 14u;
    const uint8_t* reply_icmp = reply_ip + 20u;
    assert(reply_icmp[0] == 0u);
    assert(checksum(reply_icmp,
                    read_u16_be(reply_ip + 2u) - 20u) == 0u);

    assert(net_icmp_echo_request(&stack, NET_IPV4(10, 0, 2, 2), 0xBEEFu,
                                 9u, payload,
                                 sizeof(payload)) == NET_SEND_SENT);
    assert(link.transmitted_count == 2u);
    net_tick(&stack, 150u);
    frame_size =
        build_icmp_frame(frame, LOCAL_MAC, PEER_MAC,
                         NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15), 0u,
                         0xBEEFu, 9u, payload, sizeof(payload));
    assert(net_input(&stack, frame, frame_size));
    struct net_icmp_echo_reply result = net_icmp_last_echo_reply(&stack);
    assert(result.valid);
    assert(result.source_ip == NET_IPV4(10, 0, 2, 2));
    assert(result.identifier == 0xBEEFu);
    assert(result.sequence == 9u);
    assert(result.round_trip_ms == 50u);
    assert(result.payload_size == sizeof(payload));
    net_icmp_clear_echo_reply(&stack);
    assert(!net_icmp_last_echo_reply(&stack).valid);
}

static void test_dhcp_handshake_and_renewal(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    const uint32_t transaction_id = 0x13572468u;
    assert(net_dhcp_start(&stack, transaction_id));
    assert(net_dhcp_get_state(&stack) == NET_DHCP_DISCOVERING);
    assert(link.transmitted_count == 1u);
    assert(dhcp_message_type(&link.transmitted[0]) == 1u);
    const uint8_t* discover_ip = link.transmitted[0].bytes + 14u;
    assert(read_u32_be(discover_ip + 12u) == 0u);
    assert(read_u32_be(discover_ip + 16u) == UINT32_MAX);

    uint8_t dhcp[576];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dhcp_size =
        build_dhcp_response(dhcp, 2u, transaction_id,
                            NET_IPV4(10, 0, 2, 15), LOCAL_MAC);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), UINT32_MAX, 67u, 68u, dhcp,
                        dhcp_size, true);
    assert(net_input(&stack, frame, frame_size));
    assert(net_dhcp_get_state(&stack) == NET_DHCP_REQUESTING);
    assert(link.transmitted_count == 2u);
    assert(dhcp_message_type(&link.transmitted[1]) == 3u);

    dhcp_size =
        build_dhcp_response(dhcp, 5u, transaction_id,
                            NET_IPV4(10, 0, 2, 15), LOCAL_MAC);
    /* Exercise a valid unicast ACK before the address is configured. */
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                        67u, 68u, dhcp, dhcp_size, true);
    assert(net_input(&stack, frame, frame_size));
    assert(net_dhcp_get_state(&stack) == NET_DHCP_BOUND);
    struct net_ipv4_config config = net_get_ipv4_config(&stack);
    assert(config.address == NET_IPV4(10, 0, 2, 15));
    assert(config.netmask == NET_IPV4(255, 255, 255, 0));
    assert(config.gateway == NET_IPV4(10, 0, 2, 2));
    assert(config.dns == NET_IPV4(10, 0, 2, 3));

    net_tick(&stack, 50100u);
    assert(net_dhcp_get_state(&stack) == NET_DHCP_RENEWING);
    assert(stack.pending_ipv4.valid);
    assert(link.transmitted_count == 3u);
    assert(read_u16_be(link.transmitted[2].bytes + 12u) == 0x0806u);

    net_tick(&stack, 87100u);
    assert(net_dhcp_get_state(&stack) == NET_DHCP_REBINDING);
    const struct test_frame* rebind =
        &link.transmitted[link.transmitted_count - 1u];
    assert(dhcp_message_type(rebind) == 3u);
    const uint8_t* rebind_ip = rebind->bytes + 14u;
    const uint8_t* rebind_dhcp = rebind_ip + 20u + 8u;
    assert(read_u32_be(rebind_dhcp + 12u) == NET_IPV4(10, 0, 2, 15));
    assert(read_u16_be(rebind_dhcp + 10u) == 0x8000u);
    assert(!dhcp_has_option(rebind, 50u));
    assert(!dhcp_has_option(rebind, 54u));
}

static void test_dns_resolution_and_timeout(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);
    assert(net_dns_query(&stack, "example.com", 0x4242u));
    assert(net_dns_get_result(&stack).state == NET_DNS_PENDING);
    assert(link.transmitted_count == 1u);
    assert(read_u16_be(link.transmitted[0].bytes + 12u) == 0x0806u);

    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t frame_size =
        build_arp_reply(frame, LOCAL_MAC, DNS_MAC, NET_IPV4(10, 0, 2, 3),
                        NET_IPV4(10, 0, 2, 15));
    assert(net_input(&stack, frame, frame_size));
    assert(link.transmitted_count == 2u);
    const struct test_frame* query_frame = &link.transmitted[1];
    const uint8_t* query_ip = query_frame->bytes + 14u;
    const uint8_t* query_udp = query_ip + 20u;
    uint16_t source_port = read_u16_be(query_udp);
    assert(read_u16_be(query_udp + 2u) == 53u);
    assert(read_u16_be(query_udp + 8u) == 0x4242u);
    assert(read_u16_be(query_udp + 12u) == 1u);

    uint8_t dns[512];
    size_t dns_size =
        build_dns_response(dns, 0x4242u, "example.com",
                           NET_IPV4(93, 184, 216, 34), 300u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_RESOLVED);
    assert(result.address == NET_IPV4(93, 184, 216, 34));
    assert(result.ttl_seconds == 300u);
    assert(result.response_code == 0u);

    assert(!net_dns_query(&stack, "bad..name", 1u));
    assert(net_dns_query(&stack, "timeout.example", 0x4343u));
    net_tick(&stack, 2100u);
    net_tick(&stack, 4100u);
    net_tick(&stack, 6100u);
    assert(net_dns_get_result(&stack).state == NET_DNS_TIMED_OUT);
    net_dns_clear_result(&stack);
    assert(net_dns_get_result(&stack).state == NET_DNS_IDLE);
}

static void test_dns_positive_cache_and_expiry(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(net_dns_query(&stack, "cache.example", 0x5001u));
    provide_dns_arp(&stack, &link);
    uint16_t source_port = stack.dns.source_port;

    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size =
        build_dns_response(dns, 0x5001u, "cache.example",
                           NET_IPV4(192, 0, 2, 25), 2u);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    assert(net_dns_get_result(&stack).state == NET_DNS_RESOLVED);

    struct net_dns_cache_status status =
        net_dns_cache_get_status(&stack);
    assert(status.capacity == NET_DNS_CACHE_SIZE);
    assert(status.entries == 1u);
    assert(status.positive_entries == 1u);
    assert(status.negative_entries == 0u);
    assert(status.hits == 0u);
    assert(status.misses == 1u);

    size_t sent_before = link.transmitted_count;
    net_dns_clear_result(&stack);
    assert(net_dns_query(&stack, "CACHE.EXAMPLE.", 0x5002u));
    assert(link.transmitted_count == sent_before);
    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_RESOLVED);
    assert(result.address == NET_IPV4(192, 0, 2, 25));
    assert(result.ttl_seconds == 2u);

    struct net_dns_cache_entry_info entries[NET_DNS_CACHE_SIZE];
    assert(net_dns_cache_snapshot(&stack, entries,
                                  NET_DNS_CACHE_SIZE) == 1u);
    assert(strcmp(entries[0].name, "cache.example") == 0);
    assert(entries[0].kind == NET_DNS_CACHE_POSITIVE);
    assert(entries[0].address == NET_IPV4(192, 0, 2, 25));
    assert(entries[0].remaining_ttl_seconds == 2u);
    assert(net_dns_cache_snapshot(&stack, NULL, 0u) == 0u);
    assert(net_dns_cache_snapshot(&stack, NULL, 1u) == 0u);

    net_tick(&stack, 2099u);
    status = net_dns_cache_get_status(&stack);
    assert(status.entries == 1u);
    assert(net_dns_cache_snapshot(&stack, entries, 1u) == 1u);
    assert(entries[0].remaining_ttl_seconds == 1u);
    net_tick(&stack, 2100u);
    status = net_dns_cache_get_status(&stack);
    assert(status.entries == 0u);
    assert(status.expirations == 1u);

    net_dns_clear_result(&stack);
    sent_before = link.transmitted_count;
    assert(net_dns_query(&stack, "cache.example", 0x5003u));
    assert(net_dns_get_result(&stack).state == NET_DNS_PENDING);
    assert(link.transmitted_count == sent_before + 1u);
    status = net_dns_cache_get_status(&stack);
    assert(status.hits == 1u);
    assert(status.misses == 2u);

    net_dns_cache_clear(&stack);
    status = net_dns_cache_get_status(&stack);
    assert(status.entries == 0u);
    assert(status.hits == 1u);
    assert(status.misses == 2u);
}

static void test_dns_multi_address_cache_uses_lowest_usable_ttl(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(net_dns_query(&stack, "rrset.example", 0x5051u));
    provide_dns_arp(&stack, &link);
    uint16_t source_port = stack.dns.source_port;
    const uint32_t addresses[] = {
        NET_IPV4(192, 0, 2, 10),
        NET_IPV4(0, 0, 0, 0),
        NET_IPV4(192, 0, 2, 30),
    };
    const uint32_t ttls[] = {300u, 1u, 20u};
    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size = build_dns_multi_a_response(
        dns, 0x5051u, "rrset.example", addresses, ttls,
        sizeof(addresses) / sizeof(addresses[0]));
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));

    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_RESOLVED);
    assert(result.address == addresses[0]);
    assert(result.ttl_seconds == 20u);
    struct net_dns_cache_entry_info cached;
    assert(net_dns_cache_snapshot(&stack, &cached, 1u) == 1u);
    assert(cached.kind == NET_DNS_CACHE_POSITIVE);
    assert(cached.address == addresses[0]);
    assert(cached.remaining_ttl_seconds == 20u);
}

static void test_dns_cname_validation(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(net_dns_query(&stack, "www.example.com", 0x5101u));
    provide_dns_arp(&stack, &link);
    uint16_t source_port = stack.dns.source_port;
    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size = build_dns_cname_response(
        dns, 0x5101u, "www.example.com", "edge.example.net",
        NET_IPV4(93, 184, 216, 34), false);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_RESOLVED);
    assert(result.address == NET_IPV4(93, 184, 216, 34));
    assert(result.ttl_seconds == 60u);
    assert(net_dns_cache_get_status(&stack).positive_entries == 1u);

    net_dns_cache_clear(&stack);
    net_dns_clear_result(&stack);
    assert(net_dns_query(&stack, "loop.example.com", 0x5102u));
    source_port = stack.dns.source_port;
    dns_size = build_dns_cname_response(
        dns, 0x5102u, "loop.example.com", "alias.example.com", 0u, true);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    assert(net_dns_get_result(&stack).state ==
           NET_DNS_MALFORMED_RESPONSE);
    assert(net_dns_cache_get_status(&stack).entries == 0u);
}

static void test_dns_safe_negative_cache(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(net_dns_query(&stack, "missing.example.com", 0x5201u));
    provide_dns_arp(&stack, &link);
    uint16_t source_port = stack.dns.source_port;
    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size = build_dns_negative_response(
        dns, 0x5201u, "missing.example.com", 3u, true, 120u, 120u);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_NOT_FOUND);
    assert(result.response_code == 3u);
    assert(result.ttl_seconds == NET_DNS_NEGATIVE_TTL_MAX_SECONDS);

    struct net_dns_cache_status status =
        net_dns_cache_get_status(&stack);
    assert(status.entries == 1u);
    assert(status.negative_entries == 1u);
    struct net_dns_cache_entry_info entry;
    assert(net_dns_cache_snapshot(&stack, &entry, 1u) == 1u);
    assert(entry.kind == NET_DNS_CACHE_NEGATIVE);
    assert(entry.response_code == 3u);
    assert(entry.remaining_ttl_seconds ==
           NET_DNS_NEGATIVE_TTL_MAX_SECONDS);

    size_t sent_before = link.transmitted_count;
    net_dns_clear_result(&stack);
    assert(net_dns_query(&stack, "MISSING.EXAMPLE.COM.", 0x5202u));
    assert(link.transmitted_count == sent_before);
    result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_NOT_FOUND);
    assert(result.response_code == 3u);

    net_tick(&stack,
             100u + NET_DNS_NEGATIVE_TTL_MAX_SECONDS * 1000u);
    assert(net_dns_cache_get_status(&stack).entries == 0u);

    net_dns_clear_result(&stack);
    assert(net_dns_query(&stack, "unsafe.example.com", 0x5203u));
    source_port = stack.dns.source_port;
    dns_size = build_dns_negative_response(
        dns, 0x5203u, "unsafe.example.com", 3u, false, 0u, 0u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_NOT_FOUND);
    assert(result.ttl_seconds == 0u);
    assert(net_dns_cache_get_status(&stack).entries == 0u);

    net_dns_clear_result(&stack);
    sent_before = link.transmitted_count;
    assert(net_dns_query(&stack, "unsafe.example.com", 0x5204u));
    assert(link.transmitted_count == sent_before + 1u);
    assert(net_dns_get_result(&stack).state == NET_DNS_PENDING);

    net_dns_clear_result(&stack);
    assert(net_dns_query(&stack, "nodata.example.com", 0x5205u));
    source_port = stack.dns.source_port;
    dns_size = build_dns_negative_response(
        dns, 0x5205u, "nodata.example.com", 0u, true, 120u, 30u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_NOT_FOUND);
    assert(result.response_code == 0u);
    assert(result.ttl_seconds == 30u);
    assert(net_dns_cache_get_status(&stack).negative_entries == 1u);

    net_dns_clear_result(&stack);
    sent_before = link.transmitted_count;
    assert(net_dns_query(&stack, "nodata.example.com", 0x5206u));
    assert(link.transmitted_count == sent_before);
    assert(net_dns_get_result(&stack).state == NET_DNS_NOT_FOUND);
    assert(net_dns_get_result(&stack).response_code == 0u);
}

static void test_dns_cname_caps_negative_cache_ttl(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(net_dns_query(&stack, "alias.example.com", 0x5241u));
    provide_dns_arp(&stack, &link);
    uint16_t source_port = stack.dns.source_port;
    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size = build_dns_cname_negative_response(
        dns, 0x5241u, "alias.example.com", "missing.example.com",
        3u, 7u, 120u, 90u);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));

    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_NOT_FOUND);
    assert(result.response_code == 3u);
    assert(result.ttl_seconds == 7u);
    struct net_dns_cache_entry_info entry;
    assert(net_dns_cache_snapshot(&stack, &entry, 1u) == 1u);
    assert(entry.kind == NET_DNS_CACHE_NEGATIVE);
    assert(entry.remaining_ttl_seconds == 7u);

    net_dns_cache_clear(&stack);
    net_dns_clear_result(&stack);
    assert(net_dns_query(
        &stack, "zero-alias.example.com", 0x5242u));
    source_port = stack.dns.source_port;
    dns_size = build_dns_cname_negative_response(
        dns, 0x5242u, "zero-alias.example.com",
        "still-missing.example.com", 3u, 0u, 120u, 90u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_NOT_FOUND);
    assert(result.ttl_seconds == 0u);
    assert(net_dns_cache_get_status(&stack).entries == 0u);
}

static void test_dns_server_errors_are_not_negative_answers(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(net_dns_query(&stack, "retry.example.com", 0x5251u));
    provide_dns_arp(&stack, &link);
    uint16_t source_port = stack.dns.source_port;
    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size = build_dns_negative_response(
        dns, 0x5251u, "retry.example.com", 2u, true, 120u, 120u);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    struct net_dns_result result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_SERVER_ERROR);
    assert(result.response_code == 2u);
    assert(result.ttl_seconds == 0u);
    assert(net_dns_cache_get_status(&stack).entries == 0u);

    net_dns_clear_result(&stack);
    size_t sent_before = link.transmitted_count;
    assert(net_dns_query(&stack, "retry.example.com", 0x5252u));
    assert(net_dns_get_result(&stack).state == NET_DNS_PENDING);
    assert(link.transmitted_count == sent_before + 1u);
    source_port = stack.dns.source_port;
    dns_size = build_dns_negative_response(
        dns, 0x5252u, "retry.example.com", 5u, true, 120u, 120u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    result = net_dns_get_result(&stack);
    assert(result.state == NET_DNS_SERVER_ERROR);
    assert(result.response_code == 5u);
    assert(result.ttl_seconds == 0u);
    assert(net_dns_cache_get_status(&stack).entries == 0u);
}

static void test_dns_cancel_stops_retries_and_late_completion(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);

    assert(!net_dns_cancel(NULL));
    assert(!net_dns_cancel(&stack));
    assert(net_dns_query(&stack, "cancel.example", 0x5271u));
    assert(stack.pending_ipv4.valid);
    uint16_t source_port = stack.dns.source_port;
    assert(net_dns_cancel(&stack));
    assert(net_dns_get_result(&stack).state == NET_DNS_IDLE);
    assert(!stack.pending_ipv4.valid);
    assert(!net_dns_cancel(&stack));

    size_t sent_before = link.transmitted_count;
    net_tick(&stack, 1100u);
    net_tick(&stack, 2100u);
    net_tick(&stack, 4100u);
    assert(link.transmitted_count == sent_before);
    assert(net_dns_get_result(&stack).state == NET_DNS_IDLE);

    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    size_t dns_size =
        build_dns_response(dns, 0x5271u, "cancel.example",
                           NET_IPV4(192, 0, 2, 71), 300u);
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(!net_input(&stack, frame, frame_size));
    assert(net_dns_get_result(&stack).state == NET_DNS_IDLE);

    assert(net_dns_query(&stack, "cancel.example", 0x5272u));
    provide_dns_arp(&stack, &link);
    assert(!stack.pending_ipv4.valid);
    assert(net_dns_cancel(&stack));
    sent_before = link.transmitted_count;
    net_tick(&stack, 6100u);
    net_tick(&stack, 8100u);
    net_tick(&stack, 10100u);
    assert(link.transmitted_count == sent_before);
    assert(net_dns_get_result(&stack).state == NET_DNS_IDLE);

    assert(net_dns_query(&stack, "cancel.example", 0x5273u));
    source_port = stack.dns.source_port;
    dns_size =
        build_dns_response(dns, 0x5273u, "cancel.example",
                           NET_IPV4(192, 0, 2, 72), 300u);
    frame_size =
        build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                        NET_IPV4(10, 0, 2, 3), NET_IPV4(10, 0, 2, 15),
                        53u, source_port, dns, dns_size, true);
    assert(net_input(&stack, frame, frame_size));
    assert(net_dns_get_result(&stack).state == NET_DNS_RESOLVED);
    assert(!net_dns_cancel(&stack));
    assert(net_dns_get_result(&stack).state == NET_DNS_RESOLVED);
}

static void test_dns_cache_is_bounded(void) {
    struct net_stack stack;
    struct fake_link link;
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);
    uint8_t dns[512];
    uint8_t frame[NET_ETHERNET_FRAME_MAX];

    for (size_t i = 0; i < NET_DNS_CACHE_SIZE + 1u; i++) {
        char name[] = "n0.example";
        name[1] = (char)('0' + i);
        uint16_t transaction_id = (uint16_t)(0x5300u + i);
        assert(net_dns_query(&stack, name, transaction_id));
        if (i == 0u) provide_dns_arp(&stack, &link);
        uint16_t source_port = stack.dns.source_port;
        size_t dns_size = build_dns_response(
            dns, transaction_id, name,
            NET_IPV4(192, 0, 2, (uint8_t)(i + 1u)), 300u);
        size_t frame_size =
            build_udp_frame(frame, LOCAL_MAC, DNS_MAC,
                            NET_IPV4(10, 0, 2, 3),
                            NET_IPV4(10, 0, 2, 15), 53u, source_port,
                            dns, dns_size, true);
        assert(net_input(&stack, frame, frame_size));
        assert(net_dns_get_result(&stack).state == NET_DNS_RESOLVED);
        net_dns_clear_result(&stack);
    }

    struct net_dns_cache_status status =
        net_dns_cache_get_status(&stack);
    assert(status.capacity == NET_DNS_CACHE_SIZE);
    assert(status.entries == NET_DNS_CACHE_SIZE);
    assert(status.positive_entries == NET_DNS_CACHE_SIZE);
    assert(status.evictions == 1u);
    assert(status.misses == NET_DNS_CACHE_SIZE + 1u);

    struct net_dns_cache_entry_info entries[NET_DNS_CACHE_SIZE];
    assert(net_dns_cache_snapshot(&stack, entries,
                                  NET_DNS_CACHE_SIZE) ==
           NET_DNS_CACHE_SIZE);
    bool found_oldest = false;
    for (size_t i = 0; i < NET_DNS_CACHE_SIZE; i++) {
        if (strcmp(entries[i].name, "n0.example") == 0) {
            found_oldest = true;
        }
    }
    assert(!found_oldest);
    net_dns_cache_clear(&stack);
    assert(net_dns_cache_get_status(&stack).entries == 0u);
    assert(net_dns_cache_get_status(&stack).evictions == 1u);
}

static void test_malformed_packets_are_rejected(void) {
    struct net_stack stack;
    struct fake_link link;
    struct udp_capture capture = {0};
    init_test_stack(&stack, &link);
    configure_test_ipv4(&stack);
    assert(net_udp_bind(&stack, 4321u, udp_handler, &capture));

    uint8_t frame[NET_ETHERNET_FRAME_MAX];
    const uint8_t data[] = {1u, 2u, 3u};
    size_t frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                        1234u, 4321u, data, sizeof(data), true);
    frame[24u] ^= 1u;
    assert(!net_input(&stack, frame, frame_size));

    frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                        1234u, 4321u, data, sizeof(data), true);
    write_u16_be(frame + 20u, 0x2000u);
    write_u16_be(frame + 24u, 0u);
    write_u16_be(frame + 24u, checksum(frame + 14u, 20u));
    assert(!net_input(&stack, frame, frame_size));

    frame_size =
        build_udp_frame(frame, LOCAL_MAC, PEER_MAC,
                        NET_IPV4(10, 0, 2, 2), NET_IPV4(10, 0, 2, 15),
                        1234u, 4321u, data, sizeof(data), true);
    write_u16_be(frame + 38u, 7u);
    assert(!net_input(&stack, frame, frame_size));
    assert(capture.calls == 0u);
    assert(net_get_stats(&stack)->frames_dropped == 3u);
}

int main(void) {
    test_initialization_and_configuration();
    test_arp_and_udp_output();
    test_resolved_arp_retries_ipv4_not_arp();
    test_udp_poll_and_tcp_dispatch();
    test_icmp_echo();
    test_dhcp_handshake_and_renewal();
    test_dns_resolution_and_timeout();
    test_dns_positive_cache_and_expiry();
    test_dns_multi_address_cache_uses_lowest_usable_ttl();
    test_dns_cname_validation();
    test_dns_safe_negative_cache();
    test_dns_cname_caps_negative_cache_ttl();
    test_dns_server_errors_are_not_negative_answers();
    test_dns_cancel_stops_retries_and_late_completion();
    test_dns_cache_is_bounded();
    test_malformed_packets_are_rejected();
    puts("network protocol tests passed");
    return 0;
}
