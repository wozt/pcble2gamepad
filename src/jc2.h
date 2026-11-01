#pragma once
/*
 * Joy-Con 2 BLE peripheral protocol: GATT database, ATT server, Nintendo
 * command responses, proprietary pairing and input reports.
 *
 * This module is independent of the Bluetooth transport. It consumes ATT
 * PDUs and produces ATT PDUs; the caller owns the link and L2CAP framing.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { JC2_LEFT = 0, JC2_RIGHT = 1 } Jc2Side;

/* Report button bits (little-endian 16-bit field at report offset 2). */
#define JC2_L_DOWN    0x0001
#define JC2_L_RIGHT   0x0002
#define JC2_L_LEFT    0x0004
#define JC2_L_UP      0x0008
#define JC2_L_L       0x0010
#define JC2_L_ZL      0x0020
#define JC2_L_MINUS   0x0040
#define JC2_L_STICK   0x0080
#define JC2_L_CAPTURE 0x0100
#define JC2_R_B       0x0001
#define JC2_R_A       0x0002
#define JC2_R_Y       0x0004
#define JC2_R_X       0x0008
#define JC2_R_R       0x0010
#define JC2_R_ZR      0x0020
#define JC2_R_PLUS    0x0040
#define JC2_R_STICK   0x0080
#define JC2_R_HOME    0x0100
#define JC2_R_C       0x1000
#define JC2_SR        0x4000
#define JC2_SL        0x8000

#define JC2_REPORT_SIZE 63
#define JC2_SERVER_MTU 512
#define JC2_ATT_MAX JC2_SERVER_MTU

/* Value handles that the transport needs to address directly. */
#define JC2_HANDLE_INPUT    0x000e
#define JC2_HANDLE_RESPONSE 0x001a

typedef struct {
    uint16_t buttons;
    uint16_t stick_x, stick_y; /* 12-bit raw positions */
} Jc2Input;

typedef enum {
    JC2_EVENT_NONE,
    JC2_EVENT_STREAMING,     /* console requested input reports */
    JC2_EVENT_KEY_DERIVED,   /* LTK known after subcommand 04 */
    JC2_EVENT_PAIRED,        /* subcommand 03 finalized pairing */
} Jc2Event;

typedef struct Jc2Attr Jc2Attr;

typedef struct {
    Jc2Side side;
    uint8_t controller_address[6]; /* least significant octet first */
    uint16_t mtu;
    bool streaming;
    bool have_ltk;
    uint8_t ltk[16];               /* HCI byte order */
    uint8_t sequence;
    uint8_t vendor_descriptor[2];
    uint16_t cccd[8];
    uint8_t body_color[3], button_color[3];
    Jc2Event last_event;
    /* Outgoing notification produced by a write, if any. */
    uint8_t notify[JC2_ATT_MAX];
    size_t notify_length;
} Jc2Session;

void jc2_session_init(Jc2Session *s, Jc2Side side, const uint8_t address_lsb_first[6]);

/* Handle one ATT PDU from the console. Writes a response PDU to rsp and
 * returns its length, or 0 when no response is due (commands). A
 * notification PDU may additionally be left in s->notify. */
size_t jc2_att_request(Jc2Session *s, const uint8_t *pdu, size_t length,
                       uint8_t *rsp, size_t rsp_size);

/* Handle a Nintendo command written to the command characteristics. The
 * command header starts at offset 0. Builds a response notification value
 * in out and returns its length (0 if the command is malformed). */
size_t jc2_command(Jc2Session *s, const uint8_t *command, size_t length,
                   uint8_t *out, size_t out_size);

/* Build one 63-byte input report value. */
void jc2_build_report(Jc2Session *s, const Jc2Input *input, uint8_t out[JC2_REPORT_SIZE]);

/* Neutral stick position for the side. */
void jc2_neutral(Jc2Side side, Jc2Input *input);

/* Advertising data (31 bytes). host may be NULL for pairing mode; it is the
 * console address in least-significant-octet-first order otherwise. */
void jc2_advertising_data(Jc2Side side, const uint8_t *host, uint8_t out[31]);

/* Translate the GUI's Switch Pro Controller frame into a single Joy-Con.
 * See docs/joycon2-ble.md for the mapping. */
void jc2_from_pro_frame(Jc2Side side, const uint8_t buttons[3],
                        const uint16_t sticks[4], Jc2Input *out);
