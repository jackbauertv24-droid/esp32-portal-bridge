// Devices on the hotspot: who is connected (name, address, signal), how much
// data each one uses, and blocking a device from the internet.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

struct pbuf;
struct netif;

void devices_start(void);
// Called from the lwIP input hook for every received IPv4 packet. Returns 1 if
// the packet was dropped (blocked device), else 0.
int devices_input_hook(struct pbuf *p, struct netif *inp);
cJSON *devices_json(void);
// mac is "aa:bb:cc:dd:ee:ff". name NULL = unchanged; blocked < 0 = unchanged.
bool devices_set(const char *mac, const char *name, int blocked, char *err, size_t errcap);
bool devices_kick(const char *mac, char *err, size_t errcap);
bool devices_forget(const char *mac, char *err, size_t errcap);
void devices_summary(char *out, size_t cap);
