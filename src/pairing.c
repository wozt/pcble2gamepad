#define _GNU_SOURCE
#include "pairing.h"
#include <string.h>
#include <openssl/aes.h>
#include <openssl/evp.h>

/* Nintendo proprietary Joy-Con 2 BLE pairing implementation
 * Based on ndeadly/switch2_controller_research captures
 */

/* Reverse byte order for little-endian AES operations */
static void reverse_bytes(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len / 2; i++) {
        uint8_t tmp = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = tmp;
    }
}

/* AES-128-ECB encryption */
static gboolean aes_128_ecb_encrypt(const uint8_t *key, const uint8_t *plaintext,
                                   uint8_t *ciphertext, GError **error)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create cipher context");
        return FALSE;
    }

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AES init failed");
        return FALSE;
    }

    int len, ciphertext_len;
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, 16) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AES encrypt failed");
        return FALSE;
    }
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AES final failed");
        return FALSE;
    }
    ciphertext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return TRUE;
}

/* Compute LTK = A1 XOR B1 */
static void compute_ltk(const uint8_t *a1, const uint8_t *b1, uint8_t *ltk) G_GNUC_UNUSED;
static void compute_ltk(const uint8_t *a1, const uint8_t *b1, uint8_t *ltk)
{
    for (int i = 0; i < 16; i++)
        ltk[i] = a1[i] ^ b1[i];
}

/* Subcommand 02: LTK confirmation - compute B2 = AES-128-ECB(reversed(LTK), reversed(A2)) */
gboolean pairing_compute_b2(const uint8_t *ltk, const uint8_t *a2, uint8_t *b2, GError **error)
{
    uint8_t key_rev[16], a2_rev[16];

    memcpy(key_rev, ltk, 16);
    memcpy(a2_rev, a2, 16);

    reverse_bytes(key_rev, 16);
    reverse_bytes(a2_rev, 16);

    return aes_128_ecb_encrypt(key_rev, a2_rev, b2, error);
}

/* Parse address exchange from command 0x15 subcommand 01 */
gboolean pairing_parse_address_exchange(const uint8_t *data, gsize len,
                                        JcAddressExchange *exchange, GError **error)
{
    if (len < 14) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Address exchange too short");
        return FALSE;
    }

    /* Skip selector byte and initial header */
    exchange->host_addr1[0] = data[6];
    exchange->host_addr1[1] = data[5];
    exchange->host_addr1[2] = data[4];
    exchange->host_addr1[3] = data[3];
    exchange->host_addr1[4] = data[2];
    exchange->host_addr1[5] = data[1];

    exchange->host_addr2[0] = data[12];
    exchange->host_addr2[1] = data[11];
    exchange->host_addr2[2] = data[10];
    exchange->host_addr2[3] = data[9];
    exchange->host_addr2[4] = data[8];
    exchange->host_addr2[5] = data[7];

    /* Controller address is in the response, not request */
    memset(exchange->controller_addr, 0, 6 * sizeof(uint8_t));

    return TRUE;
}

/* Parse key exchange from command 0x15 subcommand 04 */
gboolean pairing_parse_key_exchange(const uint8_t *data, gsize len,
                                     JcKeyExchange *exchange, GError **error)
{
    if (len < 18) {  /* 1 selector + 16 bytes + at least 1 */
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Key exchange too short");
        return FALSE;
    }

    /* Skip selector byte at data[0] */
    memcpy(exchange->a1, data + 1, 16);
    memset(exchange->b1, 0, 16 * sizeof(uint8_t));  /* B1 is sent in our response */

    return TRUE;
}
