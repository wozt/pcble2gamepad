/* Wire constants and calibration data adapted from NUXBT (MIT), see NUXBT-LICENSE. */
#include "protocol.h"
#include <string.h>

static void pack(uint8_t *p, uint16_t x, uint16_t y) {
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)((x >> 8) | (y << 4));
    p[2] = (uint8_t)(y >> 4);
}

void controller_init(ProState *s, ControllerType type, const uint8_t address[6]) {
    memset(s, 0, sizeof(*s));
    memcpy(s->address, address, 6);
    s->type = type;
    s->mode = type == CONTROLLER_PRO ? 0x3f : 0x30;

    if (type == CONTROLLER_PRO) {
        const uint8_t body[3] = {0x82,0x82,0x82};
        const uint8_t buttons[3] = {0x0f,0x0f,0x0f};

        memcpy(s->body_color, body, 3);
        memcpy(s->button_color, buttons, 3);
        memcpy(s->left_grip_color, body, 3);
        memcpy(s->right_grip_color, body, 3);
    } else if (type == CONTROLLER_JOYCON_L) {
        const uint8_t body[3] = {0x0a,0xb9,0xe6};
        const uint8_t buttons[3] = {0x00,0x1e,0x1e};

        memcpy(s->body_color, body, 3);
        memcpy(s->button_color, buttons, 3);
        memcpy(s->left_grip_color, body, 3);
        memcpy(s->right_grip_color, body, 3);
    } else {
        const uint8_t body[3] = {0xff,0x3c,0x28};
        const uint8_t buttons[3] = {0x1e,0x0a,0x0a};

        memcpy(s->body_color, body, 3);
        memcpy(s->button_color, buttons, 3);
        memcpy(s->left_grip_color, body, 3);
        memcpy(s->right_grip_color, body, 3);
    }

    s->sticks[0] = 0x86f;
    s->sticks[1] = 0x77c;
    s->sticks[2] = 0x816;
    s->sticks[3] = 0x7dd;
}

void controller_set_colors(ProState *s,
                           const uint8_t body[3],
                           const uint8_t buttons[3],
                           const uint8_t left_grip[3],
                           const uint8_t right_grip[3]) {
    memcpy(s->body_color, body, 3);
    memcpy(s->button_color, buttons, 3);
    memcpy(s->left_grip_color, left_grip, 3);
    memcpy(s->right_grip_color, right_grip, 3);
}

void pro_init(ProState *s, const uint8_t address[6]) {
    controller_init(s, CONTROLLER_PRO, address);
}

void pro_input(const ProState *s, uint8_t timer, uint8_t out[50]) {
    memset(out, 0, 50);
    out[0] = 0xa1;
    out[1] = 0x30;
    out[2] = timer;
    out[3] = s->type == CONTROLLER_PRO ? 0x80 : 0x9e;
    if (s->type != CONTROLLER_JOYCON_L) out[4] = s->buttons[0];
    out[5] = s->buttons[1] & (s->type == CONTROLLER_JOYCON_L ? 0x29 :
                              s->type == CONTROLLER_JOYCON_R ? 0x16 : 0xff);
    if (s->type != CONTROLLER_JOYCON_R) out[6] = s->buttons[2];
    if (s->type != CONTROLLER_JOYCON_R) pack(out + 7, s->sticks[0], s->sticks[1]);
    if (s->type != CONTROLLER_JOYCON_L) pack(out + 10, s->sticks[2], s->sticks[3]);
    out[13] = 0x80;
    if (s->imu) for (int i = 0; i < 3; i++) out[19 + 12 * i] = 0x10;
}

static uint16_t axis16(uint16_t value, uint16_t center, bool invert) {
    uint16_t scaled;
    if (value == center) scaled = 0x8000;
    else if (value > center)
        scaled = (uint16_t)(0x8000u +
            ((uint32_t)(value - center) * 0x7fffu) / (4095u - center));
    else
        scaled = (uint16_t)(0x8000u -
            ((uint32_t)(center - value) * 0x8000u) / center);
    if (!invert || scaled == 0x8000) return scaled;
    return (uint16_t)(0xffffu - scaled);
}

size_t pro_stream_input(const ProState *s, uint8_t timer, uint8_t out[50]) {
    if (s->type != CONTROLLER_PRO || s->mode == 0x30) {
        pro_input(s, timer, out);
        return 50;
    }

    memset(out, 0, 50);
    out[0] = 0xa1;
    out[1] = 0x3f;
    out[2] = ((s->buttons[0] & 0x04) ? 0x01 : 0) |
             ((s->buttons[0] & 0x08) ? 0x02 : 0) |
             ((s->buttons[0] & 0x01) ? 0x04 : 0) |
             ((s->buttons[0] & 0x02) ? 0x08 : 0) |
             ((s->buttons[2] & 0x40) ? 0x10 : 0) |
             ((s->buttons[0] & 0x40) ? 0x20 : 0) |
             ((s->buttons[2] & 0x80) ? 0x40 : 0) |
             ((s->buttons[0] & 0x80) ? 0x80 : 0);
    out[3] = (s->buttons[1] & 0x33) |
             ((s->buttons[1] & 0x08) ? 0x04 : 0) |
             ((s->buttons[1] & 0x04) ? 0x08 : 0);

    int vertical = 1 + ((s->buttons[2] & 0x02) != 0) -
                       ((s->buttons[2] & 0x01) != 0);
    int horizontal = 1 + ((s->buttons[2] & 0x04) != 0) -
                         ((s->buttons[2] & 0x08) != 0);
    static const uint8_t hats[3][3] = {
        {5, 4, 3}, {6, 8, 2}, {7, 0, 1}
    };
    out[4] = hats[vertical][horizontal];

    uint16_t axes[4] = {
        axis16(s->sticks[0], 0x86f, false),
        axis16(s->sticks[1], 0x77c, true),
        axis16(s->sticks[2], 0x816, false),
        axis16(s->sticks[3], 0x7dd, true)
    };
    for (int i = 0; i < 4; i++) {
        out[5 + i * 2] = (uint8_t)axes[i];
        out[6 + i * 2] = (uint8_t)(axes[i] >> 8);
    }
    return 13;
}

static uint8_t spi(const ProState *s, uint32_t a) {
    /*
     * Factory stick calibration occupies 0x603D..0x604E.
     * 0x604F is an additional factory byte seen on a real Pro Controller.
     *
     * Do NOT extend this table into 0x6050: that is where the controller
     * body/button colors begin.
     */
    static const uint8_t pro_sticks[] = {
        0xf0,0x07,0x7f,
        0xf0,0x07,0x7f,
        0xf0,0x07,0x7f,
        0xf0,0x07,0x7f,
        0xf0,0x07,0x7f,
        0xf0,0x07,0x7f,
        0x0f
    };
    static const uint8_t pro_config[] = {
        0x5e,0x01,0,0,0xf1,0x0f,0x19,0xd0,0x4c,0xae,0x40,0xe1,
        0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff
    };
    static const uint8_t pro_params[] = {
        0x19,0xd0,0x4c,0xae,0x40,0xe1,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff
    };
    static const uint8_t sticks[] = {
        0xba,0xf5,0x62,0x6f,0xc8,0x77,0xed,0x95,0x5b,
        0x16,0xd8,0x7d,0xf2,0xb5,0x5f,0x86,0x65,0x5e
    };
    static const uint8_t sensor[] = {
        0xd3,0xff,0xd5,0xff,0x55,0x01,0x00,0x40,0x00,0x40,0x00,0x40,
        0x19,0x00,0xdd,0xff,0xdc,0xff,0x3b,0x34,0x3b,0x34,0x3b,0x34
    };
    static const uint8_t params[] = {
        0x0f,0x30,0x61,0x96,0x30,0xf3,0xd4,0x14,0x54,
        0x41,0x15,0x54,0xc7,0x79,0x9c,0x33,0x36,0x63
    };
    /*
     * Factory identity fields used by Nintendo during controller setup.
     *
     * 0x601B controls SPI colors:
     * 0 = disabled, 1 = body/buttons, 2 = body/buttons plus grips.
     */
    if (a == 0x6012)
        return (uint8_t)s->type;
    if (a == 0x6013)
        return 0xa0;
    if (a == 0x601b)
        return s->type == CONTROLLER_PRO ? 0x02 : 0x01;

    if (s->type == CONTROLLER_PRO) {
        if (a >= 0x603d && a < 0x603d + sizeof(pro_sticks))
            return pro_sticks[a - 0x603d];
        if (a >= 0x6080 && a < 0x6080 + sizeof(pro_config)) return pro_config[a - 0x6080];
        if (a >= 0x6098 && a < 0x6098 + sizeof(pro_params)) return pro_params[a - 0x6098];
        if (a >= 0x6050 && a < 0x6053)
            return s->body_color[a - 0x6050];
        if (a >= 0x6053 && a < 0x6056)
            return s->button_color[a - 0x6053];
        if (a >= 0x6056 && a < 0x6059)
            return s->left_grip_color[a - 0x6056];
        if (a >= 0x6059 && a < 0x605c)
            return s->right_grip_color[a - 0x6059];
        if (a >= 0x605c && a < 0x6068)
            return 0;
        if (a >= 0x8010 && a < 0x8040)
            return 0xff;
        if (a >= 0x6020 && a < 0x6038)
            return 0;
        return 0xff;
    }

    if (a >= 0x603d && a < 0x6046 && s->type == CONTROLLER_JOYCON_R) return 0xff;
    if (a >= 0x6046 && a < 0x604f && s->type == CONTROLLER_JOYCON_L) return 0xff;
    if (a >= 0x603d && a < 0x604f) return sticks[a - 0x603d];
    if (a >= 0x6020 && a < 0x6038) return sensor[a - 0x6020];
    if (a >= 0x6080 && a < 0x6086) {
        const uint8_t left[] = {0x5e,0x01,0,0,0xf1,0x0f};
        const uint8_t right[] = {0x5e,0x01,0,0,0x0f,0xf0};
        return (s->type == CONTROLLER_JOYCON_L ? left : right)[a - 0x6080];
    }
    if (a >= 0x6086 && a < 0x6098) return a == 0x6089 ? 0xae : params[a - 0x6086];
    if (a >= 0x6098 && a < 0x60aa) return a == 0x609b ? 0xae : params[a - 0x6098];
    if (a >= 0x6050 && a < 0x6053)
        return s->body_color[a - 0x6050];
    if (a >= 0x6053 && a < 0x6056)
        return s->button_color[a - 0x6053];
    return 0xff;
}

bool pro_reply(ProState *s, const uint8_t *in, size_t len, uint8_t timer, uint8_t out[50]) {
    if (len < 12 || in[0] != 0xa2 || in[1] != 0x01) return false;
    uint8_t cmd = in[11];
    if ((cmd == 3 || cmd == 0x30 || cmd == 0x40 || cmd == 0x48) && len < 13) return false;
    if (cmd == 0x10 && (len < 17 || in[16] > 29)) return false;

    pro_input(s, timer, out);
    out[1] = 0x21;
    memset(out + 14, 0, 36);
    out[14] = 0x80;
    out[15] = cmd;

    switch (cmd) {
    case 1:
        break;
    case 2:
        out[14] = 0x82;
        out[16] = 3;
        out[17] = s->type == CONTROLLER_PRO ? 0x48 : 0x8b;
        out[18] = s->type;
        out[19] = 2;
        memcpy(out + 20, s->address, 6);
        out[26] = 1;

        /*
         * This mirrors SPI 0x601B:
         *   0 = no custom colors
         *   1 = body/buttons
         *   2 = body/buttons plus independent grip colors
         */
        out[27] = s->type == CONTROLLER_PRO ? 2 : 1;
        break;
    case 3:
        s->mode = in[12];
        break;
    case 4: {
        static const uint8_t elapsed[] = {0,0x6a,1,0xbb,1,0x93,1,0x95,1};
        out[14] = 0x83;
        if (s->type == CONTROLLER_PRO) memcpy(out + 16, elapsed, sizeof(elapsed));
        break;
    }
    case 8:
        break;
    case 0x10: {
        uint32_t addr = (uint32_t)in[12] | ((uint32_t)in[13] << 8) |
            ((uint32_t)in[14] << 16) | ((uint32_t)in[15] << 24);
        out[14] = 0x90;
        memcpy(out + 16, in + 12, 5);
        for (unsigned i = 0; i < in[16]; i++) out[21 + i] = spi(s, addr + i);
        break;
    }
    case 0x21:
        if (s->type != CONTROLLER_PRO) {
            static const uint8_t params[] = {1,0,0xff,0,8,0,0x1b,1};
            out[14] = 0xa0;
            memcpy(out + 16, params, sizeof(params));
            out[49] = 0xc8;
        }
        break;
    case 0x22:
        break;
    case 0x30:
        s->lights = in[12];
        break;
    case 0x40:
        s->imu = in[12] != 0;
        break;
    case 0x48:
        if (s->type != CONTROLLER_PRO) out[14] = 0x82;
        s->vibration = in[12] != 0;
        break;
    default:
        return s->type == CONTROLLER_PRO;
    }
    return true;
}
