// Injected into the lwIP build (see the top-level CMakeLists.txt) to replace
// ESP-IDF's default source-route hook with the load balancer's (main/lb.c).
#pragma once
#include "lwip/ip4_addr.h"

struct netif;
struct netif *pb_route_hook(const ip4_addr_t *src, const ip4_addr_t *dest);

#undef LWIP_HOOK_IP4_ROUTE_SRC
#define LWIP_HOOK_IP4_ROUTE_SRC pb_route_hook

// Data meter (main/meter.c): counts received bytes per interface.
struct pbuf;
int pb_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(p, inp) pb_input_hook(p, inp)
