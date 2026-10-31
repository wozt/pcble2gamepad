/*
 * Diagnostic tool to test single-adapter dual BLE identity capability.
 * This attempts to create two connectable advertising sets with distinct
 * random addresses on one HCI device, which is required for emulating
 * two independent Joy-Con 2 controllers simultaneously.
 *
 * This is a READ-ONLY diagnostic; it never modifies firmware or NVM.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#define TRUE 1
#define FALSE 0
typedef int gboolean;

#define OCF_LE_READ_NUM_SUPPORTED_ADV_SETS 0x0043
#define OCF_LE_SET_EXT_ADV_PARAM 0x0044
#define OCF_LE_SET_EXT_ADV_DATA 0x0045
#define OCF_LE_SET_EXT_ADV_ENABLE 0x0046
#define OCF_LE_SET_ADV_SET_RAND_ADDR 0x0047

typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  num_sets;
} LeNumAdvSetsReply;

typedef struct __attribute__((packed)) {
    uint16_t min_interval;
    uint16_t max_interval;
    uint16_t   primary_phy;
    uint8_t    secondary_max_skip;
    uint8_t    secondary_phy;
    uint8_t    own_addr_type;
    uint8_t    peer_addr_type;
    uint8_t    peer_addr[6];
    uint8_t    adv_channel_map;
    uint8_t    filter_policy;
} LeExtAdvParam;

typedef struct __attribute__((packed)) {
    uint8_t  status;
} LeSetAdvParamReply;

typedef struct __attribute__((packed)) {
    uint8_t  adv_handle;
    uint8_t  operation;
    uint8_t  fragment_pref;
    uint8_t  length;
    uint8_t  data[251];
} LeExtAdvData;

typedef struct __attribute__((packed)) {
    uint8_t  enable;
    uint8_t  num_sets;
    uint8_t  data[];
} LeExtAdvEnable;

typedef struct __attribute__((packed)) {
    uint8_t  adv_handle;
    uint8_t  random_addr[6];
} LeSetRandomAddr;

static gboolean read_num_adv_sets(int fd, uint8_t *num_sets)
{
    LeNumAdvSetsReply reply = {0};
    struct hci_request request = {
        .ogf = OGF_LE_CTL, .ocf = OCF_LE_READ_NUM_SUPPORTED_ADV_SETS,
        .rparam = &reply, .rlen = sizeof(reply),
    };
    if (hci_send_req(fd, &request, 2000) < 0) {
        fprintf(stderr, "LE Read Number of Supported Advertising Sets failed: %s\n", strerror(errno));
        return FALSE;
    }
    if (request.rlen != sizeof(reply) || reply.status != 0) {
        fprintf(stderr, "Invalid response (length %d, status 0x%02x)\n",
                request.rlen, reply.status);
        return FALSE;
    }
    *num_sets = reply.num_sets;
    return TRUE;
}

static gboolean set_random_address(int fd, uint8_t handle, const uint8_t addr[6])
{
    uint8_t reply = 0;
    LeSetRandomAddr cmd = { .adv_handle = handle };
    memcpy(cmd.random_addr, addr, 6);

    struct hci_request request = {
        .ogf = OGF_LE_CTL, .ocf = OCF_LE_SET_ADV_SET_RAND_ADDR,
        .cparam = &cmd, .clen = sizeof(cmd),
        .rparam = &reply, .rlen = sizeof(reply),
    };
    if (hci_send_req(fd, &request, 2000) < 0) {
        fprintf(stderr, "LE Set Advertising Set Random Address failed: %s\n", strerror(errno));
        return FALSE;
    }
    if (reply != 0) {
        fprintf(stderr, "Set random address failed with status 0x%02x\n", reply);
        return FALSE;
    }
    return TRUE;
}

static gboolean set_adv_parameters(int fd, uint8_t handle)
{
    (void)handle;  /* Parameter reserved for future use */
    uint8_t reply = 0;
    LeExtAdvParam cmd = {
        .min_interval = 0x0020,  /* 20 ms */
        .max_interval = 0x0020,  /* 20 ms */
        .primary_phy = 0x01,     /* LE 1M */
        .secondary_max_skip = 0,
        .secondary_phy = 0x01,   /* LE 1M */
        .own_addr_type = 0x01,   /* Random address */
        .peer_addr_type = 0x00,  /* Public (irrelevant for undirected) */
        .adv_channel_map = 0x07, /* All channels */
        .filter_policy = 0x00,   /* Allow any */
    };
    memset(cmd.peer_addr, 0, 6);

    struct hci_request request = {
        .ogf = OGF_LE_CTL, .ocf = OCF_LE_SET_EXT_ADV_PARAM,
        .cparam = &cmd, .clen = sizeof(cmd),
        .rparam = &reply, .rlen = sizeof(reply),
    };
    if (hci_send_req(fd, &request, 2000) < 0) {
        fprintf(stderr, "LE Set Extended Advertising Parameters failed: %s\n", strerror(errno));
        return FALSE;
    }
    if (reply != 0) {
        fprintf(stderr, "Set advertising parameters failed with status 0x%02x\n", reply);
        return FALSE;
    }
    return TRUE;
}

static gboolean set_adv_data(int fd, uint8_t handle)
{
    uint8_t reply = 0;
    LeExtAdvData cmd = {
        .adv_handle = handle,
        .operation = 0x03,  /* Complete */
        .fragment_pref = 0x00,
        .length = 31,
        /* Flags + Manufacturer Data (Nintendo format) */
        .data = {
            0x02, 0x01, 0x06,  /* Flags: BR/EDR not supported, general discoverable */
            0x1b, 0xff, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x66, 0x20,
            0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        }
    };

    struct hci_request request = {
        .ogf = OGF_LE_CTL, .ocf = OCF_LE_SET_EXT_ADV_DATA,
        .cparam = &cmd, .clen = sizeof(cmd),
        .rparam = &reply, .rlen = sizeof(reply),
    };
    if (hci_send_req(fd, &request, 2000) < 0) {
        fprintf(stderr, "LE Set Extended Advertising Data failed: %s\n", strerror(errno));
        return FALSE;
    }
    if (reply != 0) {
        fprintf(stderr, "Set advertising data failed with status 0x%02x\n", reply);
        return FALSE;
    }
    return TRUE;
}

static gboolean enable_advertising(int fd, gboolean enable, uint8_t num_sets)
{
    uint8_t reply = 0;
    uint8_t buffer[256];
    LeExtAdvEnable *cmd = (LeExtAdvEnable *)buffer;
    cmd->enable = enable ? 0x01 : 0x00;
    cmd->num_sets = num_sets;

    /* For each set, specify handle and duration (0 = infinite) */
    for (uint8_t i = 0; i < num_sets; i++) {
        cmd->data[i * 4] = i;        /* Handle */
        cmd->data[i * 4 + 1] = 0;    /* Duration: 0 = no limit */
        cmd->data[i * 4 + 2] = 0;    /* Max events: 0 = no limit */
    }

    struct hci_request request = {
        .ogf = OGF_LE_CTL, .ocf = OCF_LE_SET_EXT_ADV_ENABLE,
        .cparam = cmd, .clen = 2 + num_sets * 4,
        .rparam = &reply, .rlen = sizeof(reply),
    };
    if (hci_send_req(fd, &request, 2000) < 0) {
        fprintf(stderr, "LE Set Extended Advertising Enable failed: %s\n", strerror(errno));
        return FALSE;
    }
    if (reply != 0) {
        fprintf(stderr, "Enable advertising failed with status 0x%02x\n", reply);
        return FALSE;
    }
    return TRUE;
}

static void generate_random_address(uint8_t addr[6], uint8_t handle)
{
    /* Generate locally administered random static addresses.
     * Format: 11xxxx for random static, then unique bytes */
    addr[0] = 0xC0 | (handle << 4);  /* Random static, distinct per handle */
    static int seeded = 0;
    if (!seeded) {
        srand(time(NULL));
        seeded = 1;
    }
    for (int i = 1; i < 6; i++)
        addr[i] = rand() % 256;
}

int main(int argc, char **argv)
{
    const char *adapter = argc == 2 ? argv[1] : "hci0";
    if (argc > 2 || strncmp(adapter, "hci", 3) != 0 ||
        adapter[3] == '\0' || strspn(adapter + 3, "0123456789") != strlen(adapter + 3) ||
        strtoul(adapter + 3, NULL, 10) > 65535) {
        fprintf(stderr, "Usage: %s [hciN]\n", argv[0]);
        return 2;
    }

    printf("=== Dual BLE Identity Diagnostic ===\n");
    printf("Adapter: %s\n", adapter);
    printf("Purpose: Test if one adapter can host two independent advertising sets\n");
    printf("          with distinct random addresses (required for dual Joy-Con 2).\n\n");

    int id = hci_devid(adapter);
    int fd = id < 0 ? -1 : hci_open_dev(id);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", adapter, strerror(errno));
        fprintf(stderr, "This diagnostic requires root privileges.\n");
        return 1;
    }

    uint8_t num_sets = 0;
    if (!read_num_adv_sets(fd, &num_sets)) {
        close(fd);
        return 1;
    }

    printf("Step 1: LE Read Number of Supported Advertising Sets\n");
    printf("  Result: %u advertising sets supported\n", num_sets);

    if (num_sets < 2) {
        printf("\n❌ FAILURE: Adapter supports only %u advertising set(s).\n", num_sets);
        printf("   At least 2 sets are required for dual Joy-Con 2 emulation.\n");
        close(fd);
        return 1;
    }
    printf("  ✓ PASS: Adapter supports 2+ advertising sets\n\n");

    printf("Step 2: Configure first advertising set (handle 0)\n");
    uint8_t addr0[6];
    generate_random_address(addr0, 0);
    printf("  Random address: %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr0[0], addr0[1], addr0[2], addr0[3], addr0[4], addr0[5]);

    if (!set_random_address(fd, 0, addr0) ||
        !set_adv_parameters(fd, 0) ||
        !set_adv_data(fd, 0)) {
        close(fd);
        return 1;
    }
    printf("  ✓ PASS: First set configured\n\n");

    printf("Step 3: Configure second advertising set (handle 1)\n");
    uint8_t addr1[6];
    generate_random_address(addr1, 1);
    printf("  Random address: %02X:%02X:%02X:%02X:%02X:%02X\n",
           addr1[0], addr1[1], addr1[2], addr1[3], addr1[4], addr1[5]);

    if (!set_random_address(fd, 1, addr1) ||
        !set_adv_parameters(fd, 1) ||
        !set_adv_data(fd, 1)) {
        printf("  ❌ FAILURE: Could not configure second advertising set\n");
        printf("  Cleanup: Disabling first set...\n");
        enable_advertising(fd, FALSE, 1);
        close(fd);
        return 1;
    }
    printf("  ✓ PASS: Second set configured\n\n");

    printf("Step 4: Enable both advertising sets simultaneously\n");
    if (!enable_advertising(fd, TRUE, 2)) {
        printf("  ❌ FAILURE: Could not enable dual advertising\n");
        printf("  Cleanup: Disabling advertising...\n");
        enable_advertising(fd, FALSE, 2);
        close(fd);
        return 1;
    }
    printf("  ✓ PASS: Both advertising sets enabled\n\n");

    printf("Step 5: Verify with external scanner (manual check required)\n");
    printf("  Use 'bluetoothctl scan on' or nRF Connect on another device.\n");
    printf("  Expected: TWO distinct BLE devices with the random addresses above.\n");
    printf("  Press Enter to disable advertising and finish...\n");
    getchar();

    printf("\nStep 6: Cleanup - disable advertising\n");
    if (!enable_advertising(fd, FALSE, 2)) {
        printf("  ⚠ WARNING: Cleanup failed; advertising may still be active\n");
    } else {
        printf("  ✓ PASS: Advertising disabled\n");
    }

    close(fd);
    printf("\n=== DIAGNOSTIC COMPLETE ===\n");
    printf("Result: Adapter appears to support dual independent advertising sets.\n");
    printf("Next: Verify over-the-air with an external BLE scanner.\n");
    return 0;
}
