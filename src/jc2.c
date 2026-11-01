#include "jc2.h"
#include "jc2-data.h"
#include "pairing.h"
#include <string.h>

/*
 * GATT layout and command behaviour follow ndeadly's switch2_controller_research
 * (bluetooth_interface.md) and the XD2Joy firmware, which emulates Joy-Con 2
 * controllers on a real Switch 2. Handles are fixed and match a real
 * controller; see docs/protocol.md.
 */

enum {
    PROP_READ = 0x02, PROP_WRITE_NR = 0x04, PROP_WRITE = 0x08, PROP_NOTIFY = 0x10,
};

typedef enum {
    KIND_PRIMARY, KIND_CHAR, KIND_VALUE, KIND_CCCD, KIND_VDESC,
} Kind;

typedef enum {
    ROLE_NONE, ROLE_CFG1, ROLE_CFG2, ROLE_CFG3, ROLE_EMPTY, ROLE_STUB,
    ROLE_COMMAND, ROLE_NOTIFY, ROLE_NAME, ROLE_APPEARANCE,
} Role;

struct Jc2Attr {
    uint16_t handle;
    Kind kind;
    const char *uuid;   /* 128-bit string, or NULL for 16-bit */
    uint16_t uuid16;
    uint8_t props;      /* KIND_CHAR: declared properties */
    uint16_t end;       /* KIND_PRIMARY: group end; KIND_CHAR: value handle */
    Role role;
    uint8_t cccd;       /* KIND_CCCD: index into session cccd[] */
};

#define S128(h, u, e) {h, KIND_PRIMARY, u, 0, 0, e, ROLE_NONE, 0}
#define S16(h, u, e) {h, KIND_PRIMARY, NULL, u, 0, e, ROLE_NONE, 0}
#define C128(h, p, u, role) {h, KIND_CHAR, u, 0, p, (h) + 1, ROLE_NONE, 0}, \
                            {(h) + 1, KIND_VALUE, u, 0, p, 0, role, 0}
#define C16(h, p, u, role) {h, KIND_CHAR, NULL, u, p, (h) + 1, ROLE_NONE, 0}, \
                           {(h) + 1, KIND_VALUE, NULL, u, p, 0, role, 0}
#define CCCD(h, i) {h, KIND_CCCD, NULL, 0x2902, 0, 0, ROLE_NONE, i}
#define VDESC(h, u) {h, KIND_VDESC, u, 0, 0, 0, ROLE_EMPTY, 0}

#define UUID_SVC_CFG   "00c5af5d-1964-4e30-8f51-1956f96bd280"
#define UUID_CFG1      "00c5af5d-1964-4e30-8f51-1956f96bd281"
#define UUID_CFG2      "00c5af5d-1964-4e30-8f51-1956f96bd282"
#define UUID_CFG3      "00c5af5d-1964-4e30-8f51-1956f96bd283"
#define UUID_SVC_MAIN  "ab7de9be-89fe-49ad-828f-118f09df7fd0"
#define UUID_N000A     "ab7de9be-89fe-49ad-828f-118f09df7fd2"
#define UUID_W0014     "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"
#define UUID_W0018     "4147423d-fdae-4df7-a4f7-d23e5df59f8d"
#define UUID_CMDRSP    "c765a961-d9d8-4d36-a20a-5315b111836a"
#define UUID_N0022     "d3bd69d2-841c-4241-ab15-f86f406d2a80"
#define UUID_N0026     "ab7de9be-89fe-49ad-828f-118f09df7fde"
#define UUID_W002A     "ab7de9be-89fe-49ad-828f-118f09df7fdf"
#define UUID_DESC1     "679d5510-5a24-4dee-9557-95df80486ecb"
#define UUID_DESC2     "b746df8c-f358-495b-9cd2-e3bbeda4f979"

#define DATABASE(INPUT, W0012, W0016, N001E) { \
    S128(0x0001, UUID_SVC_CFG, 0x0007), \
    C128(0x0002, PROP_READ, UUID_CFG1, ROLE_CFG1), \
    C128(0x0004, PROP_WRITE, UUID_CFG2, ROLE_CFG2), \
    C128(0x0006, PROP_READ, UUID_CFG3, ROLE_CFG3), \
    S128(0x0008, UUID_SVC_MAIN, 0x002a), \
    C128(0x0009, PROP_READ | PROP_NOTIFY, UUID_N000A, ROLE_EMPTY), \
    CCCD(0x000b, 0), VDESC(0x000c, UUID_DESC1), \
    C128(0x000d, PROP_READ | PROP_NOTIFY, INPUT, ROLE_EMPTY), \
    CCCD(0x000f, 1), VDESC(0x0010, UUID_DESC1), \
    C128(0x0011, PROP_WRITE_NR, W0012, ROLE_STUB), \
    C128(0x0013, PROP_WRITE_NR, UUID_W0014, ROLE_STUB), \
    C128(0x0015, PROP_WRITE_NR, W0016, ROLE_COMMAND), \
    C128(0x0017, PROP_WRITE_NR, UUID_W0018, ROLE_STUB), \
    C128(0x0019, PROP_NOTIFY, UUID_CMDRSP, ROLE_NOTIFY), \
    CCCD(0x001b, 2), VDESC(0x001c, UUID_DESC2), \
    C128(0x001d, PROP_NOTIFY, N001E, ROLE_NOTIFY), \
    CCCD(0x001f, 3), VDESC(0x0020, UUID_DESC2), \
    C128(0x0021, PROP_NOTIFY, UUID_N0022, ROLE_NOTIFY), \
    CCCD(0x0023, 4), VDESC(0x0024, UUID_DESC2), \
    C128(0x0025, PROP_READ | PROP_NOTIFY, UUID_N0026, ROLE_EMPTY), \
    CCCD(0x0027, 5), VDESC(0x0028, UUID_DESC1), \
    C128(0x0029, PROP_WRITE_NR, UUID_W002A, ROLE_STUB), \
    S16(0x002b, 0x1800, 0x002f), \
    C16(0x002c, PROP_READ, 0x2a00, ROLE_NAME), \
    C16(0x002e, PROP_READ, 0x2a01, ROLE_APPEARANCE), \
    S16(0x0030, 0x1801, 0x0030), \
}

static const Jc2Attr left_db[] = DATABASE(
    "cc1bbbb5-7354-4d32-a716-a81cb241a32a",
    "289326cb-a471-485d-a8f4-240c14f18241",
    "ce49a830-dced-48ae-931e-c8cf88aadbea",
    "63a3810f-aec7-474b-9010-3d52403cb996");
static const Jc2Attr right_db[] = DATABASE(
    "d5a9e01e-2ffc-4cca-b20c-8b67142bf442",
    "fa19b0fb-cd1f-46a7-84a1-bbb09e00c149",
    "65a724b3-f1e7-4a61-8078-a342376b27ff",
    "640ca58e-0e88-410c-a7f3-426faf2b690b");

#define DB_SIZE (sizeof(left_db) / sizeof(left_db[0]))
_Static_assert(sizeof(left_db) == sizeof(right_db), "both sides share one layout");

static const Jc2Attr *database(Jc2Side side)
{
    return side == JC2_LEFT ? left_db : right_db;
}

static const Jc2Attr *find(Jc2Side side, uint16_t handle)
{
    const Jc2Attr *db = database(side);
    for (size_t i = 0; i < DB_SIZE; i++)
        if (db[i].handle == handle)
            return &db[i];
    return NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ATT carries UUIDs least significant octet first. */
static size_t uuid_wire(const char *uuid, uint16_t uuid16, uint8_t out[16])
{
    if (!uuid) {
        out[0] = uuid16 & 0xff;
        out[1] = uuid16 >> 8;
        return 2;
    }
    uint8_t big[16];
    size_t n = 0;
    for (const char *p = uuid; *p && n < 16; p++) {
        if (*p == '-')
            continue;
        big[n++] = (uint8_t)(hex_value(p[0]) << 4 | hex_value(p[1]));
        p++;
    }
    for (size_t i = 0; i < 16; i++)
        out[i] = big[15 - i];
    return 16;
}

/* Attribute type, as compared by Read By Type and reported by Find Information. */
static size_t attr_type(const Jc2Attr *a, uint8_t out[16])
{
    switch (a->kind) {
    case KIND_PRIMARY: return uuid_wire(NULL, 0x2800, out);
    case KIND_CHAR: return uuid_wire(NULL, 0x2803, out);
    case KIND_CCCD: return uuid_wire(NULL, 0x2902, out);
    default: return uuid_wire(a->uuid, a->uuid16, out);
    }
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

static const uint8_t cfg1_value[7] = {0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00};
static const uint8_t cfg3_left[8] = {0x35, 0xdc, 0xdf, 0xec, 0xfc, 0x92, 0xee, 0x19};
static const uint8_t cfg3_right[8] = {0x29, 0x31, 0x95, 0x87, 0x06, 0x4c, 0x0a, 0x9f};

/* Returns the value length, or a negative ATT error code. */
static int attr_read(const Jc2Session *s, const Jc2Attr *a, uint8_t *out, size_t size)
{
    uint8_t tmp[24];
    size_t n = 0;
    switch (a->kind) {
    case KIND_PRIMARY:
        n = uuid_wire(a->uuid, a->uuid16, tmp);
        break;
    case KIND_CHAR:
        tmp[0] = a->props;
        put16(tmp + 1, a->end);
        n = 3 + uuid_wire(a->uuid, a->uuid16, tmp + 3);
        break;
    case KIND_CCCD:
        put16(tmp, s->cccd[a->cccd]);
        n = 2;
        break;
    case KIND_VDESC:
        n = 0;
        break;
    case KIND_VALUE:
        if (!(a->props & PROP_READ))
            return -0x02; /* Read Not Permitted */
        switch (a->role) {
        case ROLE_CFG1: memcpy(tmp, cfg1_value, 7); n = 7; break;
        case ROLE_CFG3:
            memcpy(tmp, s->side == JC2_LEFT ? cfg3_left : cfg3_right, 8);
            n = 8;
            break;
        case ROLE_NAME: {
            const char *name = s->side == JC2_LEFT ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)";
            n = strlen(name);
            memcpy(tmp, name, n);
            break;
        }
        case ROLE_APPEARANCE: put16(tmp, 0); n = 2; break;
        default: n = 0; break;
        }
        break;
    }
    if (n > size)
        n = size;
    memcpy(out, tmp, n);
    return (int)n;
}

static size_t error_rsp(uint8_t *rsp, uint8_t opcode, uint16_t handle, uint8_t code)
{
    rsp[0] = 0x01;
    rsp[1] = opcode;
    put16(rsp + 2, handle);
    rsp[4] = code;
    return 5;
}

static bool uuid_equal(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    return alen == blen && !memcmp(a, b, alen);
}

static size_t read_by_group(const Jc2Session *s, const uint8_t *pdu, size_t length,
                            uint8_t *rsp, size_t mtu)
{
    if (length != 7 && length != 21)
        return error_rsp(rsp, pdu[0], 0, 0x04);
    uint16_t start = get16(pdu + 1), end = get16(pdu + 3);
    if (!start || start > end)
        return error_rsp(rsp, pdu[0], start, 0x01);
    uint8_t want[16], primary[2] = {0x00, 0x28};
    size_t want_len = length - 5;
    memcpy(want, pdu + 5, want_len);
    if (!uuid_equal(want, want_len, primary, 2))
        return error_rsp(rsp, pdu[0], start, 0x10); /* Unsupported Group Type */
    const Jc2Attr *db = database(s->side);
    size_t used = 2, entry = 0;
    rsp[0] = 0x11;
    for (size_t i = 0; i < DB_SIZE; i++) {
        const Jc2Attr *a = &db[i];
        if (a->kind != KIND_PRIMARY || a->handle < start || a->handle > end)
            continue;
        uint8_t uuid[16];
        size_t ulen = uuid_wire(a->uuid, a->uuid16, uuid);
        if (entry && entry != 4 + ulen)
            break;
        if (used + 4 + ulen > mtu)
            break;
        entry = 4 + ulen;
        put16(rsp + used, a->handle);
        put16(rsp + used + 2, a->end);
        memcpy(rsp + used + 4, uuid, ulen);
        used += entry;
    }
    if (!entry)
        return error_rsp(rsp, pdu[0], start, 0x0a);
    rsp[1] = (uint8_t)entry;
    return used;
}

static size_t find_by_type_value(const Jc2Session *s, const uint8_t *pdu, size_t length,
                                 uint8_t *rsp, size_t mtu)
{
    if (length < 7)
        return error_rsp(rsp, pdu[0], 0, 0x04);
    uint16_t start = get16(pdu + 1), end = get16(pdu + 3), type = get16(pdu + 5);
    if (!start || start > end)
        return error_rsp(rsp, pdu[0], start, 0x01);
    const Jc2Attr *db = database(s->side);
    size_t used = 1;
    rsp[0] = 0x07;
    for (size_t i = 0; i < DB_SIZE && type == 0x2800; i++) {
        const Jc2Attr *a = &db[i];
        if (a->kind != KIND_PRIMARY || a->handle < start || a->handle > end)
            continue;
        uint8_t uuid[16];
        size_t ulen = uuid_wire(a->uuid, a->uuid16, uuid);
        if (!uuid_equal(uuid, ulen, pdu + 7, length - 7))
            continue;
        if (used + 4 > mtu)
            break;
        put16(rsp + used, a->handle);
        put16(rsp + used + 2, a->end);
        used += 4;
    }
    if (used == 1)
        return error_rsp(rsp, pdu[0], start, 0x0a);
    return used;
}

static size_t read_by_type(const Jc2Session *s, const uint8_t *pdu, size_t length,
                           uint8_t *rsp, size_t mtu)
{
    if (length != 7 && length != 21)
        return error_rsp(rsp, pdu[0], 0, 0x04);
    uint16_t start = get16(pdu + 1), end = get16(pdu + 3);
    if (!start || start > end)
        return error_rsp(rsp, pdu[0], start, 0x01);
    const Jc2Attr *db = database(s->side);
    size_t used = 2, entry = 0;
    rsp[0] = 0x09;
    for (size_t i = 0; i < DB_SIZE; i++) {
        const Jc2Attr *a = &db[i];
        if (a->handle < start || a->handle > end)
            continue;
        uint8_t type[16];
        size_t tlen = attr_type(a, type);
        if (!uuid_equal(type, tlen, pdu + 5, length - 5))
            continue;
        uint8_t value[24];
        int n = attr_read(s, a, value, sizeof(value));
        if (n < 0) {
            if (!entry)
                return error_rsp(rsp, pdu[0], a->handle, (uint8_t)-n);
            break;
        }
        size_t this_entry = 2 + (size_t)n;
        if (this_entry > 255 || (entry && entry != this_entry))
            break;
        if (used + this_entry > mtu) {
            if (entry)
                break;
            this_entry = mtu - used;
        }
        entry = this_entry;
        put16(rsp + used, a->handle);
        memcpy(rsp + used + 2, value, entry - 2);
        used += entry;
    }
    if (!entry)
        return error_rsp(rsp, pdu[0], start, 0x0a);
    rsp[1] = (uint8_t)entry;
    return used;
}

static size_t find_information(const Jc2Session *s, const uint8_t *pdu, size_t length,
                               uint8_t *rsp, size_t mtu)
{
    if (length != 5)
        return error_rsp(rsp, pdu[0], 0, 0x04);
    uint16_t start = get16(pdu + 1), end = get16(pdu + 3);
    if (!start || start > end)
        return error_rsp(rsp, pdu[0], start, 0x01);
    const Jc2Attr *db = database(s->side);
    size_t used = 2, format_len = 0;
    rsp[0] = 0x05;
    for (size_t i = 0; i < DB_SIZE; i++) {
        const Jc2Attr *a = &db[i];
        if (a->handle < start || a->handle > end)
            continue;
        uint8_t type[16];
        size_t tlen = attr_type(a, type);
        if (format_len && format_len != tlen)
            break;
        if (used + 2 + tlen > mtu)
            break;
        format_len = tlen;
        put16(rsp + used, a->handle);
        memcpy(rsp + used + 2, type, tlen);
        used += 2 + tlen;
    }
    if (!format_len)
        return error_rsp(rsp, pdu[0], start, 0x0a);
    rsp[1] = format_len == 2 ? 0x01 : 0x02;
    return used;
}

static void handle_write(Jc2Session *s, const Jc2Attr *a, const uint8_t *value, size_t length)
{
    if (a->kind == KIND_CCCD && length >= 2) {
        s->cccd[a->cccd] = get16(value);
    } else if (a->kind == KIND_VDESC && length && length <= sizeof(s->vendor_descriptor)) {
        memcpy(s->vendor_descriptor, value, length);
    } else if (a->kind == KIND_VALUE && a->role == ROLE_COMMAND && length >= 17 + 8) {
        /* Rumble + command characteristic: 17-byte rumble prefix. */
        uint8_t *n = s->notify;
        size_t produced = jc2_command(s, value + 17, length - 17, n + 3,
                                      sizeof(s->notify) - 3);
        if (produced) {
            if (produced + 3 > s->mtu)
                produced = s->mtu - 3;
            n[0] = 0x1b;
            put16(n + 1, JC2_HANDLE_RESPONSE);
            s->notify_length = produced + 3;
        }
    }
}

size_t jc2_att_request(Jc2Session *s, const uint8_t *pdu, size_t length,
                       uint8_t *rsp, size_t rsp_size)
{
    s->notify_length = 0;
    if (!length || rsp_size < 5)
        return 0;
    size_t mtu = s->mtu < rsp_size ? s->mtu : rsp_size;
    uint8_t opcode = pdu[0];
    switch (opcode) {
    case 0x02: /* Exchange MTU */
        if (length != 3)
            return error_rsp(rsp, opcode, 0, 0x04);
        {
            uint16_t client = get16(pdu + 1);
            s->mtu = client < 23 ? 23 : client > JC2_SERVER_MTU ? JC2_SERVER_MTU : client;
        }
        rsp[0] = 0x03;
        put16(rsp + 1, JC2_SERVER_MTU);
        return 3;
    case 0x04: return find_information(s, pdu, length, rsp, mtu);
    case 0x06: return find_by_type_value(s, pdu, length, rsp, mtu);
    case 0x08: return read_by_type(s, pdu, length, rsp, mtu);
    case 0x10: return read_by_group(s, pdu, length, rsp, mtu);
    case 0x0a:
    case 0x0c: {
        if (length != (opcode == 0x0a ? 3u : 5u))
            return error_rsp(rsp, opcode, 0, 0x04);
        uint16_t handle = get16(pdu + 1);
        uint16_t offset = opcode == 0x0c ? get16(pdu + 3) : 0;
        const Jc2Attr *a = find(s->side, handle);
        if (!a)
            return error_rsp(rsp, opcode, handle, 0x01);
        uint8_t value[24];
        int n = attr_read(s, a, value, sizeof(value));
        if (n < 0)
            return error_rsp(rsp, opcode, handle, (uint8_t)-n);
        if (offset > n)
            return error_rsp(rsp, opcode, handle, 0x07); /* Invalid Offset */
        size_t count = (size_t)n - offset;
        if (count > mtu - 1)
            count = mtu - 1;
        rsp[0] = opcode + 1;
        memcpy(rsp + 1, value + offset, count);
        return 1 + count;
    }
    case 0x12:
    case 0x52: {
        if (length < 3)
            return opcode == 0x12 ? error_rsp(rsp, opcode, 0, 0x04) : 0;
        uint16_t handle = get16(pdu + 1);
        const Jc2Attr *a = find(s->side, handle);
        bool writable = a && (a->kind == KIND_CCCD || a->kind == KIND_VDESC ||
                              (a->kind == KIND_VALUE &&
                               (a->props & (PROP_WRITE | PROP_WRITE_NR))));
        if (!writable)
            return opcode == 0x12 ? error_rsp(rsp, opcode, handle, a ? 0x03 : 0x01) : 0;
        handle_write(s, a, pdu + 3, length - 3);
        if (opcode == 0x52)
            return 0;
        rsp[0] = 0x13;
        return 1;
    }
    case 0x1e: /* Handle Value Confirmation */
    case 0xd2: /* Signed Write Command */
        return 0;
    default:
        if (opcode & 0x40) /* Commands never get a response. */
            return 0;
        return error_rsp(rsp, opcode, 0, 0x06);
    }
}

static const uint8_t controller_b1[16] = {
    0x5c, 0xf6, 0xee, 0x79, 0x2c, 0xdf, 0x05, 0xe1,
    0xba, 0x2b, 0x63, 0x25, 0xc4, 0x1a, 0x5f, 0x10,
};

static const uint8_t *spi_region(Jc2Side side, uint32_t address, uint16_t *length)
{
    if (side == JC2_LEFT) {
        for (size_t i = 0; i < sizeof(JC2_LEFT_SPI_REGIONS) / sizeof(JC2_LEFT_SPI_REGIONS[0]); i++)
            if (JC2_LEFT_SPI_REGIONS[i].addr == address) {
                *length = JC2_LEFT_SPI_REGIONS[i].len;
                return JC2_LEFT_SPI_REGIONS[i].data;
            }
    } else {
        for (size_t i = 0; i < sizeof(JC2_RIGHT_SPI_REGIONS) / sizeof(JC2_RIGHT_SPI_REGIONS[0]); i++)
            if (JC2_RIGHT_SPI_REGIONS[i].addr == address) {
                *length = JC2_RIGHT_SPI_REGIONS[i].len;
                return JC2_RIGHT_SPI_REGIONS[i].data;
            }
    }
    return NULL;
}

size_t jc2_command(Jc2Session *s, const uint8_t *f, size_t length,
                   uint8_t *out, size_t out_size)
{
    if (length < 8 || out_size < 8 + 96)
        return 0;
    const uint8_t command = f[0], sub = f[3];
    const uint8_t *payload = f + 8;
    size_t payload_length = length - 8;
    uint8_t *rsp = out + 8;
    size_t n = 0;
    bool left = s->side == JC2_LEFT;
    s->last_event = JC2_EVENT_NONE;

    out[0] = command; out[1] = 0x01; out[2] = 0x01; out[3] = sub;
    out[4] = 0x10; out[5] = 0x78; out[6] = 0x00; out[7] = 0x00;

    if (command == 0x07 && sub == 0x01) {
        rsp[0] = 0x00; n = 1;
    } else if (command == 0x02 && sub == 0x04 && payload_length >= 7) {
        uint32_t address = payload[4] | payload[5] << 8 | (uint32_t)payload[6] << 16;
        uint16_t region_length = 0;
        const uint8_t *region = spi_region(s->side, address, &region_length);
        if (!region) {
            memset(rsp, 0, 40);
            rsp[0] = payload[0];
            n = 40;
        } else {
            n = region_length < 96 ? region_length : 96;
            memcpy(rsp, region, n);
            if (address == 0x013000 && n > 44) {
                memcpy(rsp + 33, s->body_color, 3);
                memcpy(rsp + 36, s->button_color, 3);
            }
        }
    } else if (command == 0x15 && sub == 0x01) {
        rsp[0] = 0x01; rsp[1] = 0x04; rsp[2] = 0x01;
        memcpy(rsp + 3, s->controller_address, 6);
        n = 9;
    } else if (command == 0x15 && sub == 0x04 && payload_length >= 17) {
        pairing_compute_ltk(payload + 1, controller_b1, s->ltk);
        s->have_ltk = true;
        s->last_event = JC2_EVENT_KEY_DERIVED;
        rsp[0] = 0x01;
        memcpy(rsp + 1, controller_b1, 16);
        n = 17;
    } else if (command == 0x15 && sub == 0x02 && payload_length >= 17) {
        if (!s->have_ltk || !pairing_compute_b2(s->ltk, payload + 1, rsp + 1, NULL)) {
            rsp[0] = 0x00; n = 1;
        } else {
            rsp[0] = 0x01; n = 17;
        }
    } else if (command == 0x15 && sub == 0x03) {
        if (s->have_ltk)
            s->last_event = JC2_EVENT_PAIRED;
        rsp[0] = 0x01; n = 1;
    } else if (command == 0x0c && (sub == 0x02 || sub == 0x04)) {
        if (sub == 0x02) {
            s->streaming = true;
            s->last_event = JC2_EVENT_STREAMING;
        }
        memset(rsp, 0, 4); n = 4;
    } else if (command == 0x10 && sub == 0x01) {
        const uint8_t *r = left ? JC2_LEFT_RSP_10_01 : JC2_RIGHT_RSP_10_01;
        n = sizeof(JC2_LEFT_RSP_10_01);
        memcpy(rsp, r, n);
    } else if (command == 0x08 && sub == 0x01) {
        n = sizeof(JC2_RSP_08_01);
        memcpy(rsp, JC2_RSP_08_01, n);
    } else if (command == 0x16 && sub == 0x01) {
        memset(rsp, 0, 24); n = 24;
    } else if (command == 0x11 && sub == 0x03) {
        n = sizeof(JC2_LEFT_RSP_11_03);
        memcpy(rsp, left ? JC2_LEFT_RSP_11_03 : JC2_RIGHT_RSP_11_03, n);
    } else if (command == 0x11 && sub == 0x01) {
        n = sizeof(JC2_LEFT_RSP_11_01);
        memcpy(rsp, left ? JC2_LEFT_RSP_11_01 : JC2_RIGHT_RSP_11_01, n);
    } else if (command == 0x01 && sub == 0x0c) {
        n = sizeof(JC2_LEFT_RSP_01_0C);
        memcpy(rsp, left ? JC2_LEFT_RSP_01_0C : JC2_RIGHT_RSP_01_0C, n);
    } else if ((command == 0x09 && sub == 0x07) || (command == 0x08 && sub == 0x02) ||
               (command == 0x0a && (sub == 0x02 || sub == 0x08))) {
        n = 0;
    } else {
        rsp[0] = 0x00; n = 1;
    }
    return 8 + n;
}

typedef struct { int center_x, center_y, throw_x, throw_y; } StickCal;

/* Centers and conservative ranges decoded from SPI region 0x013080. */
static const StickCal stick_cal[2] = {
    [JC2_LEFT] = {2059, 2123, 1180, 1180},
    [JC2_RIGHT] = {2124, 1960, 1150, 1110},
};

void jc2_neutral(Jc2Side side, Jc2Input *input)
{
    input->buttons = 0;
    input->stick_x = (uint16_t)stick_cal[side].center_x;
    input->stick_y = (uint16_t)stick_cal[side].center_y;
}

void jc2_session_init(Jc2Session *s, Jc2Side side, const uint8_t address[6])
{
    memset(s, 0, sizeof(*s));
    s->side = side;
    memcpy(s->controller_address, address, 6);
    s->mtu = 23;
    const uint8_t *spi = side == JC2_LEFT ? JC2_LEFT_SPI_013000 : JC2_RIGHT_SPI_013000;
    memcpy(s->body_color, spi + 33, 3);
    memcpy(s->button_color, spi + 36, 3);
}

void jc2_build_report(Jc2Session *s, const Jc2Input *in, uint8_t r[JC2_REPORT_SIZE])
{
    if (s->side == JC2_LEFT) {
        memcpy(r, JC2_LEFT_BASE_REPORT, JC2_REPORT_SIZE);
        memcpy(r + 0x0e, JC2_LEFT_MOTION_REPORT + 0x0e, 0x37 - 0x0e);
    } else {
        memcpy(r, JC2_RIGHT_BASE_REPORT, JC2_REPORT_SIZE);
    }
    r[0x00] = s->sequence++;
    put16(r + 0x02, in->buttons);
    r[0x04] = 0x07;
    uint32_t x = in->stick_x > 4095 ? 4095 : in->stick_x;
    uint32_t y = in->stick_y > 4095 ? 4095 : in->stick_y;
    uint32_t packed = x | y << 12;
    r[0x05] = packed & 0xff;
    r[0x06] = (packed >> 8) & 0xff;
    r[0x07] = (packed >> 16) & 0xff;
    /* No mouse: zero deltas and a lifted sensor. */
    memset(r + 0x09, 0, 4);
    r[0x0d] = 0xff;
}

void jc2_advertising_data(Jc2Side side, const uint8_t *host, uint8_t out[31])
{
    static const uint8_t base[31] = {
        0x02, 0x01, 0x06, 0x1b, 0xff, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05,
        0x66, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
    };
    memcpy(out, base, 31);
    out[12] = side == JC2_LEFT ? 0x67 : 0x66;
    /* Manufacturer data starts at out[5]; host address lives at offset 0x0c. */
    if (host)
        memcpy(out + 5 + 0x0c, host, 6);
}

/* Pro frame bits as produced by src/input-model.c. */
#define PRO0_Y 0x01
#define PRO0_X 0x02
#define PRO0_B 0x04
#define PRO0_A 0x08
#define PRO0_R 0x40
#define PRO0_ZR 0x80
#define PRO1_MINUS 0x01
#define PRO1_PLUS 0x02
#define PRO1_RSTICK 0x04
#define PRO1_LSTICK 0x08
#define PRO1_HOME 0x10
#define PRO1_CAPTURE 0x20
#define PRO2_DOWN 0x01
#define PRO2_UP 0x02
#define PRO2_RIGHT 0x04
#define PRO2_LEFT 0x08
#define PRO2_L 0x40
#define PRO2_ZL 0x80

static const int pro_center[4] = {2159, 1916, 2070, 2013};
static const int pro_positive[4] = {1466, 1583, 1414, 1510};
static const int pro_negative[4] = {1517, 1465, 1522, 1531};

static double pro_axis(const uint16_t sticks[4], int i)
{
    int delta = (int)sticks[i] - pro_center[i];
    double range = delta >= 0 ? pro_positive[i] : pro_negative[i];
    double value = delta / range;
    return value > 1 ? 1 : value < -1 ? -1 : value;
}

void jc2_from_pro_frame(Jc2Side side, const uint8_t b[3], const uint16_t sticks[4],
                        Jc2Input *out)
{
    /*
     * A single Joy-Con is held sideways. The console rotates sideways
     * Joy-Con input itself, so the emulator sends the physical button and
     * stick that occupy the requested on-screen position:
     *   left Joy-Con: A=Down, B=Left, X=Right, Y=Up; stick x=up, y=-right
     *   right Joy-Con: A=X, B=A, X=Y, Y=B; stick x=-up, y=right
     * L/R become SL/SR, ZL/ZR become the side's own shoulder and trigger.
     */
    double ux = pro_axis(sticks, 0), uy = pro_axis(sticks, 1);
    if (ux * ux + uy * uy < 0.01) {
        ux = pro_axis(sticks, 2);
        uy = pro_axis(sticks, 3);
    }
    uint16_t v = 0;
    double px, py;
    if (b[2] & PRO2_L) v |= JC2_SL;
    if (b[0] & PRO0_R) v |= JC2_SR;
    if (side == JC2_LEFT) {
        if (b[0] & PRO0_A) v |= JC2_L_DOWN;
        if (b[0] & PRO0_B) v |= JC2_L_LEFT;
        if (b[0] & PRO0_X) v |= JC2_L_RIGHT;
        if (b[0] & PRO0_Y) v |= JC2_L_UP;
        if (b[2] & PRO2_DOWN) v |= JC2_L_DOWN;
        if (b[2] & PRO2_LEFT) v |= JC2_L_LEFT;
        if (b[2] & PRO2_RIGHT) v |= JC2_L_RIGHT;
        if (b[2] & PRO2_UP) v |= JC2_L_UP;
        if (b[2] & PRO2_ZL) v |= JC2_L_L;
        if (b[0] & PRO0_ZR) v |= JC2_L_ZL;
        if (b[1] & (PRO1_MINUS | PRO1_PLUS)) v |= JC2_L_MINUS;
        if (b[1] & (PRO1_CAPTURE | PRO1_HOME)) v |= JC2_L_CAPTURE;
        if (b[1] & (PRO1_LSTICK | PRO1_RSTICK)) v |= JC2_L_STICK;
        px = uy;
        py = -ux;
    } else {
        if (b[0] & PRO0_A) v |= JC2_R_X;
        if (b[0] & PRO0_B) v |= JC2_R_A;
        if (b[0] & PRO0_X) v |= JC2_R_Y;
        if (b[0] & PRO0_Y) v |= JC2_R_B;
        if (b[2] & PRO2_RIGHT) v |= JC2_R_X;
        if (b[2] & PRO2_DOWN) v |= JC2_R_A;
        if (b[2] & PRO2_UP) v |= JC2_R_Y;
        if (b[2] & PRO2_LEFT) v |= JC2_R_B;
        if (b[2] & PRO2_ZL) v |= JC2_R_R;
        if (b[0] & PRO0_ZR) v |= JC2_R_ZR;
        if (b[1] & (PRO1_MINUS | PRO1_PLUS)) v |= JC2_R_PLUS;
        if (b[1] & PRO1_HOME) v |= JC2_R_HOME;
        if (b[1] & PRO1_CAPTURE) v |= JC2_R_C;
        if (b[1] & (PRO1_LSTICK | PRO1_RSTICK)) v |= JC2_R_STICK;
        px = -uy;
        py = ux;
    }
    const StickCal *c = &stick_cal[side];
    out->buttons = v;
    out->stick_x = (uint16_t)(c->center_x + (int)(px * c->throw_x));
    out->stick_y = (uint16_t)(c->center_y + (int)(py * c->throw_y));
}

void jc2_from_pro_frame_vertical(Jc2Side side, const uint8_t b[3], const uint16_t sticks[4],
                                 Jc2Input *out)
{
    /* Joy-Con held upright as half of a pair: every button keeps its name. */
    int first = side == JC2_LEFT ? 0 : 2;
    double x = pro_axis(sticks, first), y = pro_axis(sticks, first + 1);
    if (x * x + y * y < 0.01) {
        x = pro_axis(sticks, 2 - first);
        y = pro_axis(sticks, 3 - first);
    }
    uint16_t v = 0;
    if (side == JC2_LEFT) {
        if (b[2] & PRO2_DOWN) v |= JC2_L_DOWN;
        if (b[2] & PRO2_LEFT) v |= JC2_L_LEFT;
        if (b[2] & PRO2_RIGHT) v |= JC2_L_RIGHT;
        if (b[2] & PRO2_UP) v |= JC2_L_UP;
        if (b[2] & PRO2_L) v |= JC2_L_L;
        if (b[2] & PRO2_ZL) v |= JC2_L_ZL;
        if (b[1] & PRO1_MINUS) v |= JC2_L_MINUS;
        if (b[1] & PRO1_CAPTURE) v |= JC2_L_CAPTURE;
        if (b[1] & PRO1_LSTICK) v |= JC2_L_STICK;
    } else {
        if (b[0] & PRO0_A) v |= JC2_R_A;
        if (b[0] & PRO0_B) v |= JC2_R_B;
        if (b[0] & PRO0_X) v |= JC2_R_X;
        if (b[0] & PRO0_Y) v |= JC2_R_Y;
        if (b[0] & PRO0_R) v |= JC2_R_R;
        if (b[0] & PRO0_ZR) v |= JC2_R_ZR;
        if (b[1] & PRO1_PLUS) v |= JC2_R_PLUS;
        if (b[1] & PRO1_HOME) v |= JC2_R_HOME;
        if (b[1] & PRO1_RSTICK) v |= JC2_R_STICK;
    }
    const StickCal *c = &stick_cal[side];
    out->buttons = v;
    out->stick_x = (uint16_t)(c->center_x + (int)(x * c->throw_x));
    out->stick_y = (uint16_t)(c->center_y + (int)(y * c->throw_y));
}
