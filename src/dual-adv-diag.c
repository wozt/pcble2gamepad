#define _DEFAULT_SOURCE

/*
 * Reversible dual extended-advertising diagnostic.
 *
 * This program requires exclusive HCI user-channel ownership. It refuses to
 * use the raw channel, where its commands could race with bluetoothd and the
 * kernel Bluetooth stack. It changes volatile controller state only and
 * clears both advertising sets before releasing the controller.
 */

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define OCF_LE_SET_ADV_SET_RANDOM_ADDRESS 0x0035
#define OCF_LE_SET_EXT_ADV_PARAMETERS     0x0036
#define OCF_LE_SET_EXT_ADV_DATA           0x0037
#define OCF_LE_SET_EXT_ADV_ENABLE         0x0039
#define OCF_LE_READ_NUM_ADV_SETS          0x003b
#define OCF_LE_CLEAR_ADV_SETS             0x003d

#define JC_LEFT_PID  0x2067
#define JC_RIGHT_PID 0x2066

typedef struct __attribute__((packed)) {
    uint8_t handle;
    uint16_t event_properties;
    uint8_t min_interval[3];
    uint8_t max_interval[3];
    uint8_t channel_map;
    uint8_t own_address_type;
    uint8_t peer_address_type;
    bdaddr_t peer_address;
    uint8_t filter_policy;
    int8_t tx_power;
    uint8_t primary_phy;
    uint8_t secondary_max_skip;
    uint8_t secondary_phy;
    uint8_t sid;
    uint8_t scan_request_notification;
} LeSetExtAdvParameters;

typedef struct __attribute__((packed)) {
    uint8_t handle;
    bdaddr_t address;
} LeSetAdvSetRandomAddress;

typedef struct __attribute__((packed)) {
    uint8_t handle;
    uint16_t duration;
    uint8_t max_events;
} LeExtAdvSet;

static volatile sig_atomic_t interrupted;

static void interrupt_handler(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static bool send_hci_command(int fd, uint16_t ogf, uint16_t ocf,
                             const void *parameters, uint8_t parameter_length,
                             uint8_t *response, size_t *response_length)
{
    uint8_t packet[1 + HCI_COMMAND_HDR_SIZE + UINT8_MAX] = {HCI_COMMAND_PKT};
    uint16_t opcode = cmd_opcode_pack(ogf, ocf);
    hci_command_hdr header = {
        .opcode = htobs(opcode),
        .plen = parameter_length,
    };
    memcpy(packet + 1, &header, sizeof(header));
    if (parameter_length)
        memcpy(packet + 1 + sizeof(header), parameters, parameter_length);

    size_t packet_length = 1 + sizeof(header) + parameter_length;
    if (write(fd, packet, packet_length) != (ssize_t)packet_length) {
        fprintf(stderr, "HCI command 0x%04x write failed: %s\n",
                opcode, strerror(errno));
        return false;
    }

    for (;;) {
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        int ready = poll(&poll_fd, 1, 2000);
        if (ready < 0 && errno == EINTR && !interrupted)
            continue;
        if (ready <= 0) {
            fprintf(stderr, "HCI command 0x%04x timed out%s\n", opcode,
                    interrupted ? " after interruption" : "");
            return false;
        }

        uint8_t event[HCI_MAX_EVENT_SIZE + 1];
        ssize_t event_length = read(fd, event, sizeof(event));
        if (event_length < 0 && errno == EINTR && !interrupted)
            continue;
        if (event_length < 3 || event[0] != HCI_EVENT_PKT)
            continue;
        if ((size_t)event_length != (size_t)event[2] + 3)
            continue;

        if (event[1] == EVT_CMD_STATUS && event[2] >= 4) {
            uint16_t event_opcode;
            memcpy(&event_opcode, event + 5, sizeof(event_opcode));
            if (btohs(event_opcode) != opcode)
                continue;
            fprintf(stderr, "HCI command 0x%04x failed with status 0x%02x\n",
                    opcode, event[3]);
            return false;
        }
        if (event[1] != EVT_CMD_COMPLETE || event[2] < 3)
            continue;

        uint16_t event_opcode;
        memcpy(&event_opcode, event + 4, sizeof(event_opcode));
        if (btohs(event_opcode) != opcode)
            continue;
        size_t available = event[2] - 3;
        if (available > *response_length) {
            fprintf(stderr, "HCI command 0x%04x response is too large\n", opcode);
            return false;
        }
        memcpy(response, event + 6, available);
        *response_length = available;
        return true;
    }
}

static bool command_status_only(int fd, uint16_t ocf, const void *parameters,
                                uint8_t parameter_length)
{
    uint8_t status = 0xff;
    size_t length = sizeof(status);
    if (!send_hci_command(fd, OGF_LE_CTL, ocf, parameters, parameter_length,
                          &status, &length))
        return false;
    if (length != 1 || status != 0) {
        fprintf(stderr, "LE command OCF 0x%04x returned length %zu, status 0x%02x\n",
                ocf, length, status);
        return false;
    }
    return true;
}

static bool controller_reset(int fd)
{
    uint8_t status = 0xff;
    size_t length = sizeof(status);
    return send_hci_command(fd, OGF_HOST_CTL, OCF_RESET, NULL, 0,
                            &status, &length) && length == 1 && status == 0;
}

static bool read_num_adv_sets(int fd, uint8_t *num_sets)
{
    uint8_t reply[2] = {0};
    size_t length = sizeof(reply);
    if (!send_hci_command(fd, OGF_LE_CTL, OCF_LE_READ_NUM_ADV_SETS, NULL, 0,
                          reply, &length))
        return false;
    if (length != sizeof(reply) || reply[0] != 0) {
        fprintf(stderr, "LE Read Number of Supported Advertising Sets returned "
                "length %zu, status 0x%02x\n", length, reply[0]);
        return false;
    }
    *num_sets = reply[1];
    return true;
}

static void set_u24(uint8_t output[3], uint32_t value)
{
    output[0] = value & 0xff;
    output[1] = (value >> 8) & 0xff;
    output[2] = (value >> 16) & 0xff;
}

static bool set_random_address(int fd, uint8_t handle, const bdaddr_t *address)
{
    LeSetAdvSetRandomAddress command = {.handle = handle};
    bacpy(&command.address, address);
    return command_status_only(fd, OCF_LE_SET_ADV_SET_RANDOM_ADDRESS,
                               &command, sizeof(command));
}

static bool set_adv_parameters(int fd, uint8_t handle)
{
    LeSetExtAdvParameters command = {
        .handle = handle,
        /* Connectable + scannable + legacy PDU = ADV_IND. */
        .event_properties = htole16(0x0013),
        .channel_map = 0x07,
        .own_address_type = 0x01,
        .peer_address_type = 0x00,
        .filter_policy = 0x00,
        .tx_power = 0x7f,
        .primary_phy = 0x01,
        .secondary_max_skip = 0,
        .secondary_phy = 0x01,
        .sid = handle,
        .scan_request_notification = 0,
    };
    set_u24(command.min_interval, 0x20);
    set_u24(command.max_interval, 0x20);

    uint8_t reply[2] = {0};
    size_t length = sizeof(reply);
    if (!send_hci_command(fd, OGF_LE_CTL, OCF_LE_SET_EXT_ADV_PARAMETERS,
                          &command, sizeof(command), reply, &length))
        return false;
    if (length != sizeof(reply) || reply[0] != 0) {
        fprintf(stderr, "LE Set Extended Advertising Parameters returned "
                "length %zu, status 0x%02x\n", length, reply[0]);
        return false;
    }
    return true;
}

static bool set_adv_data(int fd, uint8_t handle, uint16_t product_id)
{
    static const uint8_t prefix[] = {
        0x02,0x01,0x06,0x1b,0xff,0x53,0x05,0x01,0x00,0x03,0x7e,0x05
    };
    uint8_t command[4 + 31] = {handle, 0x03, 0x00, 31};
    memcpy(command + 4, prefix, sizeof(prefix));
    command[4 + 12] = product_id & 0xff;
    command[4 + 13] = product_id >> 8;
    command[4 + 15] = 0x01;
    command[4 + 24] = 0x0f;
    return command_status_only(fd, OCF_LE_SET_EXT_ADV_DATA,
                               command, sizeof(command));
}

static bool enable_advertising(int fd, bool enable)
{
    uint8_t command[2 + 2 * sizeof(LeExtAdvSet)] = {
        enable ? 0x01 : 0x00, 0x02,
    };
    LeExtAdvSet sets[2] = {
        {.handle = 0, .duration = htole16(0), .max_events = 0},
        {.handle = 1, .duration = htole16(0), .max_events = 0},
    };
    memcpy(command + 2, sets, sizeof(sets));
    return command_status_only(fd, OCF_LE_SET_EXT_ADV_ENABLE,
                               command, sizeof(command));
}

static bool generate_random_address(bdaddr_t *address, uint8_t discriminator)
{
    if (getrandom(address->b, sizeof(address->b), 0) != sizeof(address->b)) {
        fprintf(stderr, "Secure random-address generation failed: %s\n",
                strerror(errno));
        return false;
    }
    /* bdaddr_t is least-significant octet first. The displayed first octet is
     * b[5], whose two high bits identify a static random address. */
    address->b[5] = (address->b[5] & 0x3f) | 0xc0;
    address->b[0] ^= discriminator;
    return true;
}

static int open_user_channel(int device_id)
{
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0)
        return -1;
    struct sockaddr_hci address = {
        .hci_family = AF_BLUETOOTH,
        .hci_dev = device_id,
        .hci_channel = HCI_CHANNEL_USER,
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    const char *adapter = argc == 2 ? argv[1] : NULL;
    if (!adapter || argc != 2) {
        fprintf(stderr, "Usage: %s <hciN|adapter-address>\n", argv[0]);
        return 2;
    }
    int device_id = hci_devid(adapter);
    if (device_id < 0) {
        fprintf(stderr, "Cannot resolve Bluetooth adapter %s: %s\n",
                adapter, strerror(errno));
        return 1;
    }

    int fd = open_user_channel(device_id);
    if (fd < 0) {
        fprintf(stderr, "Cannot obtain exclusive HCI user-channel ownership of %s: %s\n",
                adapter, strerror(errno));
        fprintf(stderr, "Stop bluetoothd and take this adapter down before retrying. "
                "Do not run this diagnostic while BlueZ owns the controller.\n");
        return 1;
    }

    struct sigaction action = {.sa_handler = interrupt_handler};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    bool advertising_enabled = false;
    int result = 1;
    uint8_t num_sets = 0;
    bdaddr_t addresses[2];

    printf("Dual BLE identity diagnostic on hci%d\n", device_id);
    printf("Exclusive HCI user-channel ownership acquired. BlueZ cannot compete.\n");
    if (!controller_reset(fd) || !read_num_adv_sets(fd, &num_sets))
        goto cleanup;
    printf("Controller reports %u extended advertising set(s).\n", num_sets);
    if (num_sets < 2) {
        fprintf(stderr, "At least two advertising sets are required.\n");
        goto cleanup;
    }
    if (!generate_random_address(&addresses[0], 0) ||
        !generate_random_address(&addresses[1], 1))
        goto cleanup;

    for (uint8_t handle = 0; handle < 2; handle++) {
        uint16_t product_id = handle == 0 ? JC_LEFT_PID : JC_RIGHT_PID;
        char address_text[18];
        ba2str(&addresses[handle], address_text);
        printf("Configuring handle %u as Joy-Con 2 %s, PID 0x%04x, address %s\n",
               handle, handle == 0 ? "LEFT" : "RIGHT", product_id,
               address_text);
        if (!set_random_address(fd, handle, &addresses[handle]) ||
            !set_adv_parameters(fd, handle) ||
            !set_adv_data(fd, handle, product_id))
            goto cleanup;
    }
    if (!enable_advertising(fd, true))
        goto cleanup;
    advertising_enabled = true;
    printf("Both connectable legacy advertising sets were accepted by the controller.\n");
    printf("Verify both addresses with an independent scanner, then press Enter to clean up.\n");
    while (!interrupted) {
        int character = getchar();
        if (character == '\n' || character == EOF)
            break;
    }
    result = 0;

cleanup:
    if (advertising_enabled && !enable_advertising(fd, false)) {
        fprintf(stderr, "Warning: disabling the advertising sets failed.\n");
        result = 1;
    }
    if (!command_status_only(fd, OCF_LE_CLEAR_ADV_SETS, NULL, 0)) {
        fprintf(stderr, "Warning: clearing the volatile advertising sets failed.\n");
        result = 1;
    }
    close(fd);
    printf("Exclusive controller ownership released. Restart BlueZ if it was stopped.\n");
    if (result == 0)
        printf("HCI command success is not over-the-air or simultaneous-connection proof.\n");
    return result;
}
