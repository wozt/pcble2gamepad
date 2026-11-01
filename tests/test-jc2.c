#include "jc2.h"
#include <glib.h>
#include <string.h>

static const uint8_t address[6] = {0xe5, 0x49, 0xcd, 0x6f, 0x15, 0x60};

static size_t request(Jc2Session *s, const uint8_t *pdu, size_t length, uint8_t *rsp)
{
    return jc2_att_request(s, pdu, length, rsp, JC2_ATT_MAX);
}

static void discovery(void)
{
    Jc2Session s;
    uint8_t rsp[JC2_ATT_MAX];
    jc2_session_init(&s, JC2_RIGHT, address);

    static const uint8_t mtu[] = {0x02, 0x00, 0x02};
    g_assert_cmpuint(request(&s, mtu, sizeof(mtu), rsp), ==, 3);
    g_assert_cmpuint(rsp[0], ==, 0x03);
    g_assert_cmpuint(s.mtu, ==, 512);

    /* Primary services: the two 128-bit Nintendo services come first. */
    static const uint8_t groups[] = {0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28};
    size_t n = request(&s, groups, sizeof(groups), rsp);
    g_assert_cmpuint(rsp[0], ==, 0x11);
    g_assert_cmpuint(rsp[1], ==, 20);
    g_assert_cmpuint(n, ==, 2 + 2 * 20);
    g_assert_cmpuint(rsp[2] | rsp[3] << 8, ==, 0x0001);
    g_assert_cmpuint(rsp[4] | rsp[5] << 8, ==, 0x0007);
    g_assert_cmpuint(rsp[22] | rsp[23] << 8, ==, 0x0008);
    g_assert_cmpuint(rsp[24] | rsp[25] << 8, ==, 0x002a);
    static const uint8_t gap[] = {0x10, 0x2b, 0x00, 0xff, 0xff, 0x00, 0x28};
    n = request(&s, gap, sizeof(gap), rsp);
    g_assert_cmpuint(rsp[1], ==, 6);
    g_assert_cmpuint(n, ==, 2 + 2 * 6);
    static const uint8_t done[] = {0x10, 0x31, 0x00, 0xff, 0xff, 0x00, 0x28};
    n = request(&s, done, sizeof(done), rsp);
    g_assert_cmpuint(rsp[0], ==, 0x01);
    g_assert_cmpuint(rsp[4], ==, 0x0a);

    /* Characteristics of the report service keep the real value handles. */
    static const uint8_t chars[] = {0x08, 0x08, 0x00, 0x2a, 0x00, 0x03, 0x28};
    n = request(&s, chars, sizeof(chars), rsp);
    g_assert_cmpuint(rsp[0], ==, 0x09);
    g_assert_cmpuint(rsp[1], ==, 21);
    g_assert_cmpuint(rsp[2] | rsp[3] << 8, ==, 0x0009);
    g_assert_cmpuint(rsp[4], ==, 0x12);
    g_assert_cmpuint(rsp[5] | rsp[6] << 8, ==, 0x000a);
    g_assert_cmpuint(rsp[23] | rsp[24] << 8, ==, 0x000d);
    g_assert_cmpuint(rsp[26] | rsp[27] << 8, ==, JC2_HANDLE_INPUT);

    /* Descriptors of the command response characteristic. */
    static const uint8_t info[] = {0x04, 0x1b, 0x00, 0x1b, 0x00};
    n = request(&s, info, sizeof(info), rsp);
    g_assert_cmpuint(n, ==, 6);
    g_assert_cmpuint(rsp[1], ==, 0x01);
    g_assert_cmpuint(rsp[4] | rsp[5] << 8, ==, 0x2902);

    static const uint8_t read_cfg1[] = {0x0a, 0x03, 0x00};
    n = request(&s, read_cfg1, sizeof(read_cfg1), rsp);
    static const uint8_t cfg1[] = {0x0b, 0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00};
    g_assert_cmpmem(rsp, n, cfg1, sizeof(cfg1));

    static const uint8_t read_write_only[] = {0x0a, 0x16, 0x00};
    n = request(&s, read_write_only, sizeof(read_write_only), rsp);
    g_assert_cmpuint(rsp[0], ==, 0x01);
    g_assert_cmpuint(rsp[4], ==, 0x02);

    static const uint8_t cccd[] = {0x12, 0x1b, 0x00, 0x01, 0x00};
    g_assert_cmpuint(request(&s, cccd, sizeof(cccd), rsp), ==, 1);
    g_assert_cmpuint(rsp[0], ==, 0x13);
    g_assert_cmpuint(s.cccd[2], ==, 1);

    static const uint8_t name[] = {0x0a, 0x2d, 0x00};
    n = request(&s, name, sizeof(name), rsp);
    g_assert_cmpmem(rsp + 1, n - 1, "Joy-Con 2 (R)", 13);
}

/* Write a command to handle 0x0016 behind the 17-byte rumble prefix. */
static const uint8_t *command(Jc2Session *s, const uint8_t *header_and_payload, size_t length,
                              size_t *value_length)
{
    uint8_t pdu[3 + 17 + 64] = {0x52, 0x16, 0x00};
    uint8_t rsp[JC2_ATT_MAX];
    memcpy(pdu + 3 + 17, header_and_payload, length);
    g_assert_cmpuint(request(s, pdu, 3 + 17 + length, rsp), ==, 0);
    g_assert_cmpuint(s->notify_length, >, 3 + 8);
    g_assert_cmpuint(s->notify[0], ==, 0x1b);
    g_assert_cmpuint(s->notify[1] | s->notify[2] << 8, ==, JC2_HANDLE_RESPONSE);
    *value_length = s->notify_length - 3;
    return s->notify + 3;
}

static void pairing_flow(void)
{
    Jc2Session s;
    size_t n;
    s.mtu = 0;
    jc2_session_init(&s, JC2_LEFT, address);
    s.mtu = 512;

    static const uint8_t addresses[] = {
        0x15, 0x91, 0x01, 0x01, 0x00, 0x0e, 0x00, 0x00,
        0x00, 0x02, 0x81, 0xeb, 0x3a, 0xeb, 0xf1, 0x48, 0x80, 0xeb, 0x3a, 0xeb, 0xf1, 0x48,
    };
    const uint8_t *r = command(&s, addresses, sizeof(addresses), &n);
    static const uint8_t header[] = {0x15, 0x01, 0x01, 0x01, 0x10, 0x78, 0x00, 0x00};
    g_assert_cmpmem(r, 8, header, 8);
    g_assert_cmpuint(n, ==, 8 + 9);
    g_assert_cmpmem(r + 11, 6, address, 6);

    /* Capture-derived vector, see tests/test-pairing.c. */
    static const uint8_t key_exchange[] = {
        0x15, 0x91, 0x01, 0x04, 0x00, 0x11, 0x00, 0x00, 0x00,
        0x35, 0x03, 0xe9, 0x29, 0x82, 0x87, 0x71, 0x24,
        0xbe, 0xa8, 0x0c, 0x66, 0x46, 0x15, 0x83, 0x4b,
    };
    r = command(&s, key_exchange, sizeof(key_exchange), &n);
    g_assert_cmpuint(n, ==, 8 + 17);
    g_assert_cmpuint(s.last_event, ==, JC2_EVENT_KEY_DERIVED);
    static const uint8_t expected_ltk[16] = {
        0x69, 0xf5, 0x07, 0x50, 0xae, 0x58, 0x74, 0xc5,
        0x04, 0x83, 0x6f, 0x43, 0x82, 0x0f, 0xdc, 0x5b,
    };
    g_assert_true(s.have_ltk);
    g_assert_cmpmem(s.ltk, 16, expected_ltk, 16);

    static const uint8_t confirm[] = {
        0x15, 0x91, 0x01, 0x02, 0x00, 0x11, 0x00, 0x00, 0x00,
        0x6f, 0xc6, 0xdf, 0x8a, 0xd8, 0xfe, 0xdf, 0x15,
        0xbb, 0x8c, 0x15, 0xe9, 0x1f, 0x32, 0x05, 0x44,
    };
    r = command(&s, confirm, sizeof(confirm), &n);
    static const uint8_t expected_b2[16] = {
        0x13, 0x4c, 0x97, 0xf5, 0x11, 0xb9, 0xb6, 0xdd,
        0x4d, 0x86, 0xfd, 0x40, 0xf5, 0x36, 0xe9, 0xed,
    };
    g_assert_cmpuint(n, ==, 8 + 17);
    g_assert_cmpuint(r[8], ==, 0x01);
    g_assert_cmpmem(r + 9, 16, expected_b2, 16);

    static const uint8_t finalize[] = {0x15, 0x91, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00};
    r = command(&s, finalize, sizeof(finalize), &n);
    g_assert_cmpuint(s.last_event, ==, JC2_EVENT_PAIRED);

    static const uint8_t spi[] = {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00,
                                  0x40, 0x7e, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00};
    r = command(&s, spi, sizeof(spi), &n);
    g_assert_cmpuint(n, ==, 8 + 72);
    g_assert_cmpuint(r[8 + 4], ==, 0x00);
    g_assert_cmpuint(r[8 + 5], ==, 0x30);

    static const uint8_t stream[] = {0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00,
                                     0x37, 0x00, 0x00, 0x00};
    command(&s, stream, sizeof(stream), &n);
    g_assert_true(s.streaming);
}

static void reports(void)
{
    Jc2Session s;
    jc2_session_init(&s, JC2_RIGHT, address);
    Jc2Input in = {.buttons = JC2_R_A | JC2_SR, .stick_x = 0x123, .stick_y = 0xabc};
    uint8_t r[JC2_REPORT_SIZE];
    jc2_build_report(&s, &in, r);
    g_assert_cmpuint(r[0], ==, 0);
    g_assert_cmpuint(r[2] | r[3] << 8, ==, JC2_R_A | JC2_SR);
    g_assert_cmpuint(r[5], ==, 0x23);
    g_assert_cmpuint(r[6], ==, 0xc1);
    g_assert_cmpuint(r[7], ==, 0xab);
    g_assert_cmpuint(r[0x0d], ==, 0xff);
    Jc2Input mouse = {.stick_x = 2048, .stick_y = 2048, .mouse_dx = -5, .mouse_dy = 700, .mouse = true};
    jc2_build_report(&s, &mouse, r);
    g_assert_cmpint((int16_t)(r[0x09] | r[0x0a] << 8), ==, -5);
    g_assert_cmpint((int16_t)(r[0x0b] | r[0x0c] << 8), ==, 700);
    g_assert_cmpuint(r[0x0d], ==, 0x3d);
    jc2_build_report(&s, &in, r);
    g_assert_cmpuint(r[0], ==, 2);

    /* Neutral Pro frame stays centered; L+R become SL+SR. */
    const uint8_t buttons[3] = {0x40 | 0x08, 0, 0x40};
    const uint16_t sticks[4] = {2159, 1916, 2070, 2013};
    Jc2Input out, neutral;
    jc2_from_pro_frame(JC2_RIGHT, buttons, sticks, &out);
    jc2_neutral(JC2_RIGHT, &neutral);
    g_assert_cmpuint(out.buttons, ==, JC2_SL | JC2_SR | JC2_R_X);
    g_assert_cmpuint(out.stick_x, ==, neutral.stick_x);
    g_assert_cmpuint(out.stick_y, ==, neutral.stick_y);
    jc2_from_pro_frame(JC2_LEFT, buttons, sticks, &out);
    g_assert_cmpuint(out.buttons, ==, JC2_SL | JC2_SR | JC2_L_DOWN);

    Jc2Input upright;
    jc2_from_pro_frame_vertical(JC2_RIGHT, buttons, sticks, &upright);
    g_assert_cmpuint(upright.buttons, ==, JC2_R_R | JC2_R_A);
    jc2_from_pro_frame_vertical(JC2_LEFT, buttons, sticks, &upright);
    g_assert_cmpuint(upright.buttons, ==, JC2_L_L);

    uint8_t adv[31];
    jc2_advertising_data(JC2_LEFT, NULL, adv);
    g_assert_cmpuint(adv[12] | adv[13] << 8, ==, 0x2067);
    g_assert_cmpuint(adv[5 + 0x0c], ==, 0);
    jc2_advertising_data(JC2_RIGHT, address, adv);
    g_assert_cmpuint(adv[12] | adv[13] << 8, ==, 0x2066);
    g_assert_cmpmem(adv + 5 + 0x0c, 6, address, 6);
    g_assert_cmpuint(adv[5 + 0x12], ==, 0x0f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/jc2/discovery", discovery);
    g_test_add_func("/jc2/pairing-flow", pairing_flow);
    g_test_add_func("/jc2/reports", reports);
    return g_test_run();
}
