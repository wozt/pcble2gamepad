#pragma once
#include <glib.h>

#define JC_COMPANY_ID 0x0553
#define JC_VENDOR_ID 0x057e
#define JC_RIGHT_PID 0x2066
#define JC_SERVICE_CONTROL "00c5af5d-1964-4e30-8f51-1956f96bd280"
#define JC_SERVICE_REPORTS "ab7de9be-89fe-49ad-828f-118f09df7fd0"
#define JC_RATE_DESCRIPTOR "679d5510-5a24-4dee-9557-95df80486ecb"
#define JC_RESPONSE_DESCRIPTOR "b746df8c-f358-495b-9cd2-e3bbeda4f979"

/* Reference handles describe the real device, not BlueZ's allocated handles. */
typedef struct {
    guint16 handle;
    guint service;
    const char *uuid;
    const char *flags[3];
    const char *purpose;
    const char *descriptor;
} JcCharacteristic;
extern const guint8 jc_advertisement[31];
extern const JcCharacteristic jc_characteristics[];
extern const gsize jc_characteristic_count;
char *jc_hex(const guint8 *data, gsize length);
char *jc_describe_command(guint16 reference_handle, const guint8 *data, gsize length);
