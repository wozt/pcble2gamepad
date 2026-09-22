#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* Only the two fixed, read-only LE capability commands are exposed. */
static gboolean read_capabilities(int fd, uint16_t command, uint8_t bytes[8])
{
    uint8_t reply[9] = {0};
    struct hci_request request = {
        .ogf = OGF_LE_CTL, .ocf = command,
        .rparam = reply, .rlen = sizeof(reply),
    };
    if (hci_send_req(fd, &request, 2000) < 0) {
        int saved_errno = errno;
        fprintf(stderr, "Capability read failed: %s\n", strerror(saved_errno));
        if (saved_errno == EPERM || saved_errno == EACCES)
            fprintf(stderr, "Run this read-only diagnostic with sudo; keep the application unprivileged.\n");
        return FALSE;
    }
    if (request.rlen != sizeof(reply) || reply[0] != 0) {
        fprintf(stderr, "Invalid capability response (length %d, status 0x%02x)\n",
                request.rlen, reply[0]);
        return FALSE;
    }
    memcpy(bytes, reply + 1, 8);
    return TRUE;
}

static void print_bytes(const uint8_t bytes[8])
{
    for (unsigned i = 0; i < 8; i++)
        printf("%02x", bytes[i]);
}

int main(int argc, char **argv)
{
    const char *adapter = argc == 2 ? argv[1] : "hci0";
    if (argc > 2 || !g_str_has_prefix(adapter, "hci") ||
        adapter[3] == '\0' || strspn(adapter + 3, "0123456789") != strlen(adapter + 3) ||
        g_ascii_strtoull(adapter + 3, NULL, 10) > G_MAXUINT16) {
        fprintf(stderr, "Usage: %s [hciN]\n", argv[0]);
        return 2;
    }
    int id = hci_devid(adapter);
    int fd = id < 0 ? -1 : hci_open_dev(id);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s. This read-only diagnostic may require sudo.\n",
                adapter, strerror(errno));
        return 1;
    }
    uint8_t features[8], states[8];
    gboolean ok = read_capabilities(fd, OCF_LE_READ_LOCAL_SUPPORTED_FEATURES, features) &&
                  read_capabilities(fd, OCF_LE_READ_SUPPORTED_STATES, states);
    close(fd);
    if (!ok)
        return 1;
    printf("{\n  \"adapter\": \"%s\",\n  \"le_features_hex\": \"", adapter);
    print_bytes(features);
    printf("\",\n  \"le_states_hex\": \"");
    print_bytes(states);
    /* Features: 2M bit 8, DLE bit 5. Linux 6.12 is_advertising_allowed()
     * requires state bits 38+21 (peripheral) or 35+19 (central). */
    printf("\",\n  \"le_2m_phy\": %s,\n  \"data_length_extension\": %s,\n"
           "  \"connectable_advertising_with_peripheral_link\": %s,\n"
           "  \"connectable_advertising_with_central_link\": %s\n}\n",
           features[1] & 0x01 ? "true" : "false",
           features[0] & 0x20 ? "true" : "false",
           (states[4] & 0x40) && (states[2] & 0x20) ? "true" : "false",
           (states[4] & 0x08) && (states[2] & 0x08) ? "true" : "false");
    return 0;
}
