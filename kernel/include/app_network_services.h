#ifndef APP_NETWORK_SERVICES_H
#define APP_NETWORK_SERVICES_H

#include <stdint.h>

struct paging_address_space;

/*
 * Process-owned asynchronous network request services. User pointers are
 * copied through the supplied address space and never retained. The URL
 * snapshot remains in kernel-owned storage until the backend request closes.
 * Handles are scoped to process_id and every operation rechecks the network
 * capability. Process release revokes ownership immediately; failed backend
 * cleanup is retained privately and retried before the service is reused.
 */
uint64_t app_network_services_dispatch(
    uint64_t syscall_id,
    struct paging_address_space* address_space,
    uint64_t process_id,
    uint64_t granted_capabilities,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3,
    uint64_t argument4,
    uint64_t argument5);

void app_network_services_release_process(uint64_t process_id);

#endif /* APP_NETWORK_SERVICES_H */
