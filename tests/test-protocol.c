#include "protocol.h"
#include <string.h>

static void advertisement(void)
{
    /* Independent captured ADV data; includes the company ID only once. */
    const char *capture = "02 01 06 1b ff 53 05 01 00 03 7e 05 66 20 00 01 00 00 00 00 00 00 00 0f 00 00 00 00 00 00 00";
    g_autofree char *hex = jc_hex(jc_advertisement, sizeof(jc_advertisement));
    g_assert_cmpstr(hex, ==, capture);
    gsize offset = 0;
    while (offset < sizeof(jc_advertisement)) {
        g_assert_cmpuint(jc_advertisement[offset], >, 0);
        offset += jc_advertisement[offset] + 1;
    }
    g_assert_cmpuint(offset, ==, 31);
}

static void captured_command(void)
{
    /* Pairing capture frame 573: 17-byte rumble prefix then 8-byte command header. */
    guint8 value[39] = {0};
    const guint8 command[] = {0x15,0x91,0x01,0x01,0x00,0x0e,0x00,0x00,0x00,0x02,
                              0x5f,0x11,0x85,0xeb,0xf1,0x48,0x5e,0x11,0x85,0xeb,0xf1,0x48};
    memcpy(value + 17, command, sizeof(command));
    g_autofree char *description = jc_describe_command(0x16, value, sizeof(value));
    g_assert_nonnull(strstr(description, "stage=address exchange"));
    g_assert_null(strstr(description, "truncated"));
    for (gsize length = 0; length < 25; length++) {
        g_autofree char *short_description = jc_describe_command(0x16, value, length);
        g_assert_nonnull(strstr(short_description, "truncated header"));
    }
    g_autofree char *truncated = jc_describe_command(0x16, value, 25);
    g_assert_nonnull(strstr(truncated, "truncated data"));
    g_assert_null(jc_describe_command(0x12, value, sizeof(value)));
}

static void gatt_topology(void)
{
    g_assert_cmpuint(jc_characteristic_count, ==, 14);
    guint notifications = 0;
    for (gsize i = 0; i < jc_characteristic_count; i++) {
        const JcCharacteristic *s = &jc_characteristics[i];
        if (s->descriptor) notifications++;
        for (gsize j = 0; j < i; j++) {
            g_assert_cmpuint(s->handle, !=, jc_characteristics[j].handle);
            g_assert_cmpstr(s->uuid, !=, jc_characteristics[j].uuid);
        }
        if (s->handle == 0x0e) g_assert_cmpstr(s->uuid, ==, "d5a9e01e-2ffc-4cca-b20c-8b67142bf442");
    }
    g_assert_cmpuint(notifications, ==, 6);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/protocol/advertisement-capture", advertisement);
    g_test_add_func("/protocol/captured-command", captured_command);
    g_test_add_func("/protocol/gatt-topology", gatt_topology);
    return g_test_run();
}
