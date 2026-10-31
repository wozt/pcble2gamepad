#define _GNU_SOURCE
#include "mgmt.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#define MGMT_OP_SET_PHY 0x0059

typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint16_t index;
    uint16_t len;
} MgmtHdr;

typedef struct __attribute__((packed)) {
    uint8_t  address_type;
    uint8_t  address[6];
    uint8_t  phy_tx;
    uint8_t  phy_rx;
    uint8_t  all_phys;
    uint8_t  options;
    uint8_t  phys;
} MgmtSetPhyParams;

gboolean mgmt_set_preferred_phy(const char *adapter, gboolean le_2m, GError **error)
{
    int id = hci_devid(adapter);
    if (id < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Adapter %s not found", adapter);
        return FALSE;
    }

    int fd = hci_open_dev(id);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Cannot open %s: %s", adapter, strerror(errno));
        return FALSE;
    }

    MgmtSetPhyParams params = {
        .address_type = 0x00,  /* Public address (all) */
        .address = {0},
        .phy_tx = 0x03,        /* Prefer 2M and 1M */
        .phy_rx = 0x03,        /* Prefer 2M and 1M */
        .all_phys = 0x03,      /* 2M and 1M supported */
        .options = 0x00,
        .phys = 0x03,          /* TX and RX */
    };

    if (!le_2m) {
        params.phy_tx = 0x01;  /* 1M only */
        params.phy_rx = 0x01;
        params.all_phys = 0x01;
    }

    uint8_t buffer[sizeof(MgmtHdr) + sizeof(params)];
    MgmtHdr *hdr = (MgmtHdr *)buffer;
    hdr->opcode = htole16(MGMT_OP_SET_PHY);
    hdr->index = htole16(id);
    hdr->len = htole16(sizeof(params));
    memcpy(buffer + sizeof(MgmtHdr), &params, sizeof(params));

    ssize_t written = write(fd, buffer, sizeof(buffer));
    close(fd);

    if (written < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "MGMT Set PHY failed: %s", strerror(errno));
        return FALSE;
    }

    if ((size_t)written != sizeof(buffer)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "MGMT Set PHY partial write");
        return FALSE;
    }

    return TRUE;
}
