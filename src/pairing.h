#pragma once
#include <gio/gio.h>
#include <stdint.h>

typedef struct {
    uint8_t host_addr1[6];
    uint8_t host_addr2[6];
    uint8_t controller_addr[6];
} JcAddressExchange;

typedef struct {
    uint8_t a1[16];
    uint8_t b1[16];
} JcKeyExchange;

gboolean pairing_compute_b2(const uint8_t *ltk, const uint8_t *a2, uint8_t *b2, GError **error);
gboolean pairing_parse_address_exchange(const uint8_t *data, gsize len,
                                        JcAddressExchange *exchange, GError **error);
gboolean pairing_parse_key_exchange(const uint8_t *data, gsize len,
                                     JcKeyExchange *exchange, GError **error);
