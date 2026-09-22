#include "protocol.h"

/* Observed in ndeadly's btle_joycon2_advertise.pcapng, frame 1. */
const guint8 jc_advertisement[31] = {
    0x02,0x01,0x06,0x1b,0xff,0x53,0x05,0x01,0x00,0x03,0x7e,0x05,0x66,0x20,
    0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const JcCharacteristic jc_characteristics[] = {
 {0x03,0,"00c5af5d-1964-4e30-8f51-1956f96bd281",{"read",NULL},"Control information",NULL},
 {0x05,0,"00c5af5d-1964-4e30-8f51-1956f96bd282",{"write",NULL},"Control write",NULL},
 {0x07,0,"00c5af5d-1964-4e30-8f51-1956f96bd283",{"read",NULL},"Unknown control value",NULL},
 {0x0a,1,"ab7de9be-89fe-49ad-828f-118f09df7fd2",{"read","notify",NULL},"Report 0x05",JC_RATE_DESCRIPTOR},
 {0x0e,1,"d5a9e01e-2ffc-4cca-b20c-8b67142bf442",{"read","notify",NULL},"Right report 0x08",JC_RATE_DESCRIPTOR},
 {0x12,1,"fa19b0fb-cd1f-46a7-84a1-bbb09e00c149",{"write-without-response",NULL},"Rumble",NULL},
 {0x14,1,"649d4ac9-8eb7-4e6c-af44-1ea54fe5f005",{"write-without-response",NULL},"Command",NULL},
 {0x16,1,"65a724b3-f1e7-4a61-8078-a342376b27ff",{"write-without-response",NULL},"Rumble and command",NULL},
 {0x18,1,"4147423d-fdae-4df7-a4f7-d23e5df59f8d",{"write-without-response",NULL},"Firmware command",NULL},
 {0x1a,1,"c765a961-d9d8-4d36-a20a-5315b111836a",{"notify",NULL},"Command response",JC_RESPONSE_DESCRIPTOR},
 {0x1e,1,"640ca58e-0e88-410c-a7f3-426faf2b690b",{"notify",NULL},"Extended right response",JC_RESPONSE_DESCRIPTOR},
 {0x22,1,"d3bd69d2-841c-4241-ab15-f86f406d2a80",{"notify",NULL},"Unknown response",JC_RESPONSE_DESCRIPTOR},
 {0x26,1,"ab7de9be-89fe-49ad-828f-118f09df7fde",{"read","notify",NULL},"Unknown report",JC_RATE_DESCRIPTOR},
 {0x2a,1,"ab7de9be-89fe-49ad-828f-118f09df7fdf",{"write-without-response",NULL},"Unknown output",NULL}
};
const gsize jc_characteristic_count = G_N_ELEMENTS(jc_characteristics);

char *jc_hex(const guint8 *data, gsize length)
{
    GString *s = g_string_sized_new(length * 3);
    for (gsize i = 0; i < length; i++)
        g_string_append_printf(s, "%s%02x", i ? " " : "", data[i]);
    return g_string_free(s, FALSE);
}

char *jc_describe_command(guint16 handle, const guint8 *data, gsize length)
{
    if (handle != 0x14 && handle != 0x16)
        return NULL;
    gsize offset = handle == 0x16 ? 17 : 0;
    if (length < offset + 8)
        return g_strdup("Malformed command: truncated header");
    const guint8 *h = data + offset;
    const char *stage = "not a pairing command";
    if (h[0] == 0x15) {
        switch (h[3]) {
        case 1: stage = "address exchange"; break;
        case 4: stage = "key exchange"; break;
        case 2: stage = "LTK confirmation"; break;
        case 3: stage = "finalization"; break;
        default: stage = "unknown pairing stage";
        }
    }
    return g_strdup_printf("command=0x%02x direction=0x%02x transport=0x%02x subcommand=0x%02x length=%u stage=%s%s; no response implemented",
                           h[0], h[1], h[2], h[3], h[5], stage,
                           length - offset - 8 < h[5] ? " (truncated data)" : "");
}
