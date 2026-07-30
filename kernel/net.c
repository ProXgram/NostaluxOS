#include "net.h"

#include <limits.h>

#define ETHERTYPE_IPV4 0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ARP_HARDWARE_ETHERNET 1u
#define ARP_OPERATION_REQUEST 1u
#define ARP_OPERATION_REPLY 2u
#define ARP_PACKET_SIZE 28u

#define IPV4_VERSION_IHL 0x45u
#define IPV4_DEFAULT_TTL 64u
#define IPV4_FLAG_DONT_FRAGMENT 0x4000u

#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO_REQUEST 8u

#define DHCP_CLIENT_PORT 68u
#define DHCP_SERVER_PORT 67u
#define DHCP_FIXED_SIZE 240u
#define DHCP_PACKET_CAPACITY 576u
#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_MESSAGE_DISCOVER 1u
#define DHCP_MESSAGE_OFFER 2u
#define DHCP_MESSAGE_REQUEST 3u
#define DHCP_MESSAGE_ACK 5u
#define DHCP_MESSAGE_NAK 6u
#define DHCP_OPTION_PAD 0u
#define DHCP_OPTION_SUBNET_MASK 1u
#define DHCP_OPTION_ROUTER 3u
#define DHCP_OPTION_DNS 6u
#define DHCP_OPTION_REQUESTED_IP 50u
#define DHCP_OPTION_LEASE_TIME 51u
#define DHCP_OPTION_MESSAGE_TYPE 53u
#define DHCP_OPTION_SERVER_IDENTIFIER 54u
#define DHCP_OPTION_PARAMETER_REQUEST 55u
#define DHCP_OPTION_MAXIMUM_SIZE 57u
#define DHCP_OPTION_RENEWAL_TIME 58u
#define DHCP_OPTION_REBIND_TIME 59u
#define DHCP_OPTION_CLIENT_IDENTIFIER 61u
#define DHCP_OPTION_END 255u

#define DNS_SERVER_PORT 53u
#define DNS_HEADER_SIZE 12u
#define DNS_TYPE_A 1u
#define DNS_TYPE_CNAME 5u
#define DNS_TYPE_SOA 6u
#define DNS_CLASS_IN 1u
#define DNS_FLAG_RECURSION_DESIRED 0x0100u
#define DNS_FLAG_RESPONSE 0x8000u
#define DNS_FLAG_TRUNCATED 0x0200u
#define DNS_QUERY_CAPACITY (DNS_HEADER_SIZE + NET_DNS_NAME_MAX + 6u)
#define DNS_MAX_CNAME_HOPS 8u

#define ARP_ENTRY_LIFETIME_MS 120000u
#define ARP_RETRY_INTERVAL_MS 1000u
#define ARP_MAX_ATTEMPTS 3u
#define DHCP_RETRY_BASE_MS 4000u
#define DHCP_DEFAULT_LEASE_SECONDS 3600u
#define DNS_RETRY_INTERVAL_MS 2000u
#define DNS_MAX_ATTEMPTS 3u
#define ICMP_ECHO_TIMEOUT_MS 10000u
#define NET_POLL_BUDGET 32u

struct dhcp_options {
    bool has_message_type;
    uint8_t message_type;
    bool has_subnet_mask;
    uint32_t subnet_mask;
    bool has_router;
    uint32_t router;
    bool has_dns;
    uint32_t dns;
    bool has_server_identifier;
    uint32_t server_identifier;
    bool has_lease_time;
    uint32_t lease_time;
    bool has_renewal_time;
    uint32_t renewal_time;
    bool has_rebind_time;
    uint32_t rebind_time;
};

struct dns_record_view {
    size_t owner_offset;
    size_t data_offset;
    size_t next_offset;
    uint16_t type;
    uint16_t record_class;
    uint16_t data_size;
    uint32_t ttl_seconds;
};

enum dns_answer_search_result {
    DNS_ANSWER_NOT_FOUND = 0,
    DNS_ANSWER_ADDRESS,
    DNS_ANSWER_MALFORMED,
};

static void bytes_zero(void* destination, size_t size) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t i = 0; i < size; i++) bytes[i] = 0;
}

static void bytes_copy(void* destination, const void* source, size_t size) {
    uint8_t* output = (uint8_t*)destination;
    const uint8_t* input = (const uint8_t*)source;
    if (output == input || size == 0u) return;

    uintptr_t output_address = (uintptr_t)output;
    uintptr_t input_address = (uintptr_t)input;
    if (output_address <= input_address ||
        output_address - input_address >= size) {
        for (size_t i = 0; i < size; i++) output[i] = input[i];
    } else {
        for (size_t i = size; i > 0u; i--) output[i - 1u] = input[i - 1u];
    }
}

static bool bytes_equal(const void* left, const void* right, size_t size) {
    const uint8_t* a = (const uint8_t*)left;
    const uint8_t* b = (const uint8_t*)right;
    uint8_t difference = 0;
    for (size_t i = 0; i < size; i++) difference |= (uint8_t)(a[i] ^ b[i]);
    return difference == 0u;
}

static uint16_t read_u16_be(const uint8_t* bytes) {
    return (uint16_t)((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1];
}

static uint32_t read_u32_be(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
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

static uint32_t checksum_add(uint32_t sum, const uint8_t* bytes, size_t size) {
    while (size >= 2u) {
        sum += read_u16_be(bytes);
        bytes += 2;
        size -= 2u;
    }
    if (size != 0u) sum += (uint32_t)bytes[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while ((sum >> 16) != 0u) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t internet_checksum(const uint8_t* bytes, size_t size) {
    return checksum_finish(checksum_add(0u, bytes, size));
}

static uint16_t transport_checksum(uint32_t source_ip,
                                   uint32_t destination_ip, uint8_t protocol,
                                   const uint8_t* bytes, size_t size) {
    uint8_t pseudo_header[12];
    write_u32_be(pseudo_header, source_ip);
    write_u32_be(pseudo_header + 4u, destination_ip);
    pseudo_header[8] = 0u;
    pseudo_header[9] = protocol;
    write_u16_be(pseudo_header + 10u, (uint16_t)size);

    uint32_t sum = checksum_add(0u, pseudo_header, sizeof(pseudo_header));
    sum = checksum_add(sum, bytes, size);
    return checksum_finish(sum);
}

static bool mac_is_broadcast(const uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]) {
    static const uint8_t broadcast[NET_ETHERNET_ADDRESS_SIZE] = {
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    };
    return bytes_equal(mac, broadcast, sizeof(broadcast));
}

static bool mac_is_zero(const uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]) {
    static const uint8_t zero[NET_ETHERNET_ADDRESS_SIZE] = {0};
    return bytes_equal(mac, zero, sizeof(zero));
}

static bool mac_is_valid_source(
    const uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]) {
    return !mac_is_zero(mac) && !mac_is_broadcast(mac) &&
           (mac[0] & 1u) == 0u;
}

static bool ipv4_is_multicast(uint32_t ip) {
    return (ip & 0xF0000000u) == 0xE0000000u;
}

static bool ipv4_is_usable_unicast(uint32_t ip) {
    uint8_t first_octet = (uint8_t)(ip >> 24);
    return first_octet != 0u && first_octet != 127u &&
           first_octet < 224u && ip != UINT32_MAX;
}

static bool ipv4_netmask_is_valid(uint32_t mask) {
    if (mask == 0u) return false;
    uint32_t host_bits = ~mask;
    return (host_bits & (host_bits + 1u)) == 0u;
}

static bool ipv4_is_subnet_broadcast(const struct net_stack* stack,
                                     uint32_t ip) {
    uint32_t mask = stack->ipv4.netmask;
    if (stack->ipv4.address == 0u || mask == 0u || mask == UINT32_MAX) {
        return false;
    }
    return ip == ((stack->ipv4.address & mask) | ~mask);
}

static bool ipv4_is_broadcast(const struct net_stack* stack, uint32_t ip) {
    return ip == UINT32_MAX || ipv4_is_subnet_broadcast(stack, ip);
}

static bool ipv4_is_on_link(const struct net_stack* stack, uint32_t ip) {
    if (stack->ipv4.address == 0u) return false;
    return stack->ipv4.netmask == 0u ||
           (stack->ipv4.address & stack->ipv4.netmask) ==
               (ip & stack->ipv4.netmask);
}

static bool ipv4_destination_is_local(const struct net_stack* stack,
                                      uint32_t ip) {
    if (ip == UINT32_MAX || ipv4_is_subnet_broadcast(stack, ip)) return true;
    return stack->ipv4.address != 0u && ip == stack->ipv4.address;
}

static uint64_t saturating_add_u64(uint64_t value, uint64_t addition) {
    return addition > UINT64_MAX - value ? UINT64_MAX : value + addition;
}

static uint64_t seconds_to_ms(uint32_t seconds) {
    return (uint64_t)seconds * 1000u;
}

static bool deadline_reached(uint64_t now_ms, uint64_t deadline_ms) {
    return now_ms >= deadline_ms;
}

static bool link_transmit(struct net_stack* stack, size_t frame_size) {
    if (stack == NULL || stack->link.send == NULL ||
        frame_size < NET_ETHERNET_HEADER_SIZE ||
        frame_size > NET_ETHERNET_FRAME_MAX) {
        return false;
    }
    if (!stack->link.send(stack->link.context, stack->transmit_frame,
                          frame_size)) {
        return false;
    }
    stack->stats.frames_transmitted++;
    return true;
}

static void write_ethernet_header(
    struct net_stack* stack,
    const uint8_t destination[NET_ETHERNET_ADDRESS_SIZE], uint16_t ethertype) {
    bytes_copy(stack->transmit_frame, destination, NET_ETHERNET_ADDRESS_SIZE);
    bytes_copy(stack->transmit_frame + NET_ETHERNET_ADDRESS_SIZE, stack->mac,
               NET_ETHERNET_ADDRESS_SIZE);
    write_u16_be(stack->transmit_frame + 12u, ethertype);
}

static struct net_arp_entry* arp_find(struct net_stack* stack, uint32_t ip) {
    for (size_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        if (stack->arp[i].valid && stack->arp[i].ip == ip) {
            return &stack->arp[i];
        }
    }
    return NULL;
}

static const struct net_arp_entry* arp_find_const(
    const struct net_stack* stack, uint32_t ip) {
    for (size_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        if (stack->arp[i].valid && stack->arp[i].ip == ip) {
            return &stack->arp[i];
        }
    }
    return NULL;
}

static void arp_learn(struct net_stack* stack, uint32_t ip,
                      const uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]) {
    if (!ipv4_is_usable_unicast(ip) || !mac_is_valid_source(mac)) return;

    struct net_arp_entry* entry = arp_find(stack, ip);
    if (entry == NULL) {
        size_t selected = 0u;
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
            if (!stack->arp[i].valid) {
                selected = i;
                oldest = 0u;
                break;
            }
            if (stack->arp[i].refreshed_ms < oldest) {
                selected = i;
                oldest = stack->arp[i].refreshed_ms;
            }
        }
        entry = &stack->arp[selected];
    }

    entry->valid = true;
    entry->ip = ip;
    bytes_copy(entry->mac, mac, NET_ETHERNET_ADDRESS_SIZE);
    entry->refreshed_ms = stack->current_time_ms;
}

bool net_arp_lookup(const struct net_stack* stack, uint32_t ip,
                    uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]) {
    if (stack == NULL || mac == NULL) return false;
    const struct net_arp_entry* entry = arp_find_const(stack, ip);
    if (entry == NULL) return false;
    bytes_copy(mac, entry->mac, NET_ETHERNET_ADDRESS_SIZE);
    return true;
}

bool net_arp_request(struct net_stack* stack, uint32_t ip) {
    if (stack == NULL || stack->link.send == NULL ||
        stack->ipv4.address == 0u || !ipv4_is_usable_unicast(ip)) {
        return false;
    }

    static const uint8_t broadcast[NET_ETHERNET_ADDRESS_SIZE] = {
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    };
    uint8_t* arp = stack->transmit_frame + NET_ETHERNET_HEADER_SIZE;
    write_ethernet_header(stack, broadcast, ETHERTYPE_ARP);
    write_u16_be(arp, ARP_HARDWARE_ETHERNET);
    write_u16_be(arp + 2u, ETHERTYPE_IPV4);
    arp[4] = NET_ETHERNET_ADDRESS_SIZE;
    arp[5] = 4u;
    write_u16_be(arp + 6u, ARP_OPERATION_REQUEST);
    bytes_copy(arp + 8u, stack->mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(arp + 14u, stack->ipv4.address);
    bytes_zero(arp + 18u, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(arp + 24u, ip);

    if (!link_transmit(stack, NET_ETHERNET_HEADER_SIZE + ARP_PACKET_SIZE)) {
        return false;
    }
    stack->stats.arp_requests_sent++;
    return true;
}

static bool send_arp_reply(
    struct net_stack* stack,
    const uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE],
    uint32_t destination_ip) {
    uint8_t* arp = stack->transmit_frame + NET_ETHERNET_HEADER_SIZE;
    write_ethernet_header(stack, destination_mac, ETHERTYPE_ARP);
    write_u16_be(arp, ARP_HARDWARE_ETHERNET);
    write_u16_be(arp + 2u, ETHERTYPE_IPV4);
    arp[4] = NET_ETHERNET_ADDRESS_SIZE;
    arp[5] = 4u;
    write_u16_be(arp + 6u, ARP_OPERATION_REPLY);
    bytes_copy(arp + 8u, stack->mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(arp + 14u, stack->ipv4.address);
    bytes_copy(arp + 18u, destination_mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(arp + 24u, destination_ip);
    return link_transmit(stack, NET_ETHERNET_HEADER_SIZE + ARP_PACKET_SIZE);
}

static bool route_ipv4(struct net_stack* stack, uint32_t destination_ip,
                       uint32_t* next_hop_ip,
                       uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE],
                       bool* needs_arp) {
    if (destination_ip == 0u || next_hop_ip == NULL ||
        destination_mac == NULL || needs_arp == NULL) {
        return false;
    }

    if (ipv4_is_broadcast(stack, destination_ip)) {
        for (size_t i = 0; i < NET_ETHERNET_ADDRESS_SIZE; i++) {
            destination_mac[i] = 0xFFu;
        }
        *next_hop_ip = destination_ip;
        *needs_arp = false;
        return true;
    }

    if (ipv4_is_multicast(destination_ip)) {
        destination_mac[0] = 0x01u;
        destination_mac[1] = 0x00u;
        destination_mac[2] = 0x5Eu;
        destination_mac[3] = (uint8_t)((destination_ip >> 16) & 0x7Fu);
        destination_mac[4] = (uint8_t)(destination_ip >> 8);
        destination_mac[5] = (uint8_t)destination_ip;
        *next_hop_ip = destination_ip;
        *needs_arp = false;
        return true;
    }

    if (stack->ipv4.address == 0u) return false;

    uint32_t target = destination_ip;
    if (stack->ipv4.netmask != 0u &&
        (destination_ip & stack->ipv4.netmask) !=
            (stack->ipv4.address & stack->ipv4.netmask)) {
        target = stack->ipv4.gateway;
    }
    if (!ipv4_is_usable_unicast(target)) return false;

    *next_hop_ip = target;
    *needs_arp = !net_arp_lookup(stack, target, destination_mac);
    return true;
}

static bool send_ipv4_with_mac(
    struct net_stack* stack, uint32_t destination_ip, uint8_t protocol,
    const uint8_t* payload, size_t payload_size,
    const uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE]) {
    if (payload_size > NET_IPV4_PAYLOAD_MAX ||
        (payload_size != 0u && payload == NULL)) {
        return false;
    }

    uint8_t* ip = stack->transmit_frame + NET_ETHERNET_HEADER_SIZE;
    write_ethernet_header(stack, destination_mac, ETHERTYPE_IPV4);
    ip[0] = IPV4_VERSION_IHL;
    ip[1] = 0u;
    write_u16_be(ip + 2u,
                 (uint16_t)(NET_IPV4_HEADER_MIN_SIZE + payload_size));
    write_u16_be(ip + 4u, stack->next_ipv4_id++);
    write_u16_be(ip + 6u, IPV4_FLAG_DONT_FRAGMENT);
    ip[8] = IPV4_DEFAULT_TTL;
    ip[9] = protocol;
    write_u16_be(ip + 10u, 0u);
    write_u32_be(ip + 12u, stack->ipv4.address);
    write_u32_be(ip + 16u, destination_ip);
    write_u16_be(ip + 10u, internet_checksum(ip, NET_IPV4_HEADER_MIN_SIZE));
    bytes_copy(ip + NET_IPV4_HEADER_MIN_SIZE, payload, payload_size);

    size_t frame_size = NET_ETHERNET_HEADER_SIZE +
                        NET_IPV4_HEADER_MIN_SIZE + payload_size;
    if (!link_transmit(stack, frame_size)) return false;
    stack->stats.ipv4_packets_transmitted++;
    return true;
}

static bool flush_pending_ipv4(struct net_stack* stack) {
    struct net_pending_ipv4* pending = &stack->pending_ipv4;
    if (!pending->valid) return true;

    uint8_t mac[NET_ETHERNET_ADDRESS_SIZE];
    if (!net_arp_lookup(stack, pending->next_hop_ip, mac)) return false;

    if (!send_ipv4_with_mac(stack, pending->destination_ip,
                            pending->protocol, pending->payload,
                            pending->payload_size, mac)) {
        pending->arp_requested_ms = stack->current_time_ms;
        pending->transmit_attempts++;
        return false;
    }
    if (pending->protocol == NET_IPV4_PROTOCOL_UDP) {
        stack->stats.udp_datagrams_transmitted++;
    }
    pending->valid = false;
    return true;
}

enum net_send_result net_ipv4_send(struct net_stack* stack,
                                   uint32_t destination_ip, uint8_t protocol,
                                   const void* payload, size_t payload_size) {
    if (stack == NULL || stack->link.send == NULL || destination_ip == 0u ||
        payload_size > NET_IPV4_PAYLOAD_MAX ||
        (payload_size != 0u && payload == NULL)) {
        return NET_SEND_FAILED;
    }
    if (stack->ipv4.address == 0u && destination_ip != UINT32_MAX) {
        return NET_SEND_FAILED;
    }

    uint32_t next_hop = 0u;
    uint8_t destination_mac[NET_ETHERNET_ADDRESS_SIZE];
    bool needs_arp = false;
    if (!route_ipv4(stack, destination_ip, &next_hop, destination_mac,
                    &needs_arp)) {
        return NET_SEND_FAILED;
    }

    if (!needs_arp) {
        return send_ipv4_with_mac(stack, destination_ip, protocol,
                                  (const uint8_t*)payload, payload_size,
                                  destination_mac)
                   ? NET_SEND_SENT
                   : NET_SEND_FAILED;
    }

    if (stack->pending_ipv4.valid) return NET_SEND_FAILED;
    stack->pending_ipv4.valid = true;
    stack->pending_ipv4.destination_ip = destination_ip;
    stack->pending_ipv4.next_hop_ip = next_hop;
    stack->pending_ipv4.protocol = protocol;
    stack->pending_ipv4.payload_size = payload_size;
    bytes_copy(stack->pending_ipv4.payload, payload, payload_size);
    stack->pending_ipv4.arp_requested_ms = stack->current_time_ms;
    stack->pending_ipv4.arp_attempts = 1u;
    stack->pending_ipv4.transmit_attempts = 0u;
    (void)net_arp_request(stack, next_hop);
    return NET_SEND_QUEUED;
}

static bool udp_checksum_valid(uint32_t source_ip, uint32_t destination_ip,
                               const uint8_t* udp, size_t udp_size) {
    uint16_t supplied = read_u16_be(udp + 6u);
    if (supplied == 0u) return true;
    return transport_checksum(source_ip, destination_ip, NET_IPV4_PROTOCOL_UDP,
                              udp, udp_size) == 0u;
}

enum net_send_result net_udp_send(struct net_stack* stack,
                                  uint32_t destination_ip,
                                  uint16_t destination_port,
                                  uint16_t source_port, const void* data,
                                  size_t data_size) {
    if (stack == NULL || destination_port == 0u || source_port == 0u ||
        data_size > NET_UDP_PAYLOAD_MAX ||
        (data_size != 0u && data == NULL)) {
        return NET_SEND_FAILED;
    }

    uint8_t* udp = stack->transmit_frame + NET_ETHERNET_HEADER_SIZE +
                   NET_IPV4_HEADER_MIN_SIZE;
    write_u16_be(udp, source_port);
    write_u16_be(udp + 2u, destination_port);
    write_u16_be(udp + 4u, (uint16_t)(8u + data_size));
    write_u16_be(udp + 6u, 0u);
    bytes_copy(udp + 8u, data, data_size);
    uint16_t checksum =
        transport_checksum(stack->ipv4.address, destination_ip,
                           NET_IPV4_PROTOCOL_UDP, udp, 8u + data_size);
    write_u16_be(udp + 6u, checksum == 0u ? 0xFFFFu : checksum);

    enum net_send_result result =
        net_ipv4_send(stack, destination_ip, NET_IPV4_PROTOCOL_UDP, udp,
                      8u + data_size);
    if (result == NET_SEND_SENT) stack->stats.udp_datagrams_transmitted++;
    return result;
}

bool net_udp_bind(struct net_stack* stack, uint16_t port,
                  net_udp_handler_fn handler, void* context) {
    if (stack == NULL || port == 0u || handler == NULL ||
        port == DHCP_CLIENT_PORT) {
        return false;
    }

    for (size_t i = 0; i < NET_UDP_BINDING_COUNT; i++) {
        if (stack->udp_bindings[i].handler != NULL &&
            stack->udp_bindings[i].port == port) {
            return false;
        }
    }
    for (size_t i = 0; i < NET_UDP_BINDING_COUNT; i++) {
        if (stack->udp_bindings[i].handler == NULL) {
            stack->udp_bindings[i].port = port;
            stack->udp_bindings[i].handler = handler;
            stack->udp_bindings[i].context = context;
            return true;
        }
    }
    return false;
}

void net_udp_unbind(struct net_stack* stack, uint16_t port) {
    if (stack == NULL) return;
    for (size_t i = 0; i < NET_UDP_BINDING_COUNT; i++) {
        if (stack->udp_bindings[i].handler != NULL &&
            stack->udp_bindings[i].port == port) {
            bytes_zero(&stack->udp_bindings[i],
                       sizeof(stack->udp_bindings[i]));
        }
    }
}

void net_set_tcp_handler(struct net_stack* stack,
                         net_ipv4_handler_fn handler, void* context) {
    if (stack == NULL) return;
    stack->tcp_handler = handler;
    stack->tcp_handler_context = handler == NULL ? NULL : context;
}

static void dhcp_reset_offer(struct net_dhcp_client* dhcp) {
    dhcp->offered_ip = 0u;
    dhcp->server_ip = 0u;
    dhcp->offered_netmask = 0u;
    dhcp->offered_gateway = 0u;
    dhcp->offered_dns = 0u;
    dhcp->lease_seconds = 0u;
    dhcp->renewal_seconds = 0u;
    dhcp->rebind_seconds = 0u;
}

static size_t dhcp_append_option(uint8_t* packet, size_t offset,
                                 size_t capacity, uint8_t option,
                                 const void* value, uint8_t value_size) {
    if (offset > capacity || (size_t)value_size + 2u > capacity - offset) {
        return 0u;
    }
    packet[offset++] = option;
    packet[offset++] = value_size;
    bytes_copy(packet + offset, value, value_size);
    return offset + value_size;
}

static size_t dhcp_append_u32_option(uint8_t* packet, size_t offset,
                                     size_t capacity, uint8_t option,
                                     uint32_t value) {
    uint8_t encoded[4];
    write_u32_be(encoded, value);
    return dhcp_append_option(packet, offset, capacity, option, encoded,
                              sizeof(encoded));
}

static uint64_t dhcp_retry_delay(unsigned int attempts) {
    unsigned int shift = attempts > 4u ? 4u : attempts;
    return (uint64_t)DHCP_RETRY_BASE_MS << shift;
}

static bool dhcp_send_message(struct net_stack* stack, uint8_t message_type,
                              bool using_active_lease) {
    uint8_t packet[DHCP_PACKET_CAPACITY];
    bytes_zero(packet, sizeof(packet));
    packet[0] = 1u;
    packet[1] = 1u;
    packet[2] = NET_ETHERNET_ADDRESS_SIZE;
    write_u32_be(packet + 4u, stack->dhcp.transaction_id);
    if (using_active_lease) {
        write_u32_be(packet + 12u, stack->ipv4.address);
    }
    if (!using_active_lease ||
        stack->dhcp.state == NET_DHCP_REBINDING) {
        write_u16_be(packet + 10u, 0x8000u);
    }
    bytes_copy(packet + 28u, stack->mac, NET_ETHERNET_ADDRESS_SIZE);
    write_u32_be(packet + 236u, DHCP_MAGIC_COOKIE);

    size_t offset = DHCP_FIXED_SIZE;
    offset = dhcp_append_option(packet, offset, sizeof(packet),
                                DHCP_OPTION_MESSAGE_TYPE, &message_type, 1u);
    if (offset == 0u) return false;

    uint8_t client_id[1u + NET_ETHERNET_ADDRESS_SIZE];
    client_id[0] = 1u;
    bytes_copy(client_id + 1u, stack->mac, NET_ETHERNET_ADDRESS_SIZE);
    offset = dhcp_append_option(packet, offset, sizeof(packet),
                                DHCP_OPTION_CLIENT_IDENTIFIER, client_id,
                                sizeof(client_id));
    if (offset == 0u) return false;

    if (message_type == DHCP_MESSAGE_REQUEST && !using_active_lease) {
        offset = dhcp_append_u32_option(packet, offset, sizeof(packet),
                                        DHCP_OPTION_REQUESTED_IP,
                                        stack->dhcp.offered_ip);
        if (offset == 0u) return false;
        if (stack->dhcp.state != NET_DHCP_REBINDING &&
            stack->dhcp.server_ip != 0u) {
            offset = dhcp_append_u32_option(
                packet, offset, sizeof(packet),
                DHCP_OPTION_SERVER_IDENTIFIER, stack->dhcp.server_ip);
            if (offset == 0u) return false;
        }
    }

    uint8_t requested[] = {
        DHCP_OPTION_SUBNET_MASK, DHCP_OPTION_ROUTER, DHCP_OPTION_DNS,
        DHCP_OPTION_LEASE_TIME, DHCP_OPTION_RENEWAL_TIME,
        DHCP_OPTION_REBIND_TIME,
    };
    offset = dhcp_append_option(packet, offset, sizeof(packet),
                                DHCP_OPTION_PARAMETER_REQUEST, requested,
                                sizeof(requested));
    if (offset == 0u) return false;
    uint8_t maximum_size[2];
    write_u16_be(maximum_size, DHCP_PACKET_CAPACITY);
    offset = dhcp_append_option(packet, offset, sizeof(packet),
                                DHCP_OPTION_MAXIMUM_SIZE, maximum_size,
                                sizeof(maximum_size));
    if (offset == 0u || offset >= sizeof(packet)) return false;
    packet[offset++] = DHCP_OPTION_END;

    uint32_t destination_ip =
        stack->dhcp.state == NET_DHCP_RENEWING &&
                stack->dhcp.server_ip != 0u
            ? stack->dhcp.server_ip
            : UINT32_MAX;
    enum net_send_result result =
        net_udp_send(stack, destination_ip, DHCP_SERVER_PORT,
                     DHCP_CLIENT_PORT, packet, offset);
    stack->dhcp.attempts++;
    stack->dhcp.next_action_ms = saturating_add_u64(
        stack->current_time_ms, dhcp_retry_delay(stack->dhcp.attempts - 1u));
    return result != NET_SEND_FAILED;
}

static bool parse_dhcp_options(const uint8_t* packet, size_t packet_size,
                               struct dhcp_options* options) {
    if (packet_size < DHCP_FIXED_SIZE ||
        read_u32_be(packet + 236u) != DHCP_MAGIC_COOKIE ||
        options == NULL) {
        return false;
    }
    bytes_zero(options, sizeof(*options));

    size_t offset = DHCP_FIXED_SIZE;
    while (offset < packet_size) {
        uint8_t code = packet[offset++];
        if (code == DHCP_OPTION_PAD) continue;
        if (code == DHCP_OPTION_END) return options->has_message_type;
        if (offset >= packet_size) return false;
        uint8_t length = packet[offset++];
        if ((size_t)length > packet_size - offset) return false;
        const uint8_t* value = packet + offset;

        switch (code) {
            case DHCP_OPTION_MESSAGE_TYPE:
                if (length == 1u && !options->has_message_type) {
                    options->has_message_type = true;
                    options->message_type = value[0];
                }
                break;
            case DHCP_OPTION_SUBNET_MASK:
                if (length == 4u && !options->has_subnet_mask) {
                    options->has_subnet_mask = true;
                    options->subnet_mask = read_u32_be(value);
                }
                break;
            case DHCP_OPTION_ROUTER:
                if (length >= 4u && !options->has_router) {
                    options->has_router = true;
                    options->router = read_u32_be(value);
                }
                break;
            case DHCP_OPTION_DNS:
                if (length >= 4u && !options->has_dns) {
                    options->has_dns = true;
                    options->dns = read_u32_be(value);
                }
                break;
            case DHCP_OPTION_SERVER_IDENTIFIER:
                if (length == 4u && !options->has_server_identifier) {
                    options->has_server_identifier = true;
                    options->server_identifier = read_u32_be(value);
                }
                break;
            case DHCP_OPTION_LEASE_TIME:
                if (length == 4u && !options->has_lease_time) {
                    options->has_lease_time = true;
                    options->lease_time = read_u32_be(value);
                }
                break;
            case DHCP_OPTION_RENEWAL_TIME:
                if (length == 4u && !options->has_renewal_time) {
                    options->has_renewal_time = true;
                    options->renewal_time = read_u32_be(value);
                }
                break;
            case DHCP_OPTION_REBIND_TIME:
                if (length == 4u && !options->has_rebind_time) {
                    options->has_rebind_time = true;
                    options->rebind_time = read_u32_be(value);
                }
                break;
            default:
                break;
        }
        offset += length;
    }
    return false;
}

static void dhcp_restart(struct net_stack* stack) {
    uint32_t transaction_id = stack->dhcp.transaction_id + 1u;
    if (transaction_id == 0u) transaction_id = 1u;
    (void)net_dhcp_start(stack, transaction_id);
}

static bool dhcp_receive(struct net_stack* stack, uint32_t source_ip,
                         const uint8_t* packet, size_t packet_size) {
    if (stack->dhcp.state == NET_DHCP_OFF ||
        packet_size < DHCP_FIXED_SIZE || packet[0] != 2u ||
        packet[1] != 1u || packet[2] != NET_ETHERNET_ADDRESS_SIZE ||
        read_u32_be(packet + 4u) != stack->dhcp.transaction_id ||
        !bytes_equal(packet + 28u, stack->mac, NET_ETHERNET_ADDRESS_SIZE)) {
        return false;
    }

    struct dhcp_options options;
    if (!parse_dhcp_options(packet, packet_size, &options)) return false;
    uint32_t offered_ip = read_u32_be(packet + 16u);
    uint32_t server_ip = options.has_server_identifier
                             ? options.server_identifier
                             : source_ip;

    if (options.message_type == DHCP_MESSAGE_OFFER &&
        stack->dhcp.state == NET_DHCP_DISCOVERING &&
        ipv4_is_usable_unicast(offered_ip) &&
        ipv4_is_usable_unicast(server_ip)) {
        stack->dhcp.offered_ip = offered_ip;
        stack->dhcp.server_ip = server_ip;
        stack->dhcp.offered_netmask =
            options.has_subnet_mask ? options.subnet_mask : 0u;
        stack->dhcp.offered_gateway =
            options.has_router ? options.router : 0u;
        stack->dhcp.offered_dns = options.has_dns ? options.dns : 0u;
        stack->dhcp.lease_seconds =
            options.has_lease_time ? options.lease_time
                                   : DHCP_DEFAULT_LEASE_SECONDS;
        stack->dhcp.renewal_seconds =
            options.has_renewal_time
                ? options.renewal_time
                : stack->dhcp.lease_seconds / 2u;
        stack->dhcp.rebind_seconds =
            options.has_rebind_time
                ? options.rebind_time
                : (uint32_t)(((uint64_t)stack->dhcp.lease_seconds * 7u) / 8u);
        stack->dhcp.state = NET_DHCP_REQUESTING;
        stack->dhcp.attempts = 0u;
        (void)dhcp_send_message(stack, DHCP_MESSAGE_REQUEST, false);
        return true;
    }

    if (options.message_type == DHCP_MESSAGE_NAK &&
        (stack->dhcp.state == NET_DHCP_REQUESTING ||
         stack->dhcp.state == NET_DHCP_RENEWING ||
         stack->dhcp.state == NET_DHCP_REBINDING)) {
        if (stack->dhcp.server_ip != 0u &&
            options.has_server_identifier &&
            options.server_identifier != stack->dhcp.server_ip &&
            stack->dhcp.state != NET_DHCP_REBINDING) {
            return false;
        }
        dhcp_restart(stack);
        return true;
    }

    if (options.message_type != DHCP_MESSAGE_ACK ||
        (stack->dhcp.state != NET_DHCP_REQUESTING &&
         stack->dhcp.state != NET_DHCP_RENEWING &&
         stack->dhcp.state != NET_DHCP_REBINDING)) {
        return false;
    }
    if (stack->dhcp.state != NET_DHCP_REBINDING &&
        stack->dhcp.server_ip != 0u && options.has_server_identifier &&
        options.server_identifier != stack->dhcp.server_ip) {
        return false;
    }

    uint32_t address =
        ipv4_is_usable_unicast(offered_ip) ? offered_ip : stack->ipv4.address;
    if (!ipv4_is_usable_unicast(address)) address = stack->dhcp.offered_ip;
    if (!ipv4_is_usable_unicast(address)) return false;

    struct net_ipv4_config configuration = {
        .address = address,
        .netmask = options.has_subnet_mask
                       ? options.subnet_mask
                       : stack->dhcp.offered_netmask,
        .gateway =
            options.has_router ? options.router : stack->dhcp.offered_gateway,
        .dns = options.has_dns ? options.dns : stack->dhcp.offered_dns,
    };
    if (!ipv4_netmask_is_valid(configuration.netmask)) {
        configuration.netmask = NET_IPV4(255, 255, 255, 0);
    }
    if (!ipv4_is_usable_unicast(configuration.gateway)) {
        configuration.gateway = 0u;
    }
    if (!ipv4_is_usable_unicast(configuration.dns)) {
        configuration.dns = 0u;
    }

    stack->dhcp.server_ip = ipv4_is_usable_unicast(server_ip)
                                ? server_ip
                                : stack->dhcp.server_ip;
    stack->dhcp.lease_seconds =
        options.has_lease_time
            ? options.lease_time
            : (stack->dhcp.lease_seconds != 0u
                   ? stack->dhcp.lease_seconds
                   : DHCP_DEFAULT_LEASE_SECONDS);
    if (stack->dhcp.lease_seconds == 0u) return false;
    stack->dhcp.renewal_seconds =
        options.has_renewal_time
            ? options.renewal_time
            : stack->dhcp.lease_seconds / 2u;
    stack->dhcp.rebind_seconds =
        options.has_rebind_time
            ? options.rebind_time
            : (uint32_t)(((uint64_t)stack->dhcp.lease_seconds * 7u) / 8u);
    if (stack->dhcp.renewal_seconds >= stack->dhcp.lease_seconds) {
        stack->dhcp.renewal_seconds = stack->dhcp.lease_seconds / 2u;
    }
    if (stack->dhcp.rebind_seconds <= stack->dhcp.renewal_seconds ||
        stack->dhcp.rebind_seconds >= stack->dhcp.lease_seconds) {
        stack->dhcp.rebind_seconds =
            (uint32_t)(((uint64_t)stack->dhcp.lease_seconds * 7u) / 8u);
    }

    net_configure_ipv4(stack, &configuration);
    stack->dhcp.state = NET_DHCP_BOUND;
    stack->dhcp.lease_started_ms = stack->current_time_ms;
    stack->dhcp.next_action_ms = saturating_add_u64(
        stack->current_time_ms,
        seconds_to_ms(stack->dhcp.renewal_seconds));
    stack->dhcp.attempts = 0u;
    return true;
}

bool net_dhcp_start(struct net_stack* stack, uint32_t transaction_id) {
    if (stack == NULL || stack->link.send == NULL) return false;
    if (transaction_id == 0u) {
        transaction_id = 2166136261u;
        for (size_t i = 0; i < NET_ETHERNET_ADDRESS_SIZE; i++) {
            transaction_id ^= stack->mac[i];
            transaction_id *= 16777619u;
        }
        transaction_id ^= (uint32_t)stack->current_time_ms;
        if (transaction_id == 0u) transaction_id = 1u;
    }

    net_clear_ipv4(stack);
    bytes_zero(&stack->dhcp, sizeof(stack->dhcp));
    stack->dhcp.state = NET_DHCP_DISCOVERING;
    stack->dhcp.transaction_id = transaction_id;
    stack->dhcp.attempts = 0u;
    dhcp_reset_offer(&stack->dhcp);
    (void)dhcp_send_message(stack, DHCP_MESSAGE_DISCOVER, false);
    return true;
}

void net_dhcp_stop(struct net_stack* stack) {
    if (stack == NULL) return;
    bytes_zero(&stack->dhcp, sizeof(stack->dhcp));
}

enum net_dhcp_state net_dhcp_get_state(const struct net_stack* stack) {
    return stack == NULL ? NET_DHCP_OFF : stack->dhcp.state;
}

static bool dns_validate_and_copy_name(char destination[NET_DNS_NAME_MAX + 1u],
                                       const char* name) {
    if (name == NULL || *name == '\0') return false;
    size_t length = 0u;
    size_t label_length = 0u;
    while (name[length] != '\0') {
        unsigned char character = (unsigned char)name[length];
        if (length >= NET_DNS_NAME_MAX) return false;
        if (character == '.') {
            if (label_length == 0u || label_length > 63u) return false;
            if (name[length + 1u] == '\0') {
                destination[length] = '\0';
                return true;
            }
            label_length = 0u;
        } else {
            if (character <= 0x20u || character >= 0x7Fu) return false;
            label_length++;
            if (label_length > 63u) return false;
        }
        destination[length] = (char)character;
        length++;
    }
    if (label_length == 0u || label_length > 63u) return false;
    destination[length] = '\0';
    return true;
}

static size_t dns_encode_name(uint8_t* output, size_t capacity,
                              const char* name) {
    size_t output_offset = 0u;
    size_t label_start = 0u;
    size_t cursor = 0u;

    while (true) {
        char character = name[cursor];
        if (character == '.' || character == '\0') {
            size_t label_size = cursor - label_start;
            if (label_size == 0u || label_size > 63u ||
                output_offset + 1u + label_size >= capacity) {
                return 0u;
            }
            output[output_offset++] = (uint8_t)label_size;
            bytes_copy(output + output_offset, name + label_start, label_size);
            output_offset += label_size;
            if (character == '\0') break;
            label_start = cursor + 1u;
        }
        cursor++;
    }
    if (output_offset >= capacity) return 0u;
    output[output_offset++] = 0u;
    return output_offset;
}

static bool dns_send_query(struct net_stack* stack) {
    uint8_t query[DNS_QUERY_CAPACITY];
    bytes_zero(query, sizeof(query));
    write_u16_be(query, stack->dns.transaction_id);
    write_u16_be(query + 2u, DNS_FLAG_RECURSION_DESIRED);
    write_u16_be(query + 4u, 1u);
    size_t name_size =
        dns_encode_name(query + DNS_HEADER_SIZE,
                        sizeof(query) - DNS_HEADER_SIZE, stack->dns.name);
    if (name_size == 0u ||
        DNS_HEADER_SIZE + name_size + 4u > sizeof(query)) {
        return false;
    }
    size_t size = DNS_HEADER_SIZE + name_size;
    write_u16_be(query + size, DNS_TYPE_A);
    write_u16_be(query + size + 2u, DNS_CLASS_IN);
    size += 4u;

    enum net_send_result result =
        net_udp_send(stack, stack->ipv4.dns, DNS_SERVER_PORT,
                     stack->dns.source_port, query, size);
    stack->dns.attempts++;
    stack->dns.next_action_ms = saturating_add_u64(
        stack->current_time_ms, DNS_RETRY_INTERVAL_MS);
    return result != NET_SEND_FAILED;
}

static char ascii_lower(char character) {
    if (character >= 'A' && character <= 'Z') {
        return (char)(character + ('a' - 'A'));
    }
    return character;
}

static bool dns_names_equal(const char* left, const char* right) {
    size_t index = 0u;
    while (left[index] != '\0' && right[index] != '\0') {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
        index++;
    }
    return left[index] == right[index];
}

static uint32_t dns_cache_remaining_seconds(uint64_t expires_ms,
                                            uint64_t now_ms) {
    if (expires_ms <= now_ms) return 0u;
    uint64_t remaining_ms = expires_ms - now_ms;
    uint64_t seconds = remaining_ms / 1000u;
    if (remaining_ms % 1000u != 0u) seconds++;
    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

static void dns_cache_expire(struct net_stack* stack) {
    for (size_t i = 0; i < NET_DNS_CACHE_SIZE; i++) {
        struct net_dns_cache_entry* entry = &stack->dns_cache[i];
        if (entry->valid && entry->expires_ms <= stack->current_time_ms) {
            entry->valid = false;
            stack->dns_cache_expirations++;
        }
    }
}

static struct net_dns_cache_entry* dns_cache_find(struct net_stack* stack,
                                                   const char* name) {
    for (size_t i = 0; i < NET_DNS_CACHE_SIZE; i++) {
        struct net_dns_cache_entry* entry = &stack->dns_cache[i];
        if (entry->valid && dns_names_equal(entry->name, name)) return entry;
    }
    return NULL;
}

static void dns_cache_store(struct net_stack* stack, const char* name,
                            bool negative, uint32_t address,
                            uint8_t response_code, uint32_t ttl_seconds) {
    if (ttl_seconds == 0u) return;
    dns_cache_expire(stack);

    struct net_dns_cache_entry* selected = dns_cache_find(stack, name);
    if (selected == NULL) {
        uint64_t oldest_use = UINT64_MAX;
        for (size_t i = 0; i < NET_DNS_CACHE_SIZE; i++) {
            struct net_dns_cache_entry* candidate = &stack->dns_cache[i];
            if (!candidate->valid) {
                selected = candidate;
                break;
            }
            if (candidate->last_used_ms < oldest_use) {
                oldest_use = candidate->last_used_ms;
                selected = candidate;
            }
        }
        if (selected != NULL && selected->valid) {
            stack->dns_cache_evictions++;
        }
    }
    if (selected == NULL) return;

    bytes_zero(selected, sizeof(*selected));
    selected->valid = true;
    selected->negative = negative;
    size_t name_length = 0u;
    while (name[name_length] != '\0') name_length++;
    bytes_copy(selected->name, name, name_length + 1u);
    selected->address = negative ? 0u : address;
    selected->response_code = response_code;
    selected->expires_ms = saturating_add_u64(
        stack->current_time_ms, seconds_to_ms(ttl_seconds));
    selected->last_used_ms = stack->current_time_ms;
}

static bool dns_expand_name(const uint8_t* message, size_t message_size,
                            size_t offset,
                            char output[NET_DNS_NAME_MAX + 1u],
                            size_t* next_offset) {
    size_t cursor = offset;
    size_t output_size = 0u;
    size_t resume_offset = 0u;
    bool jumped = false;
    unsigned int hops = 0u;

    while (true) {
        if (cursor >= message_size || hops++ > 128u) return false;
        uint8_t length = message[cursor++];
        if ((length & 0xC0u) == 0xC0u) {
            if (cursor >= message_size) return false;
            uint16_t pointer =
                (uint16_t)(((uint16_t)(length & 0x3Fu) << 8) |
                           message[cursor++]);
            if (pointer >= message_size) return false;
            if (!jumped) resume_offset = cursor;
            jumped = true;
            cursor = pointer;
            continue;
        }
        if ((length & 0xC0u) != 0u || length > 63u) return false;
        if (length == 0u) {
            if (!jumped) resume_offset = cursor;
            break;
        }
        if ((size_t)length > message_size - cursor) return false;
        if (output_size != 0u) {
            if (output_size >= NET_DNS_NAME_MAX) return false;
            output[output_size++] = '.';
        }
        if ((size_t)length > NET_DNS_NAME_MAX - output_size) return false;
        for (size_t i = 0; i < length; i++) {
            unsigned char character = message[cursor + i];
            if (character <= 0x20u || character >= 0x7Fu) return false;
            output[output_size++] = (char)character;
        }
        cursor += length;
    }
    output[output_size] = '\0';
    if (next_offset != NULL) *next_offset = resume_offset;
    return true;
}

static bool dns_skip_name(const uint8_t* message, size_t message_size,
                          size_t* offset) {
    char ignored[NET_DNS_NAME_MAX + 1u];
    size_t next = 0u;
    if (offset == NULL ||
        !dns_expand_name(message, message_size, *offset, ignored, &next)) {
        return false;
    }
    *offset = next;
    return true;
}

static bool dns_read_record(const uint8_t* message, size_t message_size,
                            size_t* offset,
                            struct dns_record_view* record) {
    if (offset == NULL || record == NULL || *offset >= message_size) {
        return false;
    }
    size_t cursor = *offset;
    record->owner_offset = cursor;
    if (!dns_skip_name(message, message_size, &cursor) ||
        cursor > message_size || message_size - cursor < 10u) {
        return false;
    }
    record->type = read_u16_be(message + cursor);
    record->record_class = read_u16_be(message + cursor + 2u);
    record->ttl_seconds = read_u32_be(message + cursor + 4u);
    record->data_size = read_u16_be(message + cursor + 8u);
    cursor += 10u;
    if ((size_t)record->data_size > message_size - cursor) return false;
    record->data_offset = cursor;
    record->next_offset = cursor + record->data_size;
    *offset = record->next_offset;
    return true;
}

static bool dns_validate_record_sections(
    const uint8_t* message, size_t message_size, size_t records_offset,
    uint16_t answer_count, uint16_t authority_count,
    uint16_t additional_count, size_t* authority_offset) {
    size_t offset = records_offset;
    struct dns_record_view record;
    for (uint16_t i = 0; i < answer_count; i++) {
        if (!dns_read_record(message, message_size, &offset, &record)) {
            return false;
        }
    }
    if (authority_offset != NULL) *authority_offset = offset;
    for (uint16_t i = 0; i < authority_count; i++) {
        if (!dns_read_record(message, message_size, &offset, &record)) {
            return false;
        }
    }
    for (uint16_t i = 0; i < additional_count; i++) {
        if (!dns_read_record(message, message_size, &offset, &record)) {
            return false;
        }
    }
    return offset == message_size;
}

static void dns_copy_text(char destination[NET_DNS_NAME_MAX + 1u],
                          const char* source) {
    size_t length = 0u;
    while (source[length] != '\0') length++;
    bytes_zero(destination, NET_DNS_NAME_MAX + 1u);
    bytes_copy(destination, source, length + 1u);
}

static enum dns_answer_search_result dns_find_address_answer(
    const uint8_t* message, size_t message_size, size_t answers_offset,
    uint16_t answer_count, const char* query_name, uint32_t* address,
    uint32_t* ttl_seconds,
    char terminal_name[NET_DNS_NAME_MAX + 1u]) {
    char current_name[NET_DNS_NAME_MAX + 1u];
    dns_copy_text(current_name, query_name);
    uint32_t effective_ttl = UINT32_MAX;
    *ttl_seconds = UINT32_MAX;

    for (unsigned int hop = 0; hop <= DNS_MAX_CNAME_HOPS; hop++) {
        bool found_address = false;
        bool found_cname = false;
        uint32_t selected_address = 0u;
        uint32_t address_ttl = 0u;
        uint32_t cname_ttl = 0u;
        char cname_target[NET_DNS_NAME_MAX + 1u];
        bytes_zero(cname_target, sizeof(cname_target));

        size_t offset = answers_offset;
        for (uint16_t index = 0; index < answer_count; index++) {
            struct dns_record_view record;
            char owner[NET_DNS_NAME_MAX + 1u];
            if (!dns_read_record(message, message_size, &offset, &record) ||
                !dns_expand_name(message, message_size, record.owner_offset,
                                 owner, NULL)) {
                return DNS_ANSWER_MALFORMED;
            }
            if (record.record_class != DNS_CLASS_IN ||
                !dns_names_equal(owner, current_name)) {
                continue;
            }

            if (record.type == DNS_TYPE_A) {
                if (record.data_size != 4u || found_cname) {
                    return DNS_ANSWER_MALFORMED;
                }
                uint32_t candidate =
                    read_u32_be(message + record.data_offset);
                if (!ipv4_is_usable_unicast(candidate)) continue;
                if (!found_address) {
                    found_address = true;
                    selected_address = candidate;
                    address_ttl = record.ttl_seconds;
                } else if (record.ttl_seconds < address_ttl) {
                    /*
                     * The cached address represents the whole usable RRset.
                     * It must not outlive a shorter-lived peer record.
                     */
                    address_ttl = record.ttl_seconds;
                }
            } else if (record.type == DNS_TYPE_CNAME) {
                if (found_address || found_cname) {
                    return DNS_ANSWER_MALFORMED;
                }
                size_t next = 0u;
                if (!dns_expand_name(message, message_size,
                                     record.data_offset, cname_target,
                                     &next) ||
                    cname_target[0] == '\0' ||
                    next != record.next_offset) {
                    return DNS_ANSWER_MALFORMED;
                }
                found_cname = true;
                cname_ttl = record.ttl_seconds;
            }
        }

        if (found_address) {
            if (effective_ttl == UINT32_MAX ||
                address_ttl < effective_ttl) {
                effective_ttl = address_ttl;
            }
            *address = selected_address;
            *ttl_seconds = effective_ttl;
            dns_copy_text(terminal_name, current_name);
            return DNS_ANSWER_ADDRESS;
        }
        if (!found_cname) {
            /*
             * A negative answer reached through one or more aliases must not
             * be cached beyond the shortest CNAME in that chain.
             */
            *ttl_seconds = effective_ttl;
            dns_copy_text(terminal_name, current_name);
            return DNS_ANSWER_NOT_FOUND;
        }
        if (hop == DNS_MAX_CNAME_HOPS ||
            dns_names_equal(cname_target, current_name)) {
            return DNS_ANSWER_MALFORMED;
        }
        if (effective_ttl == UINT32_MAX || cname_ttl < effective_ttl) {
            effective_ttl = cname_ttl;
        }
        dns_copy_text(current_name, cname_target);
    }
    return DNS_ANSWER_MALFORMED;
}

static bool dns_name_is_within_zone(const char* name, const char* zone) {
    size_t name_length = 0u;
    size_t zone_length = 0u;
    while (name[name_length] != '\0') name_length++;
    while (zone[zone_length] != '\0') zone_length++;
    if (zone_length == 0u) return true;
    if (name_length < zone_length) return false;
    const char* suffix = name + name_length - zone_length;
    if (!dns_names_equal(suffix, zone)) return false;
    return name_length == zone_length ||
           name[name_length - zone_length - 1u] == '.';
}

static uint32_t dns_find_negative_ttl(
    const uint8_t* message, size_t message_size, size_t authority_offset,
    uint16_t authority_count, const char* terminal_name) {
    uint32_t selected_ttl = 0u;
    size_t offset = authority_offset;

    for (uint16_t index = 0; index < authority_count; index++) {
        struct dns_record_view record;
        char owner[NET_DNS_NAME_MAX + 1u];
        if (!dns_read_record(message, message_size, &offset, &record) ||
            !dns_expand_name(message, message_size, record.owner_offset,
                             owner, NULL)) {
            return 0u;
        }
        if (record.type != DNS_TYPE_SOA ||
            record.record_class != DNS_CLASS_IN ||
            !dns_name_is_within_zone(terminal_name, owner)) {
            continue;
        }

        size_t data_end = record.next_offset;
        size_t cursor = record.data_offset;
        char mname[NET_DNS_NAME_MAX + 1u];
        char rname[NET_DNS_NAME_MAX + 1u];
        size_t next = 0u;
        if (!dns_expand_name(message, message_size, cursor, mname, &next) ||
            mname[0] == '\0' || next > data_end) {
            continue;
        }
        cursor = next;
        if (!dns_expand_name(message, message_size, cursor, rname, &next) ||
            rname[0] == '\0' || next > data_end ||
            data_end - next != 20u) {
            continue;
        }
        uint32_t minimum_ttl = read_u32_be(message + next + 16u);
        uint32_t candidate = record.ttl_seconds < minimum_ttl
                                 ? record.ttl_seconds
                                 : minimum_ttl;
        if (candidate > NET_DNS_NEGATIVE_TTL_MAX_SECONDS) {
            candidate = NET_DNS_NEGATIVE_TTL_MAX_SECONDS;
        }
        if (candidate != 0u &&
            (selected_ttl == 0u || candidate < selected_ttl)) {
            selected_ttl = candidate;
        }
    }
    return selected_ttl;
}

static bool dns_receive(struct net_stack* stack, uint32_t source_ip,
                        uint16_t source_port, uint16_t destination_port,
                        const uint8_t* message, size_t message_size) {
    if (stack->dns.state != NET_DNS_PENDING ||
        source_ip != stack->ipv4.dns || source_port != DNS_SERVER_PORT ||
        destination_port != stack->dns.source_port ||
        message_size < DNS_HEADER_SIZE ||
        read_u16_be(message) != stack->dns.transaction_id) {
        return false;
    }

    uint16_t flags = read_u16_be(message + 2u);
    if ((flags & DNS_FLAG_RESPONSE) == 0u ||
        (flags & DNS_FLAG_TRUNCATED) != 0u ||
        ((flags >> 11) & 0xFu) != 0u) {
        stack->dns.state = NET_DNS_MALFORMED_RESPONSE;
        return true;
    }
    stack->dns.response_code = (uint8_t)(flags & 0xFu);

    uint16_t question_count = read_u16_be(message + 4u);
    uint16_t answer_count = read_u16_be(message + 6u);
    uint16_t authority_count = read_u16_be(message + 8u);
    uint16_t additional_count = read_u16_be(message + 10u);
    uint32_t record_count = (uint32_t)answer_count + authority_count +
                            additional_count;
    if (question_count != 1u || record_count > 64u) {
        stack->dns.state = NET_DNS_MALFORMED_RESPONSE;
        return true;
    }

    size_t offset = DNS_HEADER_SIZE;
    char question_name[NET_DNS_NAME_MAX + 1u];
    size_t next = 0u;
    if (!dns_expand_name(message, message_size, offset, question_name,
                         &next) ||
        next > message_size || message_size - next < 4u ||
        !dns_names_equal(question_name, stack->dns.name) ||
        read_u16_be(message + next) != DNS_TYPE_A ||
        read_u16_be(message + next + 2u) != DNS_CLASS_IN) {
        stack->dns.state = NET_DNS_MALFORMED_RESPONSE;
        return true;
    }
    offset = next + 4u;

    size_t answers_offset = offset;
    size_t authority_offset = 0u;
    if (!dns_validate_record_sections(
            message, message_size, answers_offset, answer_count,
            authority_count, additional_count, &authority_offset)) {
        stack->dns.state = NET_DNS_MALFORMED_RESPONSE;
        return true;
    }

    /*
     * Only NOERROR and NXDOMAIN describe positive/negative lookup results.
     * SERVFAIL, REFUSED, and every other RCODE are server errors: retain the
     * code for the caller, but never turn them into durable negative answers.
     */
    if (stack->dns.response_code != 0u &&
        stack->dns.response_code != 3u) {
        stack->dns.state = NET_DNS_SERVER_ERROR;
        return true;
    }

    uint32_t address = 0u;
    uint32_t answer_ttl = UINT32_MAX;
    char terminal_name[NET_DNS_NAME_MAX + 1u];
    enum dns_answer_search_result answer = dns_find_address_answer(
        message, message_size, answers_offset, answer_count,
        stack->dns.name, &address, &answer_ttl, terminal_name);
    if (answer == DNS_ANSWER_MALFORMED ||
        (answer == DNS_ANSWER_ADDRESS &&
         stack->dns.response_code != 0u)) {
        stack->dns.state = NET_DNS_MALFORMED_RESPONSE;
        return true;
    }
    if (answer == DNS_ANSWER_ADDRESS) {
        stack->dns.result_address = address;
        stack->dns.ttl_seconds = answer_ttl;
        stack->dns.state = NET_DNS_RESOLVED;
        dns_cache_store(stack, stack->dns.name, false, address, 0u,
                        answer_ttl);
        return true;
    }

    if (stack->dns.response_code == 0u ||
        stack->dns.response_code == 3u) {
        uint32_t negative_ttl = dns_find_negative_ttl(
            message, message_size, authority_offset, authority_count,
            terminal_name);
        if (negative_ttl != 0u &&
            answer_ttl != UINT32_MAX &&
            answer_ttl < negative_ttl) {
            negative_ttl = answer_ttl;
        }
        stack->dns.ttl_seconds = negative_ttl;
        if (negative_ttl != 0u) {
            dns_cache_store(stack, stack->dns.name, true, 0u,
                            stack->dns.response_code, negative_ttl);
        }
    }
    stack->dns.state = NET_DNS_NOT_FOUND;
    return true;
}

bool net_dns_query(struct net_stack* stack, const char* name,
                   uint16_t transaction_id) {
    char validated_name[NET_DNS_NAME_MAX + 1u];
    bytes_zero(validated_name, sizeof(validated_name));
    if (stack == NULL ||
        !dns_validate_and_copy_name(validated_name, name)) {
        return false;
    }

    dns_cache_expire(stack);
    struct net_dns_cache_entry* cached =
        dns_cache_find(stack, validated_name);
    if (cached != NULL) {
        uint32_t remaining = dns_cache_remaining_seconds(
            cached->expires_ms, stack->current_time_ms);
        if (remaining != 0u) {
            stack->dns_cache_hits++;
            cached->last_used_ms = stack->current_time_ms;
            bytes_zero(&stack->dns, sizeof(stack->dns));
            bytes_copy(stack->dns.name, validated_name,
                       sizeof(validated_name));
            stack->dns.state = cached->negative
                                   ? NET_DNS_NOT_FOUND
                                   : NET_DNS_RESOLVED;
            stack->dns.result_address = cached->address;
            stack->dns.ttl_seconds = remaining;
            stack->dns.response_code = cached->response_code;
            return true;
        }
    }
    stack->dns_cache_misses++;
    if (stack->ipv4.address == 0u ||
        !ipv4_is_usable_unicast(stack->ipv4.dns)) {
        return false;
    }

    if (transaction_id == 0u) {
        transaction_id =
            (uint16_t)(stack->next_ipv4_id ^ (uint16_t)stack->current_time_ms);
        if (transaction_id == 0u) transaction_id = 1u;
    }

    bytes_copy(stack->dns.name, validated_name, sizeof(validated_name));
    stack->dns.transaction_id = transaction_id;
    stack->dns.source_port =
        (uint16_t)(49152u + (transaction_id & 0x3FFFu));
    stack->dns.result_address = 0u;
    stack->dns.ttl_seconds = 0u;
    stack->dns.response_code = 0u;
    stack->dns.attempts = 0u;
    stack->dns.state = NET_DNS_PENDING;
    (void)dns_send_query(stack);
    return true;
}

struct net_dns_result net_dns_get_result(const struct net_stack* stack) {
    struct net_dns_result result = {
        .state = NET_DNS_IDLE,
        .address = 0u,
        .ttl_seconds = 0u,
        .response_code = 0u,
    };
    if (stack == NULL) return result;
    result.state = stack->dns.state;
    result.address = stack->dns.result_address;
    result.ttl_seconds = stack->dns.ttl_seconds;
    result.response_code = stack->dns.response_code;
    return result;
}

bool net_dns_cancel(struct net_stack* stack) {
    if (stack == NULL || stack->dns.state != NET_DNS_PENDING) {
        return false;
    }
    struct net_pending_ipv4* pending = &stack->pending_ipv4;
    if (pending->valid &&
        pending->protocol == NET_IPV4_PROTOCOL_UDP &&
        pending->destination_ip == stack->ipv4.dns &&
        pending->payload_size >= 10u &&
        read_u16_be(pending->payload) == stack->dns.source_port &&
        read_u16_be(pending->payload + 2u) == DNS_SERVER_PORT &&
        read_u16_be(pending->payload + 8u) ==
            stack->dns.transaction_id) {
        /*
         * ARP may still be resolving for the first query. Remove only this
         * resolver-owned datagram so cancellation cannot transmit it later.
         */
        bytes_zero(pending, sizeof(*pending));
    }
    bytes_zero(&stack->dns, sizeof(stack->dns));
    return true;
}

void net_dns_clear_result(struct net_stack* stack) {
    if (stack == NULL) return;
    bytes_zero(&stack->dns, sizeof(stack->dns));
}

void net_dns_cache_clear(struct net_stack* stack) {
    if (stack == NULL) return;
    bytes_zero(stack->dns_cache, sizeof(stack->dns_cache));
}

struct net_dns_cache_status net_dns_cache_get_status(
    const struct net_stack* stack) {
    struct net_dns_cache_status status = {
        .capacity = NET_DNS_CACHE_SIZE,
        .entries = 0u,
        .positive_entries = 0u,
        .negative_entries = 0u,
        .hits = 0u,
        .misses = 0u,
        .evictions = 0u,
        .expirations = 0u,
    };
    if (stack == NULL) return status;

    status.hits = stack->dns_cache_hits;
    status.misses = stack->dns_cache_misses;
    status.evictions = stack->dns_cache_evictions;
    status.expirations = stack->dns_cache_expirations;
    for (size_t i = 0; i < NET_DNS_CACHE_SIZE; i++) {
        const struct net_dns_cache_entry* entry = &stack->dns_cache[i];
        if (!entry->valid ||
            entry->expires_ms <= stack->current_time_ms) {
            continue;
        }
        status.entries++;
        if (entry->negative) {
            status.negative_entries++;
        } else {
            status.positive_entries++;
        }
    }
    return status;
}

size_t net_dns_cache_snapshot(
    const struct net_stack* stack, struct net_dns_cache_entry_info* entries,
    size_t entry_capacity) {
    if (stack == NULL || (entry_capacity != 0u && entries == NULL)) return 0u;
    size_t copied = 0u;
    for (size_t i = 0; i < NET_DNS_CACHE_SIZE &&
                       copied < entry_capacity;
         i++) {
        const struct net_dns_cache_entry* cached = &stack->dns_cache[i];
        if (!cached->valid ||
            cached->expires_ms <= stack->current_time_ms) {
            continue;
        }
        struct net_dns_cache_entry_info* output = &entries[copied++];
        bytes_zero(output, sizeof(*output));
        size_t name_length = 0u;
        while (cached->name[name_length] != '\0') name_length++;
        bytes_copy(output->name, cached->name, name_length + 1u);
        output->kind = cached->negative ? NET_DNS_CACHE_NEGATIVE
                                        : NET_DNS_CACHE_POSITIVE;
        output->address = cached->address;
        output->remaining_ttl_seconds = dns_cache_remaining_seconds(
            cached->expires_ms, stack->current_time_ms);
        output->response_code = cached->response_code;
    }
    return copied;
}

enum net_send_result net_icmp_echo_request(
    struct net_stack* stack, uint32_t destination_ip, uint16_t identifier,
    uint16_t sequence, const void* payload, size_t payload_size) {
    if (stack == NULL || payload_size > NET_IPV4_PAYLOAD_MAX - 8u ||
        (payload_size != 0u && payload == NULL)) {
        return NET_SEND_FAILED;
    }
    uint8_t* icmp = stack->transmit_frame + NET_ETHERNET_HEADER_SIZE +
                    NET_IPV4_HEADER_MIN_SIZE;
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[1] = 0u;
    write_u16_be(icmp + 2u, 0u);
    write_u16_be(icmp + 4u, identifier);
    write_u16_be(icmp + 6u, sequence);
    bytes_copy(icmp + 8u, payload, payload_size);
    write_u16_be(icmp + 2u, internet_checksum(icmp, 8u + payload_size));

    enum net_send_result result =
        net_ipv4_send(stack, destination_ip, NET_IPV4_PROTOCOL_ICMP, icmp,
                      8u + payload_size);
    if (result != NET_SEND_FAILED) {
        stack->echo_request.pending = true;
        stack->echo_request.destination_ip = destination_ip;
        stack->echo_request.identifier = identifier;
        stack->echo_request.sequence = sequence;
        stack->echo_request.sent_ms = stack->current_time_ms;
        stack->echo_reply.valid = false;
    }
    return result;
}

struct net_icmp_echo_reply net_icmp_last_echo_reply(
    const struct net_stack* stack) {
    struct net_icmp_echo_reply reply = {0};
    return stack == NULL ? reply : stack->echo_reply;
}

void net_icmp_clear_echo_reply(struct net_stack* stack) {
    if (stack == NULL) return;
    bytes_zero(&stack->echo_reply, sizeof(stack->echo_reply));
}

static bool receive_icmp(struct net_stack* stack, uint32_t source_ip,
                         uint32_t destination_ip, const uint8_t* packet,
                         size_t packet_size) {
    if (packet_size < 8u || internet_checksum(packet, packet_size) != 0u) {
        return false;
    }
    uint8_t type = packet[0];
    uint8_t code = packet[1];
    if (type == ICMP_ECHO_REQUEST && code == 0u &&
        stack->ipv4.address != 0u &&
        destination_ip == stack->ipv4.address) {
        uint8_t* reply = stack->transmit_frame + NET_ETHERNET_HEADER_SIZE +
                         NET_IPV4_HEADER_MIN_SIZE;
        bytes_copy(reply, packet, packet_size);
        reply[0] = ICMP_ECHO_REPLY;
        write_u16_be(reply + 2u, 0u);
        write_u16_be(reply + 2u, internet_checksum(reply, packet_size));
        stack->stats.icmp_echo_requests_received++;
        return net_ipv4_send(stack, source_ip, NET_IPV4_PROTOCOL_ICMP, reply,
                             packet_size) != NET_SEND_FAILED;
    }
    if (type == ICMP_ECHO_REPLY && code == 0u &&
        stack->echo_request.pending &&
        source_ip == stack->echo_request.destination_ip &&
        read_u16_be(packet + 4u) == stack->echo_request.identifier &&
        read_u16_be(packet + 6u) == stack->echo_request.sequence) {
        stack->echo_reply.valid = true;
        stack->echo_reply.source_ip = source_ip;
        stack->echo_reply.identifier = read_u16_be(packet + 4u);
        stack->echo_reply.sequence = read_u16_be(packet + 6u);
        stack->echo_reply.round_trip_ms =
            stack->current_time_ms >= stack->echo_request.sent_ms
                ? stack->current_time_ms - stack->echo_request.sent_ms
                : 0u;
        stack->echo_reply.payload_size = packet_size - 8u;
        stack->echo_request.pending = false;
        stack->stats.icmp_echo_replies_received++;
        return true;
    }
    return false;
}

static bool receive_udp(struct net_stack* stack, uint32_t source_ip,
                        uint32_t destination_ip, const uint8_t* packet,
                        size_t packet_size) {
    if (packet_size < 8u) return false;
    uint16_t source_port = read_u16_be(packet);
    uint16_t destination_port = read_u16_be(packet + 2u);
    uint16_t udp_size = read_u16_be(packet + 4u);
    if (source_port == 0u || destination_port == 0u || udp_size < 8u ||
        udp_size != packet_size ||
        !udp_checksum_valid(source_ip, destination_ip, packet, udp_size)) {
        return false;
    }

    const uint8_t* data = packet + 8u;
    size_t data_size = udp_size - 8u;
    stack->stats.udp_datagrams_received++;

    if (destination_port == DHCP_CLIENT_PORT &&
        source_port == DHCP_SERVER_PORT) {
        return dhcp_receive(stack, source_ip, data, data_size);
    }
    if (destination_port == stack->dns.source_port &&
        source_port == DNS_SERVER_PORT) {
        return dns_receive(stack, source_ip, source_port, destination_port,
                           data, data_size);
    }

    for (size_t i = 0; i < NET_UDP_BINDING_COUNT; i++) {
        struct net_udp_binding* binding = &stack->udp_bindings[i];
        if (binding->handler != NULL &&
            binding->port == destination_port) {
            struct net_udp_datagram datagram = {
                .source_ip = source_ip,
                .destination_ip = destination_ip,
                .source_port = source_port,
                .destination_port = destination_port,
                .data = data,
                .data_size = data_size,
            };
            binding->handler(binding->context, &datagram);
            return true;
        }
    }
    return false;
}

static bool receive_ipv4(
    struct net_stack* stack,
    const uint8_t source_mac[NET_ETHERNET_ADDRESS_SIZE], const uint8_t* packet,
    size_t packet_size) {
    if (packet_size < NET_IPV4_HEADER_MIN_SIZE ||
        (packet[0] >> 4) != 4u) {
        return false;
    }
    size_t header_size = (size_t)(packet[0] & 0x0Fu) * 4u;
    if (header_size < NET_IPV4_HEADER_MIN_SIZE ||
        header_size > packet_size) {
        return false;
    }
    uint16_t total_size = read_u16_be(packet + 2u);
    uint16_t fragment = read_u16_be(packet + 6u);
    if (total_size < header_size || total_size > packet_size ||
        packet[8] == 0u || internet_checksum(packet, header_size) != 0u ||
        (fragment & 0x8000u) != 0u || (fragment & 0x3FFFu) != 0u) {
        return false;
    }

    uint32_t source_ip = read_u32_be(packet + 12u);
    uint32_t destination_ip = read_u32_be(packet + 16u);
    bool local_destination =
        ipv4_destination_is_local(stack, destination_ip);
    if (!local_destination && stack->ipv4.address == 0u &&
        stack->dhcp.state != NET_DHCP_OFF &&
        packet[9] == NET_IPV4_PROTOCOL_UDP &&
        total_size >= header_size + 8u) {
        local_destination =
            read_u16_be(packet + header_size + 2u) == DHCP_CLIENT_PORT;
    }
    if (!local_destination) return false;
    if (ipv4_is_usable_unicast(source_ip) &&
        ipv4_is_on_link(stack, source_ip)) {
        arp_learn(stack, source_ip, source_mac);
        (void)flush_pending_ipv4(stack);
    }

    const uint8_t* payload = packet + header_size;
    size_t payload_size = total_size - header_size;
    stack->stats.ipv4_packets_received++;
    switch (packet[9]) {
        case NET_IPV4_PROTOCOL_ICMP:
            return receive_icmp(stack, source_ip, destination_ip, payload,
                                payload_size);
        case NET_IPV4_PROTOCOL_UDP:
            return receive_udp(stack, source_ip, destination_ip, payload,
                               payload_size);
        case NET_IPV4_PROTOCOL_TCP:
            if (stack->tcp_handler == NULL) return false;
            stack->tcp_handler(stack->tcp_handler_context, source_ip,
                               destination_ip, payload, payload_size);
            return true;
        default:
            return false;
    }
}

static bool receive_arp(
    struct net_stack* stack,
    const uint8_t ethernet_source[NET_ETHERNET_ADDRESS_SIZE],
    const uint8_t* packet, size_t packet_size) {
    if (packet_size < ARP_PACKET_SIZE ||
        read_u16_be(packet) != ARP_HARDWARE_ETHERNET ||
        read_u16_be(packet + 2u) != ETHERTYPE_IPV4 ||
        packet[4] != NET_ETHERNET_ADDRESS_SIZE || packet[5] != 4u) {
        return false;
    }
    uint16_t operation = read_u16_be(packet + 6u);
    const uint8_t* sender_mac = packet + 8u;
    uint32_t sender_ip = read_u32_be(packet + 14u);
    const uint8_t* target_mac = packet + 18u;
    uint32_t target_ip = read_u32_be(packet + 24u);
    if (!bytes_equal(sender_mac, ethernet_source,
                     NET_ETHERNET_ADDRESS_SIZE) ||
        !mac_is_valid_source(sender_mac) ||
        !ipv4_is_usable_unicast(sender_ip)) {
        return false;
    }

    if (operation == ARP_OPERATION_REQUEST) {
        if (stack->ipv4.address == 0u || target_ip != stack->ipv4.address) {
            return false;
        }
        arp_learn(stack, sender_ip, sender_mac);
        (void)flush_pending_ipv4(stack);
        return send_arp_reply(stack, sender_mac, sender_ip);
    }
    if (operation == ARP_OPERATION_REPLY) {
        if (target_ip != stack->ipv4.address ||
            !bytes_equal(target_mac, stack->mac,
                         NET_ETHERNET_ADDRESS_SIZE)) {
            return false;
        }
        arp_learn(stack, sender_ip, sender_mac);
        stack->stats.arp_replies_received++;
        (void)flush_pending_ipv4(stack);
        return true;
    }
    return false;
}

bool net_input(struct net_stack* stack, const uint8_t* frame,
               size_t frame_size) {
    if (stack == NULL || frame == NULL) return false;
    stack->stats.frames_received++;
    if (frame_size < NET_ETHERNET_HEADER_SIZE ||
        frame_size > NET_ETHERNET_FRAME_MAX) {
        stack->stats.frames_dropped++;
        return false;
    }

    const uint8_t* destination = frame;
    const uint8_t* source = frame + NET_ETHERNET_ADDRESS_SIZE;
    if ((!bytes_equal(destination, stack->mac, NET_ETHERNET_ADDRESS_SIZE) &&
         !mac_is_broadcast(destination)) ||
        !mac_is_valid_source(source)) {
        stack->stats.frames_dropped++;
        return false;
    }

    uint16_t ethertype = read_u16_be(frame + 12u);
    bool handled = false;
    if (ethertype == ETHERTYPE_ARP) {
        handled = receive_arp(stack, source,
                              frame + NET_ETHERNET_HEADER_SIZE,
                              frame_size - NET_ETHERNET_HEADER_SIZE);
    } else if (ethertype == ETHERTYPE_IPV4) {
        handled = receive_ipv4(stack, source,
                               frame + NET_ETHERNET_HEADER_SIZE,
                               frame_size - NET_ETHERNET_HEADER_SIZE);
    }
    if (!handled) stack->stats.frames_dropped++;
    return handled;
}

static void tick_pending_arp(struct net_stack* stack) {
    struct net_pending_ipv4* pending = &stack->pending_ipv4;
    if (!pending->valid) return;
    uint8_t resolved_mac[NET_ETHERNET_ADDRESS_SIZE];
    bool resolved =
        net_arp_lookup(stack, pending->next_hop_ip, resolved_mac);
    if (stack->current_time_ms < pending->arp_requested_ms ||
        stack->current_time_ms - pending->arp_requested_ms <
            ARP_RETRY_INTERVAL_MS) {
        return;
    }
    if (resolved) {
        if (pending->transmit_attempts >= ARP_MAX_ATTEMPTS) {
            pending->valid = false;
            return;
        }
        (void)flush_pending_ipv4(stack);
        return;
    }
    if (pending->arp_attempts >= ARP_MAX_ATTEMPTS) {
        pending->valid = false;
        return;
    }
    pending->arp_requested_ms = stack->current_time_ms;
    pending->arp_attempts++;
    if (!resolved) (void)net_arp_request(stack, pending->next_hop_ip);
}

static void tick_dhcp(struct net_stack* stack) {
    struct net_dhcp_client* dhcp = &stack->dhcp;
    if (dhcp->state == NET_DHCP_OFF) return;

    if (dhcp->state == NET_DHCP_BOUND) {
        uint64_t lease_elapsed =
            stack->current_time_ms >= dhcp->lease_started_ms
                ? stack->current_time_ms - dhcp->lease_started_ms
                : 0u;
        if (lease_elapsed >= seconds_to_ms(dhcp->lease_seconds)) {
            dhcp_restart(stack);
        } else if (lease_elapsed >= seconds_to_ms(dhcp->renewal_seconds)) {
            dhcp->state = NET_DHCP_RENEWING;
            dhcp->attempts = 0u;
            (void)dhcp_send_message(stack, DHCP_MESSAGE_REQUEST, true);
        }
        return;
    }

    if (dhcp->state == NET_DHCP_RENEWING ||
        dhcp->state == NET_DHCP_REBINDING) {
        uint64_t lease_elapsed =
            stack->current_time_ms >= dhcp->lease_started_ms
                ? stack->current_time_ms - dhcp->lease_started_ms
                : 0u;
        if (lease_elapsed >= seconds_to_ms(dhcp->lease_seconds)) {
            dhcp_restart(stack);
            return;
        }
        if (dhcp->state == NET_DHCP_RENEWING &&
            lease_elapsed >= seconds_to_ms(dhcp->rebind_seconds)) {
            dhcp->state = NET_DHCP_REBINDING;
            dhcp->attempts = 0u;
            (void)dhcp_send_message(stack, DHCP_MESSAGE_REQUEST, true);
            return;
        }
    }

    if (!deadline_reached(stack->current_time_ms, dhcp->next_action_ms)) {
        return;
    }
    switch (dhcp->state) {
        case NET_DHCP_DISCOVERING:
            (void)dhcp_send_message(stack, DHCP_MESSAGE_DISCOVER, false);
            break;
        case NET_DHCP_REQUESTING:
            (void)dhcp_send_message(stack, DHCP_MESSAGE_REQUEST, false);
            break;
        case NET_DHCP_RENEWING:
            (void)dhcp_send_message(stack, DHCP_MESSAGE_REQUEST, true);
            break;
        case NET_DHCP_REBINDING:
            (void)dhcp_send_message(stack, DHCP_MESSAGE_REQUEST, true);
            break;
        default:
            break;
    }
}

static void tick_dns(struct net_stack* stack) {
    if (stack->dns.state != NET_DNS_PENDING ||
        !deadline_reached(stack->current_time_ms,
                          stack->dns.next_action_ms)) {
        return;
    }
    if (stack->dns.attempts >= DNS_MAX_ATTEMPTS) {
        stack->dns.state = NET_DNS_TIMED_OUT;
        return;
    }
    (void)dns_send_query(stack);
}

void net_tick(struct net_stack* stack, uint64_t now_ms) {
    if (stack == NULL) return;
    stack->current_time_ms = now_ms;
    dns_cache_expire(stack);

    for (size_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        struct net_arp_entry* entry = &stack->arp[i];
        if (entry->valid &&
            (now_ms < entry->refreshed_ms ||
             now_ms - entry->refreshed_ms >= ARP_ENTRY_LIFETIME_MS)) {
            entry->valid = false;
        }
    }
    tick_pending_arp(stack);
    tick_dhcp(stack);
    tick_dns(stack);
    if (stack->echo_request.pending &&
        (now_ms < stack->echo_request.sent_ms ||
         now_ms - stack->echo_request.sent_ms >= ICMP_ECHO_TIMEOUT_MS)) {
        stack->echo_request.pending = false;
    }
}

size_t net_poll(struct net_stack* stack) {
    if (stack == NULL) return 0u;
    if (stack->link.now_ms != NULL) {
        net_tick(stack, stack->link.now_ms(stack->link.context));
    }
    if (stack->link.poll == NULL) return 0u;

    size_t processed = 0u;
    for (size_t i = 0; i < NET_POLL_BUDGET; i++) {
        size_t frame_size =
            stack->link.poll(stack->link.context, stack->receive_frame,
                             sizeof(stack->receive_frame));
        if (frame_size == 0u) break;
        if (frame_size > sizeof(stack->receive_frame)) {
            stack->stats.frames_received++;
            stack->stats.frames_dropped++;
            break;
        }
        if (net_input(stack, stack->receive_frame, frame_size)) processed++;
    }
    if (stack->link.now_ms != NULL) {
        net_tick(stack, stack->link.now_ms(stack->link.context));
    }
    return processed;
}

bool net_init(struct net_stack* stack, const struct net_link_ops* link,
              const uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]) {
    if (stack == NULL || link == NULL || link->send == NULL || mac == NULL ||
        !mac_is_valid_source(mac)) {
        return false;
    }
    bytes_zero(stack, sizeof(*stack));
    stack->link = *link;
    bytes_copy(stack->mac, mac, NET_ETHERNET_ADDRESS_SIZE);
    stack->next_ipv4_id = 1u;
    if (stack->link.now_ms != NULL) {
        stack->current_time_ms =
            stack->link.now_ms(stack->link.context);
    }
    return true;
}

void net_configure_ipv4(struct net_stack* stack,
                        const struct net_ipv4_config* config) {
    if (stack == NULL || config == NULL) return;
    bool network_changed =
        stack->ipv4.address != config->address ||
        stack->ipv4.netmask != config->netmask ||
        stack->ipv4.gateway != config->gateway;
    bool resolver_changed = stack->ipv4.dns != config->dns;
    stack->ipv4 = *config;
    if (network_changed) {
        bytes_zero(stack->arp, sizeof(stack->arp));
        stack->pending_ipv4.valid = false;
    }
    if (network_changed || resolver_changed) net_dns_cache_clear(stack);
}

void net_clear_ipv4(struct net_stack* stack) {
    if (stack == NULL) return;
    bytes_zero(&stack->ipv4, sizeof(stack->ipv4));
    bytes_zero(stack->arp, sizeof(stack->arp));
    stack->pending_ipv4.valid = false;
    net_dns_cache_clear(stack);
    if (stack->dns.state == NET_DNS_PENDING) {
        stack->dns.state = NET_DNS_TIMED_OUT;
    }
}

bool net_is_configured(const struct net_stack* stack) {
    return stack != NULL && ipv4_is_usable_unicast(stack->ipv4.address);
}

struct net_ipv4_config net_get_ipv4_config(const struct net_stack* stack) {
    struct net_ipv4_config config = {0};
    return stack == NULL ? config : stack->ipv4;
}

uint32_t net_local_ip(const struct net_stack* stack) {
    return stack == NULL ? 0u : stack->ipv4.address;
}

uint32_t net_gateway_ip(const struct net_stack* stack) {
    return stack == NULL ? 0u : stack->ipv4.gateway;
}

uint32_t net_dns_ip(const struct net_stack* stack) {
    return stack == NULL ? 0u : stack->ipv4.dns;
}

uint64_t net_now_ms(const struct net_stack* stack) {
    return stack == NULL ? 0u : stack->current_time_ms;
}

const struct net_stats* net_get_stats(const struct net_stack* stack) {
    return stack == NULL ? NULL : &stack->stats;
}
