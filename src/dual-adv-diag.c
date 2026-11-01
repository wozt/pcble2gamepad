#define _DEFAULT_SOURCE

/*
 * Reversible dual BLE identity and connection-concurrency diagnostic.
 *
 * This program requires exclusive HCI user-channel ownership. It refuses to
 * use the raw channel, where its commands could race with bluetoothd and the
 * kernel Bluetooth stack. It changes volatile controller state only: it
 * advertises two connectable sets with distinct static random addresses,
 * records which set accepts each incoming LE connection, keeps links alive
 * with minimal ATT/SMP error responses and resets the controller on exit.
 *
 * Because the kernel and BlueZ are bypassed, a positive result demonstrates a
 * controller capability only. Kernel LE-state policy is not exercised here.
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
#include <time.h>
#include <unistd.h>

#define OCF_LE_SET_ADV_SET_RANDOM_ADDRESS 0x0035
#define OCF_LE_SET_EXT_ADV_PARAMETERS     0x0036
#define OCF_LE_SET_EXT_ADV_DATA           0x0037
#define OCF_LE_SET_EXT_ADV_ENABLE         0x0039
#define OCF_LE_READ_NUM_ADV_SETS          0x003b
#define OCF_LE_CLEAR_ADV_SETS             0x003d
#define OCF_LE_SET_PHY_DIAG               0x0032

#define DIAG_LE_CONN_COMPLETE          0x01
#define DIAG_LE_CONN_UPDATE_COMPLETE   0x03
#define DIAG_LE_LTK_REQUEST            0x05
#define DIAG_LE_DATA_LENGTH_CHANGE     0x07
#define DIAG_LE_ENH_CONN_COMPLETE      0x0a
#define DIAG_LE_PHY_UPDATE_COMPLETE    0x0c
#define DIAG_LE_ADV_SET_TERMINATED     0x12

#define L2CAP_CID_ATT 0x0004
#define L2CAP_CID_SMP 0x0006

#define JC_LEFT_PID  0x2067
#define JC_RIGHT_PID 0x2066

#define MAX_LINKS 8

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

typedef struct {
    bool active;
    uint16_t handle;
    int adv_handle;
    bdaddr_t peer;
    uint8_t peer_type;
    uint8_t role;
    uint8_t tx_phy, rx_phy;
    bool phy_requested;
    bool ltk_pending;
} Link;

static volatile sig_atomic_t interrupted;
static Link links[MAX_LINKS];
static unsigned active_links, peak_links;
static bool distinct_identities_concurrent;
static bool request_le_2m;

static void interrupt_handler(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static uint16_t get_le16(const uint8_t *data)
{
    return (uint16_t)(data[0] | data[1] << 8);
}

static void put_le16(uint8_t *data, uint16_t value)
{
    data[0] = value & 0xff;
    data[1] = value >> 8;
}

static const char *identity_name(int adv_handle)
{
    switch (adv_handle) {
    case 0: return "LEFT";
    case 1: return "RIGHT";
    default: return "unknown";
    }
}

static const char *phy_name(uint8_t phy)
{
    switch (phy) {
    case 1: return "1M";
    case 2: return "2M";
    case 3: return "Coded";
    default: return "?";
    }
}

static Link *find_link(uint16_t handle)
{
    for (size_t i = 0; i < MAX_LINKS; i++)
        if (links[i].active && links[i].handle == handle)
            return &links[i];
    return NULL;
}

static void update_concurrency(void)
{
    if (active_links > peak_links)
        peak_links = active_links;
    bool seen[2] = {false, false};
    for (size_t i = 0; i < MAX_LINKS; i++)
        if (links[i].active && links[i].adv_handle >= 0 && links[i].adv_handle < 2)
            seen[links[i].adv_handle] = true;
    if (seen[0] && seen[1] && !distinct_identities_concurrent) {
        distinct_identities_concurrent = true;
        printf(">>> Two concurrent peripheral links on distinct advertising identities.\n");
    }
}

static void send_l2cap(int fd, uint16_t handle, uint16_t cid,
                       const uint8_t *payload, uint16_t length)
{
    uint8_t packet[1 + 4 + 4 + 32];
    if (length > 32)
        return;
    packet[0] = HCI_ACLDATA_PKT;
    /* Packet boundary 00: first non-automatically-flushable fragment. */
    put_le16(packet + 1, handle & 0x0fff);
    put_le16(packet + 3, length + 4);
    put_le16(packet + 5, length);
    put_le16(packet + 7, cid);
    memcpy(packet + 9, payload, length);
    size_t total = 9 + length;
    if (write(fd, packet, total) != (ssize_t)total)
        fprintf(stderr, "ACL write on link 0x%04x failed: %s\n", handle, strerror(errno));
}

/* Keep a generic central from hitting the 30 s ATT transaction timeout. No
 * Nintendo GATT database is exposed by this diagnostic. */
static void handle_att(int fd, Link *link, const uint8_t *pdu, uint16_t length)
{
    if (length < 1)
        return;
    uint8_t opcode = pdu[0];
    if (opcode == 0x02) {
        uint8_t response[3] = {0x03};
        put_le16(response + 1, 23);
        send_l2cap(fd, link->handle, L2CAP_CID_ATT, response, sizeof(response));
        printf("link 0x%04x (%s): ATT MTU exchange answered with 23\n",
               link->handle, identity_name(link->adv_handle));
        return;
    }
    uint8_t error_code;
    switch (opcode) {
    case 0x04: case 0x06: case 0x08: case 0x10:
        error_code = 0x0a; /* Attribute Not Found */
        break;
    case 0x0a: case 0x0c: case 0x12:
        error_code = 0x01; /* Invalid Handle */
        break;
    case 0x0e: case 0x16: case 0x18: case 0x20:
        error_code = 0x06; /* Request Not Supported */
        break;
    default:
        return; /* Commands, confirmations and responses need no reply. */
    }
    uint8_t response[5] = {0x01, opcode, 0, 0, error_code};
    if (length >= 3)
        memcpy(response + 2, pdu + 1, 2);
    send_l2cap(fd, link->handle, L2CAP_CID_ATT, response, sizeof(response));
    printf("link 0x%04x (%s): ATT request 0x%02x answered with error 0x%02x\n",
           link->handle, identity_name(link->adv_handle), opcode, error_code);
}

static void handle_acl(int fd, const uint8_t *packet, size_t length)
{
    if (length < 9)
        return;
    uint16_t handle = get_le16(packet + 1) & 0x0fff;
    uint8_t boundary = (packet[2] >> 4) & 0x03;
    Link *link = find_link(handle);
    if (!link || boundary == 0x01)
        return; /* Continuation fragments are not reassembled here. */
    uint16_t l2cap_length = get_le16(packet + 5);
    uint16_t cid = get_le16(packet + 7);
    if ((size_t)l2cap_length + 9 > length)
        return;
    const uint8_t *payload = packet + 9;
    if (cid == L2CAP_CID_ATT) {
        handle_att(fd, link, payload, l2cap_length);
    } else if (cid == L2CAP_CID_SMP && l2cap_length >= 1 && payload[0] == 0x01) {
        static const uint8_t pairing_failed[] = {0x05, 0x05}; /* Pairing Not Supported */
        send_l2cap(fd, handle, L2CAP_CID_SMP, pairing_failed, sizeof(pairing_failed));
        printf("link 0x%04x (%s): SMP pairing request rejected\n",
               handle, identity_name(link->adv_handle));
    }
}

static void connection_completed(uint8_t status, uint16_t handle, uint8_t role,
                                 uint8_t peer_type, const uint8_t *peer)
{
    if (status != 0) {
        printf("LE connection attempt failed with status 0x%02x\n", status);
        return;
    }
    Link *link = NULL;
    for (size_t i = 0; i < MAX_LINKS && !link; i++)
        if (!links[i].active)
            link = &links[i];
    if (!link) {
        printf("LE connection 0x%04x exceeds the diagnostic link table\n", handle);
        return;
    }
    *link = (Link){.active = true, .handle = handle, .adv_handle = -1,
                   .role = role, .peer_type = peer_type, .tx_phy = 1, .rx_phy = 1};
    memcpy(link->peer.b, peer, 6);
    active_links++;
    char address[18];
    ba2str(&link->peer, address);
    printf("LE link 0x%04x up: role=%s peer=%s (type %u); active links=%u\n",
           handle, role == 1 ? "peripheral" : "central", address, peer_type,
           active_links);
}

static void handle_le_meta(const uint8_t *p, size_t length)
{
    if (length < 1)
        return;
    uint8_t subevent = p[0];
    p++;
    length--;
    switch (subevent) {
    case DIAG_LE_CONN_COMPLETE:
        if (length >= 11)
            connection_completed(p[0], get_le16(p + 1), p[3], p[4], p + 5);
        break;
    case DIAG_LE_ENH_CONN_COMPLETE:
        if (length >= 23)
            connection_completed(p[0], get_le16(p + 1), p[3], p[4], p + 5);
        break;
    case DIAG_LE_ADV_SET_TERMINATED:
        if (length >= 5) {
            Link *link = find_link(get_le16(p + 2));
            if (p[0] == 0 && link) {
                link->adv_handle = p[1];
                printf("LE link 0x%04x was accepted by advertising set %u (%s identity)\n",
                       link->handle, p[1], identity_name(p[1]));
                update_concurrency();
            } else {
                printf("Advertising set %u terminated with status 0x%02x\n", p[1], p[0]);
            }
        }
        break;
    case DIAG_LE_PHY_UPDATE_COMPLETE:
        if (length >= 5) {
            Link *link = find_link(get_le16(p + 1));
            if (link && p[0] == 0) {
                link->tx_phy = p[3];
                link->rx_phy = p[4];
            }
            printf("link 0x%04x: PHY update status 0x%02x tx=%s rx=%s\n",
                   get_le16(p + 1), p[0], phy_name(p[3]), phy_name(p[4]));
        }
        break;
    case DIAG_LE_CONN_UPDATE_COMPLETE:
        if (length >= 9)
            printf("link 0x%04x: connection update status 0x%02x interval=%.2f ms\n",
                   get_le16(p + 1), p[0], get_le16(p + 3) * 1.25);
        break;
    case DIAG_LE_DATA_LENGTH_CHANGE:
        if (length >= 10)
            printf("link 0x%04x: data length tx=%u rx=%u octets\n",
                   get_le16(p), get_le16(p + 2), get_le16(p + 6));
        break;
    case DIAG_LE_LTK_REQUEST:
        if (length >= 12) {
            Link *link = find_link(get_le16(p));
            if (link)
                link->ltk_pending = true;
        }
        break;
    default:
        break;
    }
}

static void handle_async_packet(int fd, const uint8_t *packet, size_t length)
{
    if (length >= 1 && packet[0] == HCI_ACLDATA_PKT) {
        handle_acl(fd, packet, length);
        return;
    }
    if (length < 3 || packet[0] != HCI_EVENT_PKT || (size_t)packet[2] + 3 != length)
        return;
    const uint8_t *p = packet + 3;
    if (packet[1] == EVT_LE_META_EVENT) {
        handle_le_meta(p, packet[2]);
    } else if (packet[1] == EVT_DISCONN_COMPLETE && packet[2] >= 4) {
        Link *link = find_link(get_le16(p + 1));
        if (link) {
            printf("LE link 0x%04x (%s) down: reason 0x%02x; active links=%u\n",
                   link->handle, identity_name(link->adv_handle), p[3],
                   active_links - 1);
            link->active = false;
            active_links--;
        }
    }
}

static ssize_t read_packet(int fd, uint8_t *buffer, size_t size, int timeout_ms)
{
    for (;;) {
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        int ready = poll(&poll_fd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR && !interrupted)
            continue;
        if (ready <= 0)
            return ready;
        ssize_t length = read(fd, buffer, size);
        if (length < 0 && errno == EINTR && !interrupted)
            continue;
        return length;
    }
}

/* When accept_status is true, a successful Command Status completes the
 * command; LE Set PHY and similar commands report their outcome later. */
static bool send_hci_command_full(int fd, uint16_t ogf, uint16_t ocf,
                                  const void *parameters, uint8_t parameter_length,
                                  uint8_t *response, size_t *response_length,
                                  bool accept_status)
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
        uint8_t event[HCI_MAX_FRAME_SIZE + 1];
        ssize_t event_length = read_packet(fd, event, sizeof(event), 2000);
        if (event_length <= 0) {
            fprintf(stderr, "HCI command 0x%04x timed out%s\n", opcode,
                    interrupted ? " after interruption" : "");
            return false;
        }

        bool is_event = event_length >= 3 && event[0] == HCI_EVENT_PKT &&
                        (size_t)event_length == (size_t)event[2] + 3;
        if (is_event && event[1] == EVT_CMD_STATUS && event[2] >= 4 &&
            get_le16(event + 5) == opcode) {
            if (accept_status && event[3] == 0) {
                *response_length = 0;
                return true;
            }
            fprintf(stderr, "HCI command 0x%04x failed with status 0x%02x\n",
                    opcode, event[3]);
            return false;
        }
        if (!is_event || event[1] != EVT_CMD_COMPLETE || event[2] < 3 ||
            get_le16(event + 4) != opcode) {
            handle_async_packet(fd, event, (size_t)event_length);
            continue;
        }
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

static bool send_hci_command(int fd, uint16_t ogf, uint16_t ocf,
                             const void *parameters, uint8_t parameter_length,
                             uint8_t *response, size_t *response_length)
{
    return send_hci_command_full(fd, ogf, ocf, parameters, parameter_length,
                                 response, response_length, false);
}

static bool command_status_only_ogf(int fd, uint16_t ogf, uint16_t ocf,
                                    const void *parameters, uint8_t parameter_length)
{
    uint8_t status = 0xff;
    size_t length = sizeof(status);
    if (!send_hci_command(fd, ogf, ocf, parameters, parameter_length,
                          &status, &length))
        return false;
    if (length != 1 || status != 0) {
        fprintf(stderr, "HCI command OGF 0x%02x OCF 0x%04x returned length %zu, status 0x%02x\n",
                ogf, ocf, length, status);
        return false;
    }
    return true;
}

static bool command_status_only(int fd, uint16_t ocf, const void *parameters,
                                uint8_t parameter_length)
{
    return command_status_only_ogf(fd, OGF_LE_CTL, ocf, parameters, parameter_length);
}

static bool controller_reset(int fd)
{
    return command_status_only_ogf(fd, OGF_HOST_CTL, OCF_RESET, NULL, 0);
}

static bool set_event_masks(int fd)
{
    /* Default BR/EDR mask plus the LE Meta event (bit 61). */
    uint64_t mask = 0x00001fffffffffffULL | (1ULL << 61);
    /* LE: connection complete, connection update, remote features, LTK
     * request, data length change, enhanced connection complete, PHY update
     * and advertising set terminated. */
    uint64_t le_mask = (1ULL << 0) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) |
                       (1ULL << 6) | (1ULL << 9) | (1ULL << 11) | (1ULL << 17);
    uint64_t mask_le = htole64(mask), le_mask_le = htole64(le_mask);
    return command_status_only_ogf(fd, OGF_HOST_CTL, OCF_SET_EVENT_MASK,
                                   &mask_le, sizeof(mask_le)) &&
           command_status_only(fd, OCF_LE_SET_EVENT_MASK, &le_mask_le, sizeof(le_mask_le));
}

static bool read_le_bitmap(int fd, uint16_t ocf, const char *label, uint8_t bytes[8])
{
    uint8_t reply[9] = {0xff};
    size_t length = sizeof(reply);
    if (!send_hci_command(fd, OGF_LE_CTL, ocf, NULL, 0, reply, &length))
        return false;
    if (length != sizeof(reply) || reply[0] != 0) {
        fprintf(stderr, "%s returned length %zu, status 0x%02x\n", label, length, reply[0]);
        return false;
    }
    memcpy(bytes, reply + 1, 8);
    printf("%s: ", label);
    for (int i = 0; i < 8; i++)
        printf("%02x", bytes[i]);
    printf("\n");
    return true;
}

static bool bit_set(const uint8_t bytes[8], unsigned bit)
{
    return bytes[bit / 8] & (1u << (bit % 8));
}

static bool report_capabilities(int fd)
{
    uint8_t features[8], states[8];
    if (!read_le_bitmap(fd, OCF_LE_READ_LOCAL_SUPPORTED_FEATURES, "LE features", features) ||
        !read_le_bitmap(fd, OCF_LE_READ_SUPPORTED_STATES, "LE supported states", states))
        return false;
    printf("  Data Length Extension: %s\n", bit_set(features, 5) ? "yes" : "no");
    printf("  LE 2M PHY: %s\n", bit_set(features, 8) ? "yes" : "no");
    printf("  LE Coded PHY: %s\n", bit_set(features, 11) ? "yes" : "no");
    printf("  Extended advertising: %s\n", bit_set(features, 12) ? "yes" : "no");
    /* Bits referenced by the Linux connectable-advertising check, see
     * docs/validation.md. */
    printf("  State bits 21/38 (peripheral + connectable advertising): %s/%s\n",
           bit_set(states, 21) ? "yes" : "no", bit_set(states, 38) ? "yes" : "no");
    printf("  State bits 19/35 (central + connectable advertising): %s/%s\n",
           bit_set(states, 19) ? "yes" : "no", bit_set(states, 35) ? "yes" : "no");
    return true;
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

/* Execute work deferred from event handlers, which must not issue commands
 * while another command is waiting for its completion. */
static void service_links(int fd)
{
    for (size_t i = 0; i < MAX_LINKS; i++) {
        Link *link = &links[i];
        if (!link->active)
            continue;
        if (link->ltk_pending) {
            link->ltk_pending = false;
            uint8_t parameters[2], reply[3];
            size_t length = sizeof(reply);
            put_le16(parameters, link->handle);
            send_hci_command(fd, OGF_LE_CTL, OCF_LE_LTK_NEG_REPLY,
                             parameters, sizeof(parameters), reply, &length);
            printf("link 0x%04x (%s): encryption request answered with LTK negative reply\n",
                   link->handle, identity_name(link->adv_handle));
        }
        if (request_le_2m && !link->phy_requested) {
            link->phy_requested = true;
            uint8_t parameters[7] = {0, 0, 0x00, 0x02, 0x02, 0x00, 0x00};
            size_t length = 0;
            put_le16(parameters, link->handle);
            if (send_hci_command_full(fd, OGF_LE_CTL, OCF_LE_SET_PHY_DIAG,
                                      parameters, sizeof(parameters), NULL,
                                      &length, true))
                printf("link 0x%04x (%s): LE 2M PHY requested\n",
                       link->handle, identity_name(link->adv_handle));
        }
    }
}

static void run_event_loop(int fd, int timeout_seconds)
{
    time_t deadline = timeout_seconds > 0 ? time(NULL) + timeout_seconds : 0;
    while (!interrupted && (!deadline || time(NULL) < deadline)) {
        struct pollfd fds[2] = {
            {.fd = fd, .events = POLLIN},
            {.fd = STDIN_FILENO, .events = POLLIN},
        };
        int ready = poll(fds, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            return;
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char line[64];
            ssize_t n = read(STDIN_FILENO, line, sizeof(line));
            if (n <= 0 || memchr(line, '\n', (size_t)n))
                return;
        }
        if (fds[0].revents & POLLIN) {
            uint8_t packet[HCI_MAX_FRAME_SIZE + 1];
            ssize_t length = read(fd, packet, sizeof(packet));
            if (length > 0)
                handle_async_packet(fd, packet, (size_t)length);
        }
        service_links(fd);
    }
}

static void print_summary(void)
{
    printf("\nSummary\n");
    printf("  Peak simultaneous LE links: %u\n", peak_links);
    printf("  Concurrent links on both LEFT and RIGHT identities: %s\n",
           distinct_identities_concurrent ? "observed" : "not observed");
    for (size_t i = 0; i < MAX_LINKS; i++) {
        if (!links[i].active)
            continue;
        char address[18];
        ba2str(&links[i].peer, address);
        printf("  Active at exit: link 0x%04x %s peer=%s tx=%s rx=%s\n",
               links[i].handle, identity_name(links[i].adv_handle), address,
               phy_name(links[i].tx_phy), phy_name(links[i].rx_phy));
    }
    printf("  This is HCI-level evidence from the local controller. Record what the\n"
           "  independent central reports before treating it as externally observed.\n");
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--le-2m] [--timeout SECONDS] <hciN|adapter-address>\n",
            program);
}

int main(int argc, char **argv)
{
    const char *adapter = NULL;
    int timeout_seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--le-2m")) {
            request_le_2m = true;
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_seconds = atoi(argv[++i]);
        } else if (argv[i][0] != '-' && !adapter) {
            adapter = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!adapter) {
        usage(argv[0]);
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
        fprintf(stderr, "Power the adapter off in BlueZ (or stop bluetoothd) before retrying. "
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
    if (!controller_reset(fd) || !set_event_masks(fd) || !report_capabilities(fd) ||
        !read_num_adv_sets(fd, &num_sets))
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
               handle, identity_name(handle), product_id, address_text);
        if (!set_random_address(fd, handle, &addresses[handle]) ||
            !set_adv_parameters(fd, handle) ||
            !set_adv_data(fd, handle, product_id))
            goto cleanup;
    }
    if (!enable_advertising(fd, true))
        goto cleanup;
    advertising_enabled = true;
    printf("Both connectable legacy advertising sets were accepted by the controller.\n");
    printf("Connect to each address from an independent central (for example nRF Connect),\n"
           "keep both links open, then press Enter to finish.\n");
    run_event_loop(fd, timeout_seconds);
    print_summary();
    result = 0;

cleanup:
    if (advertising_enabled && !enable_advertising(fd, false))
        fprintf(stderr, "Warning: disabling the advertising sets failed.\n");
    if (!command_status_only(fd, OCF_LE_CLEAR_ADV_SETS, NULL, 0))
        fprintf(stderr, "Warning: clearing the volatile advertising sets failed.\n");
    /* Reset drops any remaining links and leaves no volatile state behind. */
    if (!controller_reset(fd)) {
        fprintf(stderr, "Warning: final controller reset failed.\n");
        result = 1;
    }
    close(fd);
    printf("Exclusive controller ownership released. Power the adapter back on in BlueZ.\n");
    return result;
}
