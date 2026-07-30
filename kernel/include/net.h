#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NostaluxOS's small, allocation-free IPv4 stack.
 *
 * IPv4 addresses are always represented as canonical big-endian numeric
 * values, independent of the CPU's byte order.  For example, 10.0.2.15 is
 * 0x0A00020F.  NET_IPV4() is the preferred way to construct an address.
 */
#define NET_IPV4(a, b, c, d)                                              \
    ((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(b) & 0xFFu) << 16) |  \
     (((uint32_t)(c) & 0xFFu) << 8) | ((uint32_t)(d) & 0xFFu))

#define NET_ETHERNET_ADDRESS_SIZE 6u
#define NET_ETHERNET_HEADER_SIZE 14u
#define NET_ETHERNET_MTU 1500u
#define NET_ETHERNET_FRAME_MAX                                            \
    (NET_ETHERNET_HEADER_SIZE + NET_ETHERNET_MTU)
#define NET_IPV4_HEADER_MIN_SIZE 20u
#define NET_IPV4_PAYLOAD_MAX (NET_ETHERNET_MTU - NET_IPV4_HEADER_MIN_SIZE)
#define NET_UDP_PAYLOAD_MAX (NET_IPV4_PAYLOAD_MAX - 8u)
#define NET_ARP_CACHE_SIZE 8u
#define NET_UDP_BINDING_COUNT 8u
#define NET_DNS_NAME_MAX 253u
#define NET_DNS_CACHE_SIZE 8u
#define NET_DNS_NEGATIVE_TTL_MAX_SECONDS 60u

#define NET_IPV4_PROTOCOL_ICMP 1u
#define NET_IPV4_PROTOCOL_TCP 6u
#define NET_IPV4_PROTOCOL_UDP 17u

typedef bool (*net_link_send_fn)(void* context, const uint8_t* frame,
                                 size_t frame_size);
typedef size_t (*net_link_poll_fn)(void* context, uint8_t* frame,
                                   size_t frame_capacity);
typedef uint64_t (*net_link_now_ms_fn)(void* context);

/*
 * send() must consume or copy the frame before returning. poll() returns zero
 * when no frame is ready and otherwise returns the copied frame length.
 * now_ms() is optional when the caller drives net_tick() directly.
 */
struct net_link_ops {
    net_link_send_fn send;
    net_link_poll_fn poll;
    net_link_now_ms_fn now_ms;
    void* context;
};

struct net_ipv4_config {
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
};

enum net_send_result {
    NET_SEND_FAILED = 0,
    NET_SEND_SENT,
    NET_SEND_QUEUED,
};

struct net_udp_datagram {
    uint32_t source_ip;
    uint32_t destination_ip;
    uint16_t source_port;
    uint16_t destination_port;
    const uint8_t* data;
    size_t data_size;
};

typedef void (*net_udp_handler_fn)(void* context,
                                   const struct net_udp_datagram* datagram);
typedef void (*net_ipv4_handler_fn)(void* context, uint32_t source_ip,
                                    uint32_t destination_ip,
                                    const uint8_t* payload,
                                    size_t payload_size);

enum net_dhcp_state {
    NET_DHCP_OFF = 0,
    NET_DHCP_DISCOVERING,
    NET_DHCP_REQUESTING,
    NET_DHCP_BOUND,
    NET_DHCP_RENEWING,
    NET_DHCP_REBINDING,
};

enum net_dns_state {
    NET_DNS_IDLE = 0,
    NET_DNS_PENDING,
    NET_DNS_RESOLVED,
    NET_DNS_NOT_FOUND,
    NET_DNS_TIMED_OUT,
    NET_DNS_MALFORMED_RESPONSE,
    NET_DNS_SERVER_ERROR,
};

struct net_dns_result {
    enum net_dns_state state;
    uint32_t address;
    uint32_t ttl_seconds;
    uint8_t response_code;
};

enum net_dns_cache_entry_kind {
    NET_DNS_CACHE_POSITIVE = 0,
    NET_DNS_CACHE_NEGATIVE,
};

struct net_dns_cache_entry_info {
    char name[NET_DNS_NAME_MAX + 1u];
    enum net_dns_cache_entry_kind kind;
    uint32_t address;
    uint32_t remaining_ttl_seconds;
    uint8_t response_code;
};

struct net_dns_cache_status {
    size_t capacity;
    size_t entries;
    size_t positive_entries;
    size_t negative_entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t expirations;
};

struct net_icmp_echo_reply {
    bool valid;
    uint32_t source_ip;
    uint16_t identifier;
    uint16_t sequence;
    uint64_t round_trip_ms;
    size_t payload_size;
};

struct net_stats {
    uint64_t frames_received;
    uint64_t frames_transmitted;
    uint64_t frames_dropped;
    uint64_t arp_requests_sent;
    uint64_t arp_replies_received;
    uint64_t ipv4_packets_received;
    uint64_t ipv4_packets_transmitted;
    uint64_t icmp_echo_requests_received;
    uint64_t icmp_echo_replies_received;
    uint64_t udp_datagrams_received;
    uint64_t udp_datagrams_transmitted;
};

/*
 * The fields below are public solely so a freestanding kernel can allocate a
 * net_stack statically.  Callers should treat every field as read-only and use
 * the API functions to change state.
 */
struct net_arp_entry {
    bool valid;
    uint32_t ip;
    uint8_t mac[NET_ETHERNET_ADDRESS_SIZE];
    uint64_t refreshed_ms;
};

struct net_udp_binding {
    uint16_t port;
    net_udp_handler_fn handler;
    void* context;
};

struct net_pending_ipv4 {
    bool valid;
    uint32_t destination_ip;
    uint32_t next_hop_ip;
    uint8_t protocol;
    size_t payload_size;
    uint8_t payload[NET_IPV4_PAYLOAD_MAX];
    uint64_t arp_requested_ms;
    unsigned int arp_attempts;
    unsigned int transmit_attempts;
};

struct net_dhcp_client {
    enum net_dhcp_state state;
    uint32_t transaction_id;
    uint32_t offered_ip;
    uint32_t server_ip;
    uint32_t offered_netmask;
    uint32_t offered_gateway;
    uint32_t offered_dns;
    uint32_t lease_seconds;
    uint32_t renewal_seconds;
    uint32_t rebind_seconds;
    uint64_t lease_started_ms;
    uint64_t next_action_ms;
    unsigned int attempts;
};

struct net_dns_client {
    enum net_dns_state state;
    char name[NET_DNS_NAME_MAX + 1u];
    uint16_t transaction_id;
    uint16_t source_port;
    uint32_t result_address;
    uint32_t ttl_seconds;
    uint8_t response_code;
    uint64_t next_action_ms;
    unsigned int attempts;
};

struct net_dns_cache_entry {
    bool valid;
    bool negative;
    char name[NET_DNS_NAME_MAX + 1u];
    uint32_t address;
    uint8_t response_code;
    uint64_t expires_ms;
    uint64_t last_used_ms;
};

struct net_echo_request {
    bool pending;
    uint32_t destination_ip;
    uint16_t identifier;
    uint16_t sequence;
    uint64_t sent_ms;
};

struct net_stack {
    struct net_link_ops link;
    uint8_t mac[NET_ETHERNET_ADDRESS_SIZE];
    struct net_ipv4_config ipv4;
    uint16_t next_ipv4_id;
    uint64_t current_time_ms;
    struct net_arp_entry arp[NET_ARP_CACHE_SIZE];
    struct net_udp_binding udp_bindings[NET_UDP_BINDING_COUNT];
    struct net_pending_ipv4 pending_ipv4;
    struct net_dhcp_client dhcp;
    struct net_dns_client dns;
    struct net_dns_cache_entry dns_cache[NET_DNS_CACHE_SIZE];
    uint64_t dns_cache_hits;
    uint64_t dns_cache_misses;
    uint64_t dns_cache_evictions;
    uint64_t dns_cache_expirations;
    net_ipv4_handler_fn tcp_handler;
    void* tcp_handler_context;
    struct net_echo_request echo_request;
    struct net_icmp_echo_reply echo_reply;
    struct net_stats stats;
    uint8_t receive_frame[NET_ETHERNET_FRAME_MAX];
    uint8_t transmit_frame[NET_ETHERNET_FRAME_MAX];
};

bool net_init(struct net_stack* stack, const struct net_link_ops* link,
              const uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]);
void net_configure_ipv4(struct net_stack* stack,
                        const struct net_ipv4_config* config);
void net_clear_ipv4(struct net_stack* stack);
bool net_is_configured(const struct net_stack* stack);
struct net_ipv4_config net_get_ipv4_config(const struct net_stack* stack);
uint32_t net_local_ip(const struct net_stack* stack);
uint32_t net_gateway_ip(const struct net_stack* stack);
uint32_t net_dns_ip(const struct net_stack* stack);
uint64_t net_now_ms(const struct net_stack* stack);

/*
 * Feed one complete Ethernet frame, or poll a bounded number of frames from
 * the link driver. net_input() returns true only for a well-formed frame that
 * was addressed to this host and handled by the stack.
 */
bool net_input(struct net_stack* stack, const uint8_t* frame,
               size_t frame_size);
size_t net_poll(struct net_stack* stack);
void net_tick(struct net_stack* stack, uint64_t now_ms);

/*
 * SENT means the driver accepted the frame. QUEUED means the stack copied one
 * packet while ARP resolves its next hop; a second unresolved send fails until
 * that packet is transmitted or its ARP retries expire.
 */
enum net_send_result net_ipv4_send(struct net_stack* stack,
                                   uint32_t destination_ip, uint8_t protocol,
                                   const void* payload, size_t payload_size);
void net_set_tcp_handler(struct net_stack* stack,
                         net_ipv4_handler_fn handler, void* context);

bool net_udp_bind(struct net_stack* stack, uint16_t port,
                  net_udp_handler_fn handler, void* context);
void net_udp_unbind(struct net_stack* stack, uint16_t port);
enum net_send_result net_udp_send(struct net_stack* stack,
                                  uint32_t destination_ip,
                                  uint16_t destination_port,
                                  uint16_t source_port, const void* data,
                                  size_t data_size);

bool net_arp_lookup(const struct net_stack* stack, uint32_t ip,
                    uint8_t mac[NET_ETHERNET_ADDRESS_SIZE]);
bool net_arp_request(struct net_stack* stack, uint32_t ip);

enum net_send_result net_icmp_echo_request(
    struct net_stack* stack, uint32_t destination_ip, uint16_t identifier,
    uint16_t sequence, const void* payload, size_t payload_size);
struct net_icmp_echo_reply net_icmp_last_echo_reply(
    const struct net_stack* stack);
void net_icmp_clear_echo_reply(struct net_stack* stack);

bool net_dhcp_start(struct net_stack* stack, uint32_t transaction_id);
void net_dhcp_stop(struct net_stack* stack);
enum net_dhcp_state net_dhcp_get_state(const struct net_stack* stack);

bool net_dns_query(struct net_stack* stack, const char* name,
                   uint16_t transaction_id);
struct net_dns_result net_dns_get_result(const struct net_stack* stack);
/*
 * Cancel only an active lookup. A successful cancellation returns the
 * resolver to IDLE and prevents retries or a late reply from completing it.
 */
bool net_dns_cancel(struct net_stack* stack);
void net_dns_clear_result(struct net_stack* stack);
/*
 * Cache entries are keyed case-insensitively. clear() preserves lifetime hit,
 * miss, eviction, and expiration counters. snapshot() omits expired entries.
 */
void net_dns_cache_clear(struct net_stack* stack);
struct net_dns_cache_status net_dns_cache_get_status(
    const struct net_stack* stack);
size_t net_dns_cache_snapshot(
    const struct net_stack* stack, struct net_dns_cache_entry_info* entries,
    size_t entry_capacity);

const struct net_stats* net_get_stats(const struct net_stack* stack);

#endif /* NET_H */
