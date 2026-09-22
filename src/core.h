#pragma once
#include <gio/gio.h>
#include <json-glib/json-glib.h>

typedef struct Bluez Bluez;
typedef struct {
    Bluez *bluez;
    gboolean mock, advertising, gatt, busy, verbose;
    char *adapter_name, *address, *address_type, *last_error;
    GHashTable *peers;
    GQueue events;
    gint64 sequence;
} Engine;

Engine *engine_new(const char *adapter, gboolean mock, gboolean verbose);
void engine_free(Engine *e);
void engine_log(Engine *e, const char *level, const char *event, const char *format, ...) G_GNUC_PRINTF(4,5);
void engine_error(Engine *e, const char *message);
JsonObject *engine_status(Engine *e);
char *engine_request(Engine *e, const char *request);
char *json_serialize_object(JsonObject *object);
