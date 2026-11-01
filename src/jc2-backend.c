#define _GNU_SOURCE
/*
 * Joy-Con 2 BLE peripheral backend over an exclusive HCI user channel.
 *
 * BlueZ cannot provide what a Switch 2 expects from a Joy-Con 2: fixed GATT
 * handles (BlueZ places its own GAP/GATT services first), a 5 ms connection
 * interval (Linux rejects connection parameter requests below 7.5 ms in
 * hci_check_conn_params()) and an LTK derived in-band by Nintendo's pairing
 * (BlueZ answers LTK requests only from SMP bonds). This backend therefore
 * owns the controller while it runs, implements L2CAP/ATT itself and leaves
 * the Joy-Con protocol to src/jc2.c. The adapter is given back to the kernel
 * and BlueZ on exit; nothing persistent is written to the controller.
 */
#include "jc2.h"
#include "../poc/control.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <endian.h>
#include <errno.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define REPORT_INTERVAL_MS 15
#define TX_QUEUE_LIMIT 3
#define KEY_DIR "/var/lib/pcble2gamepad"

typedef struct {
    int fd, device_id;
    Jc2Side side;
    Jc2Session session;
    uint8_t local[6];              /* least significant octet first */
    gboolean verbose, reconnect;
    /* Link */
    gboolean connected;
    uint16_t handle;
    uint8_t peer[6], peer_type;
    unsigned tx_packets, rx_packets;
    gboolean encrypted;
    /* Flow control */
    uint16_t acl_mtu;
    int acl_credits;
    GQueue tx;
    int command_credits;
    GQueue commands;
    /* Reassembly */
    uint8_t rx[1100];
    size_t rx_length, rx_expected;
    /* Stored console association */
    gboolean stored;
    uint8_t stored_peer[6], stored_ltk[16];
    char *key_path;
    /* Control */
    ProControl *control;
    ProState state;
    gint64 release_at;
    GMainLoop *loop;
    gboolean mtu_warned;
} Backend;

static void logf_event(Backend *b, const char *event, const char *format, ...) G_GNUC_PRINTF(3, 4);
static void logf_event(Backend *b, const char *event, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_autofree char *detail = g_strdup_vprintf(format, args);
    va_end(args);
    g_print("%s %s\n", event, detail);
    if (b->control)
        pro_control_log(b->control, event, detail);
}

static char *address_text(const uint8_t lsb_first[6])
{
    return g_strdup_printf("%02X:%02X:%02X:%02X:%02X:%02X", lsb_first[5], lsb_first[4],
                           lsb_first[3], lsb_first[2], lsb_first[1], lsb_first[0]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ---------- HCI commands ---------- */

static gboolean write_all(int fd, const uint8_t *data, size_t length)
{
    ssize_t n = write(fd, data, length);
    return n == (ssize_t)length;
}

static GBytes *command_packet(uint16_t ogf, uint16_t ocf, const void *params, uint8_t length)
{
    uint8_t packet[4 + 255];
    uint16_t opcode = cmd_opcode_pack(ogf, ocf);
    packet[0] = HCI_COMMAND_PKT;
    put16(packet + 1, opcode);
    packet[3] = length;
    if (length)
        memcpy(packet + 4, params, length);
    return g_bytes_new(packet, 4 + length);
}

/* Blocking command used before the main loop starts. */
static gboolean sync_command(Backend *b, uint16_t ogf, uint16_t ocf, const void *params,
                             uint8_t length, uint8_t *reply, size_t *reply_length)
{
    g_autoptr(GBytes) packet = command_packet(ogf, ocf, params, length);
    uint16_t opcode = cmd_opcode_pack(ogf, ocf);
    gsize size;
    const uint8_t *data = g_bytes_get_data(packet, &size);
    if (!write_all(b->fd, data, size)) {
        g_printerr("HCI command 0x%04x write failed: %s\n", opcode, g_strerror(errno));
        return FALSE;
    }
    for (;;) {
        struct pollfd p = {.fd = b->fd, .events = POLLIN};
        if (poll(&p, 1, 3000) <= 0) {
            g_printerr("HCI command 0x%04x timed out\n", opcode);
            return FALSE;
        }
        uint8_t event[HCI_MAX_FRAME_SIZE];
        ssize_t n = read(b->fd, event, sizeof(event));
        if (n < 3 || event[0] != HCI_EVENT_PKT)
            continue;
        if (event[1] == EVT_CMD_STATUS && n >= 7 && get16(event + 5) == opcode) {
            if (event[3] == 0 && reply_length) {
                *reply_length = 0;
                return TRUE;
            }
            g_printerr("HCI command 0x%04x failed with status 0x%02x\n", opcode, event[3]);
            return FALSE;
        }
        if (event[1] != EVT_CMD_COMPLETE || n < 7 || get16(event + 4) != opcode)
            continue;
        size_t available = (size_t)n - 6;
        if (reply && reply_length) {
            if (available > *reply_length)
                available = *reply_length;
            memcpy(reply, event + 6, available);
            *reply_length = available;
        }
        if (event[6] != 0) {
            g_printerr("HCI command 0x%04x returned status 0x%02x\n", opcode, event[6]);
            return FALSE;
        }
        return TRUE;
    }
}

static gboolean sync_simple(Backend *b, uint16_t ogf, uint16_t ocf, const void *params, uint8_t length)
{
    uint8_t status[4];
    size_t n = sizeof(status);
    return sync_command(b, ogf, ocf, params, length, status, &n);
}

static void flush_commands(Backend *b)
{
    while (b->command_credits > 0 && !g_queue_is_empty(&b->commands)) {
        g_autoptr(GBytes) packet = g_queue_pop_head(&b->commands);
        gsize size;
        const uint8_t *data = g_bytes_get_data(packet, &size);
        if (!write_all(b->fd, data, size))
            logf_event(b, "hci_error", "command write failed: %s", g_strerror(errno));
        b->command_credits--;
    }
}

static void async_command(Backend *b, uint16_t ogf, uint16_t ocf, const void *params, uint8_t length)
{
    g_queue_push_tail(&b->commands, command_packet(ogf, ocf, params, length));
    flush_commands(b);
}

/* ---------- ACL / L2CAP ---------- */

static void flush_acl(Backend *b)
{
    while (b->acl_credits > 0 && !g_queue_is_empty(&b->tx)) {
        g_autoptr(GBytes) packet = g_queue_pop_head(&b->tx);
        gsize size;
        const uint8_t *data = g_bytes_get_data(packet, &size);
        if (!write_all(b->fd, data, size)) {
            logf_event(b, "hci_error", "ACL write failed: %s", g_strerror(errno));
            continue;
        }
        b->acl_credits--;
        b->tx_packets++;
    }
}

static void l2cap_send(Backend *b, uint16_t cid, const uint8_t *data, size_t length)
{
    if (!b->connected)
        return;
    uint8_t frame[4 + JC2_ATT_MAX];
    if (length > JC2_ATT_MAX)
        return;
    put16(frame, (uint16_t)length);
    put16(frame + 2, cid);
    memcpy(frame + 4, data, length);
    size_t total = 4 + length, offset = 0;
    while (offset < total) {
        size_t chunk = MIN((size_t)b->acl_mtu, total - offset);
        uint8_t packet[5 + 1024];
        uint16_t flags = offset == 0 ? 0x0000 : 0x1000; /* first / continuation */
        packet[0] = HCI_ACLDATA_PKT;
        put16(packet + 1, (b->handle & 0x0fff) | flags);
        put16(packet + 3, (uint16_t)chunk);
        memcpy(packet + 5, frame + offset, chunk);
        g_queue_push_tail(&b->tx, g_bytes_new(packet, 5 + chunk));
        offset += chunk;
    }
    flush_acl(b);
}

/* ---------- Console association storage ---------- */

static void load_key(Backend *b)
{
    g_autofree char *data = NULL;
    gsize length = 0;
    if (!g_file_get_contents(b->key_path, &data, &length, NULL) || length != 4 + 6 + 16 ||
        memcmp(data, "JC2K", 4))
        return;
    memcpy(b->stored_peer, data + 4, 6);
    memcpy(b->stored_ltk, data + 10, 16);
    b->stored = TRUE;
    g_autofree char *peer = address_text(b->stored_peer);
    logf_event(b, "pairing_loaded", "console=%s", peer);
}

static void save_key(Backend *b)
{
    uint8_t record[4 + 6 + 16];
    memcpy(record, "JC2K", 4);
    memcpy(record + 4, b->peer, 6);
    memcpy(record + 10, b->session.ltk, 16);
    g_autoptr(GError) error = NULL;
    if (g_mkdir_with_parents(KEY_DIR, 0700) < 0 ||
        !g_file_set_contents_full(b->key_path, (const char *)record, sizeof(record),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
        logf_event(b, "pairing_save_failed", "%s", error ? error->message : g_strerror(errno));
        return;
    }
    memcpy(b->stored_peer, b->peer, 6);
    memcpy(b->stored_ltk, b->session.ltk, 16);
    b->stored = TRUE;
    logf_event(b, "pairing_saved", "path=%s", b->key_path);
}

/* ---------- Advertising ---------- */

static void start_advertising(Backend *b, gboolean synchronous)
{
    uint8_t params[15] = {0};
    put16(params, 0x0030);      /* 30 ms */
    put16(params + 2, 0x0030);
    params[4] = 0x00;           /* ADV_IND */
    params[5] = 0x00;           /* public address */
    params[13] = 0x07;          /* all channels */
    uint8_t data[32];
    data[0] = 31;
    gboolean host = b->reconnect && b->stored;
    jc2_advertising_data(b->side, host ? b->stored_peer : NULL, data + 1);
    uint8_t enable = 1;
    if (synchronous) {
        sync_simple(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISING_PARAMETERS, params, sizeof(params));
        sync_simple(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISING_DATA, data, sizeof(data));
        sync_simple(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, &enable, 1);
    } else {
        async_command(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISING_PARAMETERS, params, sizeof(params));
        async_command(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISING_DATA, data, sizeof(data));
        async_command(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, &enable, 1);
    }
    g_autofree char *hex = g_strdup("");
    for (int i = 0; i < 31; i++) {
        char *next = g_strdup_printf("%s%02x", hex, data[1 + i]);
        g_free(hex);
        hex = next;
    }
    logf_event(b, "advertising", "side=%s mode=%s data=%s",
               b->side == JC2_LEFT ? "left" : "right", host ? "reconnect" : "pairing", hex);
}

/* ---------- Link events ---------- */

static void update_control(Backend *b)
{
    g_autofree char *peer = b->connected ? address_text(b->peer) : g_strdup("");
    pro_control_update(b->control, peer, b->session.streaming, b->tx_packets, b->rx_packets);
}

static void connection_complete(Backend *b, uint8_t status, uint16_t handle, uint8_t role,
                                uint8_t peer_type, const uint8_t *peer, uint16_t interval)
{
    if (status) {
        logf_event(b, "connection_failed", "status=0x%02x", status);
        return;
    }
    if (role != 0x01 || b->connected) {
        uint8_t params[3];
        put16(params, handle);
        params[2] = 0x13;
        async_command(b, OGF_LINK_CTL, OCF_DISCONNECT, params, sizeof(params));
        return;
    }
    b->connected = TRUE;
    b->handle = handle;
    b->peer_type = peer_type;
    memcpy(b->peer, peer, 6);
    b->encrypted = FALSE;
    b->rx_length = b->rx_expected = 0;
    b->mtu_warned = FALSE;
    Jc2Side side = b->side;
    jc2_session_init(&b->session, side, b->local);
    if (b->stored && !memcmp(peer, b->stored_peer, 6)) {
        memcpy(b->session.ltk, b->stored_ltk, 16);
        b->session.have_ltk = TRUE;
    }
    g_autofree char *text = address_text(peer);
    logf_event(b, "connected", "console=%s type=%u handle=0x%04x interval=%.2fms known=%s",
               text, peer_type, handle, interval * 1.25,
               b->session.have_ltk ? "yes" : "no");
    update_control(b);
}

static void disconnected(Backend *b, uint16_t handle, uint8_t reason)
{
    if (!b->connected || handle != b->handle)
        return;
    b->connected = FALSE;
    b->session.streaming = FALSE;
    g_queue_clear_full(&b->tx, (GDestroyNotify)g_bytes_unref);
    logf_event(b, "disconnected", "reason=0x%02x", reason);
    update_control(b);
    start_advertising(b, FALSE);
}

static void ltk_request(Backend *b, uint16_t handle)
{
    uint8_t params[18];
    put16(params, handle);
    if (b->connected && handle == b->handle && b->session.have_ltk) {
        memcpy(params + 2, b->session.ltk, 16);
        async_command(b, OGF_LE_CTL, OCF_LE_LTK_REPLY, params, sizeof(params));
        logf_event(b, "encryption_requested", "answered with the Nintendo pairing key");
    } else {
        async_command(b, OGF_LE_CTL, OCF_LE_LTK_NEG_REPLY, params, 2);
        logf_event(b, "encryption_requested", "no key for this console; negative reply");
    }
}

static void remote_parameter_request(Backend *b, const uint8_t *p)
{
    /* Accept what the console asks for, including the 5 ms interval Linux
     * itself would reject. The controller may still refuse it. */
    uint8_t params[14];
    memcpy(params, p, 10);
    put16(params + 10, 0);
    put16(params + 12, 0);
    async_command(b, OGF_LE_CTL, 0x0020, params, sizeof(params));
    logf_event(b, "connection_parameters_requested",
               "interval=%.2f-%.2fms latency=%u timeout=%ums; accepted",
               get16(p + 2) * 1.25, get16(p + 4) * 1.25, get16(p + 6), get16(p + 8) * 10u);
}

static void handle_le_meta(Backend *b, const uint8_t *p, size_t length)
{
    if (!length)
        return;
    uint8_t sub = p[0];
    p++;
    length--;
    switch (sub) {
    case 0x01: /* Connection Complete */
        if (length >= 18)
            connection_complete(b, p[0], get16(p + 1), p[3], p[4], p + 5, get16(p + 11));
        break;
    case 0x0a: /* Enhanced Connection Complete */
        if (length >= 30)
            connection_complete(b, p[0], get16(p + 1), p[3], p[4], p + 5, get16(p + 23));
        break;
    case 0x03:
        if (length >= 9)
            logf_event(b, "connection_updated", "status=0x%02x interval=%.2fms latency=%u timeout=%ums",
                       p[0], get16(p + 3) * 1.25, get16(p + 5), get16(p + 7) * 10u);
        break;
    case 0x05:
        if (length >= 12)
            ltk_request(b, get16(p));
        break;
    case 0x06:
        if (length >= 10)
            remote_parameter_request(b, p);
        break;
    case 0x07:
        if (length >= 10)
            logf_event(b, "data_length", "tx=%u rx=%u", get16(p + 2), get16(p + 6));
        break;
    case 0x0c:
        if (length >= 5)
            logf_event(b, "phy_updated", "status=0x%02x tx=%u rx=%u (1=1M 2=2M 3=Coded)",
                       p[0], p[3], p[4]);
        break;
    default:
        break;
    }
}

/* ---------- ATT / L2CAP dispatch ---------- */

static void log_att(Backend *b, const char *direction, const uint8_t *pdu, size_t length)
{
    if (!b->verbose)
        return;
    /* Pairing key material (command 0x15) is never logged. */
    gboolean secret = length >= 3 + 17 + 1 && pdu[0] == 0x52 && pdu[3 + 17] == 0x15;
    secret = secret || (length >= 4 && pdu[0] == 0x1b && pdu[3] == 0x15);
    GString *s = g_string_new(NULL);
    size_t shown = secret ? MIN(length, (size_t)(pdu[0] == 0x52 ? 3 + 17 + 8 : 3 + 8)) : length;
    for (size_t i = 0; i < shown; i++)
        g_string_append_printf(s, "%02x", pdu[i]);
    if (shown < length)
        g_string_append(s, " (pairing payload hidden)");
    g_print("att_%s %s\n", direction, s->str);
    g_string_free(s, TRUE);
}

static void handle_att(Backend *b, const uint8_t *pdu, size_t length)
{
    uint8_t rsp[JC2_ATT_MAX];
    log_att(b, "rx", pdu, length);
    if (length >= 4 && pdu[0] == 0x52 && get16(pdu + 1) == 0x0016 && length >= 3 + 17 + 8) {
        const uint8_t *f = pdu + 3 + 17;
        logf_event(b, "nintendo_command", "cmd=0x%02x sub=0x%02x", f[0], f[3]);
    }
    size_t n = jc2_att_request(&b->session, pdu, length, rsp, sizeof(rsp));
    if (n) {
        log_att(b, "tx", rsp, n);
        l2cap_send(b, 0x0004, rsp, n);
    }
    if (b->session.notify_length) {
        log_att(b, "tx", b->session.notify, b->session.notify_length);
        l2cap_send(b, 0x0004, b->session.notify, b->session.notify_length);
        b->session.notify_length = 0;
    }
    switch (b->session.last_event) {
    case JC2_EVENT_KEY_DERIVED:
        logf_event(b, "pairing", "key exchange completed");
        break;
    case JC2_EVENT_PAIRED:
        logf_event(b, "pairing", "console finalized Nintendo pairing");
        save_key(b);
        break;
    case JC2_EVENT_STREAMING:
        logf_event(b, "input", "console requested input reports");
        break;
    default:
        break;
    }
    b->session.last_event = JC2_EVENT_NONE;
    if (pdu[0] == 0x02)
        logf_event(b, "mtu", "client=%u effective=%u", get16(pdu + 1), b->session.mtu);
    update_control(b);
}

static void handle_l2cap(Backend *b, const uint8_t *frame, size_t length)
{
    uint16_t cid = get16(frame + 2);
    const uint8_t *data = frame + 4;
    size_t n = length - 4;
    if (cid == 0x0004) {
        handle_att(b, data, n);
    } else if (cid == 0x0005 && n >= 4) {
        uint8_t code = data[0];
        if (code != 0x01 && code != 0x13) {
            uint8_t reject[6] = {0x01, data[1], 0x02, 0x00, 0x00, 0x00};
            l2cap_send(b, 0x0005, reject, sizeof(reject));
        }
    } else if (cid == 0x0006 && n >= 1 && data[0] == 0x01) {
        static const uint8_t failed[2] = {0x05, 0x05}; /* Pairing Not Supported */
        l2cap_send(b, 0x0006, failed, sizeof(failed));
        logf_event(b, "smp", "standard pairing request rejected; Nintendo pairing is in-band");
    }
}

static void handle_acl(Backend *b, const uint8_t *packet, size_t length)
{
    if (length < 5 || !b->connected)
        return;
    uint16_t header = get16(packet + 1);
    uint16_t handle = header & 0x0fff;
    uint8_t boundary = (header >> 12) & 0x3;
    size_t data_length = get16(packet + 3);
    if (handle != b->handle || data_length + 5 > length)
        return;
    const uint8_t *data = packet + 5;
    b->rx_packets++;
    if (boundary != 0x01) {
        if (data_length < 4)
            return;
        b->rx_expected = (size_t)get16(data) + 4;
        b->rx_length = 0;
        if (b->rx_expected > sizeof(b->rx)) {
            b->rx_expected = 0;
            return;
        }
    } else if (!b->rx_expected) {
        return;
    }
    if (b->rx_length + data_length > b->rx_expected) {
        b->rx_expected = 0;
        return;
    }
    memcpy(b->rx + b->rx_length, data, data_length);
    b->rx_length += data_length;
    if (b->rx_length == b->rx_expected) {
        b->rx_expected = 0;
        handle_l2cap(b, b->rx, b->rx_length);
    }
}

static void handle_event(Backend *b, const uint8_t *packet, size_t length)
{
    if (length < 3 || (size_t)packet[2] + 3 > length)
        return;
    const uint8_t *p = packet + 3;
    size_t n = packet[2];
    switch (packet[1]) {
    case EVT_CMD_COMPLETE:
        if (n >= 4) {
            b->command_credits = p[0];
            if (p[3] != 0)
                logf_event(b, "hci_command_failed", "opcode=0x%04x status=0x%02x", get16(p + 1), p[3]);
        }
        flush_commands(b);
        break;
    case EVT_CMD_STATUS:
        if (n >= 4) {
            b->command_credits = p[1];
            if (p[0] != 0)
                logf_event(b, "hci_command_failed", "opcode=0x%04x status=0x%02x", get16(p + 2), p[0]);
        }
        flush_commands(b);
        break;
    case EVT_DISCONN_COMPLETE:
        if (n >= 4)
            disconnected(b, get16(p + 1), p[3]);
        break;
    case EVT_NUM_COMP_PKTS:
        for (size_t i = 0; n >= 1 && i < p[0] && 1 + 4 * i + 4 <= n; i++)
            b->acl_credits += get16(p + 1 + 4 * i + 2);
        flush_acl(b);
        break;
    case EVT_ENCRYPT_CHANGE:
        if (n >= 4) {
            b->encrypted = p[0] == 0 && p[3];
            logf_event(b, "encryption", "status=0x%02x enabled=%u", p[0], p[3]);
        }
        break;
    case 0x30: /* Encryption Key Refresh Complete */
        if (n >= 3)
            logf_event(b, "encryption", "key refresh status=0x%02x", p[0]);
        break;
    case EVT_LE_META_EVENT:
        handle_le_meta(b, p, n);
        break;
    default:
        break;
    }
}

static gboolean on_hci(int fd, GIOCondition condition, gpointer data)
{
    Backend *b = data;
    if (condition & (G_IO_ERR | G_IO_HUP)) {
        logf_event(b, "hci_error", "controller channel closed");
        g_main_loop_quit(b->loop);
        return G_SOURCE_REMOVE;
    }
    uint8_t packet[HCI_MAX_FRAME_SIZE];
    ssize_t n = read(fd, packet, sizeof(packet));
    if (n <= 0)
        return G_SOURCE_CONTINUE;
    if (packet[0] == HCI_EVENT_PKT)
        handle_event(b, packet, (size_t)n);
    else if (packet[0] == HCI_ACLDATA_PKT)
        handle_acl(b, packet, (size_t)n);
    return G_SOURCE_CONTINUE;
}

/* ---------- Input reports ---------- */

static const uint16_t pro_neutral[4] = {2159, 1916, 2070, 2013};

static gboolean report_tick(gpointer data)
{
    Backend *b = data;
    gint64 now = g_get_monotonic_time();
    if (b->control && pro_control_expired(b->control)) {
        logf_event(b, "control", "desktop client stopped responding; exiting");
        g_main_loop_quit(b->loop);
        return G_SOURCE_REMOVE;
    }
    if (b->release_at && now >= b->release_at) {
        memset(b->state.buttons, 0, 3);
        memcpy(b->state.sticks, pro_neutral, sizeof(pro_neutral));
        b->release_at = 0;
    }
    if (!b->connected || !b->session.streaming || g_queue_get_length(&b->tx) >= TX_QUEUE_LIMIT)
        return G_SOURCE_CONTINUE;
    if (b->session.mtu < 3 + JC2_REPORT_SIZE) {
        if (!b->mtu_warned)
            logf_event(b, "input", "ATT MTU %u is too small for input reports", b->session.mtu);
        b->mtu_warned = TRUE;
        return G_SOURCE_CONTINUE;
    }
    Jc2Input input;
    jc2_from_pro_frame(b->side, b->state.buttons, b->state.sticks, &input);
    uint8_t pdu[3 + JC2_REPORT_SIZE];
    pdu[0] = 0x1b;
    put16(pdu + 1, JC2_HANDLE_INPUT);
    jc2_build_report(&b->session, &input, pdu + 3);
    l2cap_send(b, 0x0004, pdu, sizeof(pdu));
    return G_SOURCE_CONTINUE;
}

/* ---------- Setup ---------- */

static int open_user_channel(int device_id)
{
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0)
        return -1;
    struct sockaddr_hci address = {
        .hci_family = AF_BLUETOOTH, .hci_dev = (unsigned short)device_id,
        .hci_channel = HCI_CHANNEL_USER,
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
        return fd;
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

static gboolean controller_setup(Backend *b)
{
    uint8_t reply[16];
    size_t n;
    if (!sync_simple(b, OGF_HOST_CTL, OCF_RESET, NULL, 0))
        return FALSE;
    n = sizeof(reply);
    if (!sync_command(b, OGF_INFO_PARAM, OCF_READ_BD_ADDR, NULL, 0, reply, &n) || n < 7)
        return FALSE;
    memcpy(b->local, reply + 1, 6);
    n = sizeof(reply);
    if (!sync_command(b, OGF_LE_CTL, OCF_LE_READ_BUFFER_SIZE, NULL, 0, reply, &n) || n < 4)
        return FALSE;
    b->acl_mtu = get16(reply + 1);
    b->acl_credits = reply[3];
    if (!b->acl_mtu || !b->acl_credits) {
        n = sizeof(reply);
        if (!sync_command(b, OGF_INFO_PARAM, OCF_READ_BUFFER_SIZE, NULL, 0, reply, &n) || n < 8)
            return FALSE;
        b->acl_mtu = get16(reply + 1);
        b->acl_credits = get16(reply + 4);
    }
    if (b->acl_mtu > 1024)
        b->acl_mtu = 1024;
    uint64_t mask = htole64(0x00001fffffffffffULL | (1ULL << 61));
    /* Connection complete, update, remote features, LTK request, remote
     * parameter request, data length, enhanced connection complete, PHY. */
    uint64_t le_mask = htole64((1ULL << 0) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) |
                               (1ULL << 5) | (1ULL << 6) | (1ULL << 9) | (1ULL << 11));
    if (!sync_simple(b, OGF_HOST_CTL, OCF_SET_EVENT_MASK, &mask, 8) ||
        !sync_simple(b, OGF_LE_CTL, OCF_LE_SET_EVENT_MASK, &le_mask, 8))
        return FALSE;
    /* Prefer 1M or 2M; the console picks. Optional features may fail. */
    uint8_t phy[3] = {0x00, 0x03, 0x03};
    sync_simple(b, OGF_LE_CTL, 0x0031, phy, sizeof(phy));
    uint8_t data_length[4];
    put16(data_length, 251);
    put16(data_length + 2, 2120);
    sync_simple(b, OGF_LE_CTL, 0x0024, data_length, sizeof(data_length));
    b->command_credits = 1;
    g_autofree char *local = address_text(b->local);
    logf_event(b, "controller_ready", "address=%s acl_mtu=%u acl_buffers=%d", local,
               b->acl_mtu, b->acl_credits);
    return TRUE;
}

static void controller_release(Backend *b)
{
    if (b->fd < 0)
        return;
    uint8_t off = 0;
    if (b->connected) {
        uint8_t params[3];
        put16(params, b->handle);
        params[2] = 0x13;
        g_autoptr(GBytes) packet = command_packet(OGF_LINK_CTL, OCF_DISCONNECT, params, 3);
        gsize size;
        const uint8_t *d = g_bytes_get_data(packet, &size);
        write_all(b->fd, d, size);
        g_usleep(100000);
    }
    sync_simple(b, OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, &off, 1);
    sync_simple(b, OGF_HOST_CTL, OCF_RESET, NULL, 0);
    close(b->fd);
    b->fd = -1;
}

static gboolean quit(gpointer data)
{
    g_main_loop_quit(((Backend *)data)->loop);
    return G_SOURCE_CONTINUE;
}

static void usage(void)
{
    g_printerr("Usage: pcble2gamepad-joycon2-backend <hciN|address> --side left|right "
               "[--desktop UID] [--reconnect] [--verbose]\n");
}

int main(int argc, char **argv)
{
    const char *adapter = NULL, *side = NULL;
    long owner = -1;
    Backend b = {.fd = -1};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--side") && i + 1 < argc) side = argv[++i];
        else if (!strcmp(argv[i], "--desktop") && i + 1 < argc) owner = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--reconnect")) b.reconnect = TRUE;
        else if (!strcmp(argv[i], "--verbose")) b.verbose = TRUE;
        else if (argv[i][0] != '-' && !adapter) adapter = argv[i];
        else { usage(); return 2; }
    }
    if (!adapter || !side || (strcmp(side, "left") && strcmp(side, "right"))) {
        usage();
        return 2;
    }
    b.side = !strcmp(side, "left") ? JC2_LEFT : JC2_RIGHT;
    b.device_id = hci_devid(adapter);
    if (b.device_id < 0) {
        g_printerr("Cannot resolve Bluetooth adapter %s\n", adapter);
        return 1;
    }
    b.fd = open_user_channel(b.device_id);
    if (b.fd < 0 && (errno == EBUSY || errno == EALREADY)) {
        /* Power the adapter down so the kernel releases it to us. BlueZ
         * powers it up again when the user channel closes. */
        int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
        if (ctl >= 0) {
            ioctl(ctl, HCIDEVDOWN, b.device_id);
            close(ctl);
        }
        b.fd = open_user_channel(b.device_id);
    }
    if (b.fd < 0) {
        g_printerr("Cannot take exclusive control of %s: %s\n", adapter, g_strerror(errno));
        return 1;
    }
    g_queue_init(&b.tx);
    g_queue_init(&b.commands);
    b.loop = g_main_loop_new(NULL, FALSE);
    b.state.type = b.side == JC2_LEFT ? CONTROLLER_JOYCON_L : CONTROLLER_JOYCON_R;
    memcpy(b.state.sticks, pro_neutral, sizeof(pro_neutral));
    if (owner >= 0) {
        g_autoptr(GError) error = NULL;
        b.control = pro_control_new(&b.state, &b.release_at, &b.verbose, (uid_t)owner, FALSE,
                                    "pro.sock", b.loop, &error);
        if (!b.control) {
            g_printerr("Control socket failed: %s\n", error->message);
            controller_release(&b);
            return 1;
        }
    }
    int result = 1;
    if (!controller_setup(&b))
        goto done;
    g_autofree char *local = g_strdup_printf("%02x%02x%02x%02x%02x%02x", b.local[5], b.local[4],
                                             b.local[3], b.local[2], b.local[1], b.local[0]);
    b.key_path = g_strdup_printf(KEY_DIR "/joycon2-%s-%s.key", local, side);
    load_key(&b);
    if (b.reconnect && !b.stored) {
        logf_event(&b, "reconnect", "no saved console for this side; using pairing mode");
        b.reconnect = FALSE;
    }
    jc2_session_init(&b.session, b.side, b.local);
    start_advertising(&b, TRUE);
    logf_event(&b, "ready", "Open Controllers > Change Grip/Order on the Switch 2");
    update_control(&b);
    g_unix_fd_add(b.fd, G_IO_IN | G_IO_ERR | G_IO_HUP, on_hci, &b);
    g_timeout_add(REPORT_INTERVAL_MS, report_tick, &b);
    g_unix_signal_add(SIGINT, quit, &b);
    g_unix_signal_add(SIGTERM, quit, &b);
    g_main_loop_run(b.loop);
    result = 0;
done:
    controller_release(&b);
    logf_event(&b, "stopped", "adapter returned to the system");
    pro_control_free(b.control);
    g_queue_clear_full(&b.tx, (GDestroyNotify)g_bytes_unref);
    g_queue_clear_full(&b.commands, (GDestroyNotify)g_bytes_unref);
    g_main_loop_unref(b.loop);
    g_free(b.key_path);
    return result;
}
