#include "salticidae/config.h"
#ifdef SALTICIDAE_CBINDINGS
#include "salticidae/netaddr.h"

extern "C" {

netaddr_t *netaddr_new() { return new netaddr_t(); }
netaddr_t *netaddr_new_from_ip_port(uint32_t ip, uint16_t port) {
    return new netaddr_t(ip, port);
}

netaddr_t *netaddr_new_from_sip_port(const char *ip, uint16_t port) {
    return new netaddr_t(ip, port);
}

netaddr_t *netaddr_new_from_sipport(const char *ip_port_addr) {
    return new netaddr_t(ip_port_addr);
}

bool netaddr_is_eq(const netaddr_t *a, const netaddr_t *b) {
    return *a == *b;
}

bool netaddr_is_null(const netaddr_t *self) { return self->is_null(); }

}

#endif