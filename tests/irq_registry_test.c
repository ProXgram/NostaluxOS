#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "irq_registry.h"

struct callback_state {
    struct irq_registry* registry;
    uint8_t irq;
    unsigned int calls;
    bool claims;
    bool unregister_self;
};

static void require(bool condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static bool callback(uint8_t irq, void* opaque) {
    struct callback_state* state = (struct callback_state*)opaque;
    require(irq == state->irq, "dispatcher should preserve the IRQ number");
    state->calls++;
    if (state->unregister_self) {
        require(irq_registry_unregister(
                    state->registry, irq, callback, state),
                "handler should be able to remove its own stable slot");
    }
    return state->claims;
}

int main(void) {
    struct irq_registry registry;
    irq_registry_init(&registry);
    require(irq_registry_count(&registry, 11) == 0,
            "new registry should be empty");

    struct callback_state first = {
        .registry = &registry,
        .irq = 11,
        .claims = false,
    };
    struct callback_state second = {
        .registry = &registry,
        .irq = 11,
        .claims = true,
    };
    require(irq_registry_register(&registry, 11, callback, &first),
            "first shared handler should register");
    require(irq_registry_register(&registry, 11, callback, &second),
            "second shared handler should register");
    require(irq_registry_register(&registry, 11, callback, &second),
            "identical registration should be idempotent");
    require(irq_registry_count(&registry, 11) == 2,
            "idempotence should not duplicate a handler");
    require(irq_registry_dispatch(&registry, 11),
            "shared line should be claimed when any handler claims it");
    require(first.calls == 1 && second.calls == 1,
            "every shared handler should run even after one claims the line");

    first.unregister_self = true;
    require(irq_registry_dispatch(&registry, 11),
            "remaining handler should still claim during self-removal");
    require(first.calls == 2 && second.calls == 2,
            "self-removal must not skip later slots");
    require(irq_registry_count(&registry, 11) == 1,
            "self-removing handler should leave one peer");
    require(irq_registry_dispatch(&registry, 11),
            "remaining handler should continue receiving interrupts");
    require(first.calls == 2 && second.calls == 3,
            "removed handler must not be called again");

    struct callback_state extras[4];
    for (size_t index = 0; index < 4; index++) {
        extras[index] = (struct callback_state){
            .registry = &registry,
            .irq = 11,
            .claims = false,
        };
    }
    require(irq_registry_register(&registry, 11, callback, &extras[0]) &&
                irq_registry_register(&registry, 11, callback, &extras[1]) &&
                irq_registry_register(&registry, 11, callback, &extras[2]),
            "remaining shared slots should accept handlers");
    require(!irq_registry_register(&registry, 11, callback, &extras[3]),
            "full shared line should reject another handler");
    require(!irq_registry_register(&registry, 16, callback, &extras[3]),
            "invalid IRQ should be rejected");
    require(!irq_registry_unregister(&registry, 11, callback, &first),
            "removing an absent handler should report false");

    puts("Shared IRQ registry tests passed.");
    return 0;
}
