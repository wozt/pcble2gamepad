#include "pairing.h"
#include <string.h>

static void crypto_capture_vector(void)
{
    static const uint8_t a1[16] = {
        0x35,0x03,0xe9,0x29,0x82,0x87,0x71,0x24,
        0xbe,0xa8,0x0c,0x66,0x46,0x15,0x83,0x4b
    };
    static const uint8_t b1[16] = {
        0x5c,0xf6,0xee,0x79,0x2c,0xdf,0x05,0xe1,
        0xba,0x2b,0x63,0x25,0xc4,0x1a,0x5f,0x10
    };
    static const uint8_t a2[16] = {
        0x6f,0xc6,0xdf,0x8a,0xd8,0xfe,0xdf,0x15,
        0xbb,0x8c,0x15,0xe9,0x1f,0x32,0x05,0x44
    };
    static const uint8_t expected_ltk[16] = {
        0x69,0xf5,0x07,0x50,0xae,0x58,0x74,0xc5,
        0x04,0x83,0x6f,0x43,0x82,0x0f,0xdc,0x5b
    };
    static const uint8_t expected_b2[16] = {
        0x13,0x4c,0x97,0xf5,0x11,0xb9,0xb6,0xdd,
        0x4d,0x86,0xfd,0x40,0xf5,0x36,0xe9,0xed
    };
    uint8_t ltk[16], b2[16];
    g_autoptr(GError) error = NULL;

    pairing_compute_ltk(a1, b1, ltk);
    g_assert_cmpmem(ltk, sizeof(ltk), expected_ltk, sizeof(expected_ltk));
    g_assert_true(pairing_compute_b2(ltk, a2, b2, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(b2, sizeof(b2), expected_b2, sizeof(expected_b2));
}

static void parse_capture_vectors(void)
{
    static const uint8_t address_data[] = {
        0x00,0x02,0x81,0xeb,0x3a,0xeb,0xf1,0x48,
        0x80,0xeb,0x3a,0xeb,0xf1,0x48
    };
    static const uint8_t expected_address1[] = {0x48,0xf1,0xeb,0x3a,0xeb,0x81};
    static const uint8_t expected_address2[] = {0x48,0xf1,0xeb,0x3a,0xeb,0x80};
    static const uint8_t key_data[] = {
        0x00,0x35,0x03,0xe9,0x29,0x82,0x87,0x71,0x24,
        0xbe,0xa8,0x0c,0x66,0x46,0x15,0x83,0x4b
    };
    JcAddressExchange addresses = {0};
    JcKeyExchange keys = {0};
    g_autoptr(GError) error = NULL;

    g_assert_true(pairing_parse_address_exchange(address_data,
        sizeof(address_data), &addresses, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(addresses.host_addr1, sizeof(addresses.host_addr1),
                    expected_address1, sizeof(expected_address1));
    g_assert_cmpmem(addresses.host_addr2, sizeof(addresses.host_addr2),
                    expected_address2, sizeof(expected_address2));
    g_assert_true(pairing_parse_key_exchange(key_data, sizeof(key_data),
                                             &keys, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(keys.a1, sizeof(keys.a1), key_data + 1, 16);
}

static void reject_malformed_exchanges(void)
{
    uint8_t data[17] = {0};
    JcAddressExchange addresses = {0};
    JcKeyExchange keys = {0};
    g_autoptr(GError) error = NULL;

    g_assert_false(pairing_parse_address_exchange(data, 13, &addresses, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    data[0] = 1;
    g_assert_false(pairing_parse_key_exchange(data, sizeof(data), &keys, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pairing/crypto-capture-vector", crypto_capture_vector);
    g_test_add_func("/pairing/parse-capture-vectors", parse_capture_vectors);
    g_test_add_func("/pairing/reject-malformed", reject_malformed_exchanges);
    return g_test_run();
}
