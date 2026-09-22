#include "core.h"
#include "bluez.h"
#include <stdarg.h>

Engine *engine_new(const char *adapter, gboolean mock, gboolean verbose)
{
    Engine *e = g_new0(Engine, 1);
    e->mock = mock;
    e->verbose = verbose;
    e->adapter_name = g_strdup(adapter);
    e->peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
    g_queue_init(&e->events);
    return e;
}

void engine_free(Engine *e)
{
    if (!e) return;
    bluez_free(e->bluez);
    g_free(e->adapter_name);
    g_free(e->address);
    g_free(e->address_type);
    g_free(e->last_error);
    g_hash_table_unref(e->peers);
    g_queue_clear_full(&e->events, (GDestroyNotify)json_object_unref);
    g_free(e);
}

char *json_serialize_object(JsonObject *object)
{
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, object);
    return json_to_string(node, FALSE);
}

void engine_log(Engine *e, const char *level, const char *event, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    g_autofree char *message = g_strdup_vprintf(format, args);
    va_end(args);
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_autofree char *time = g_date_time_format_iso8601(now);
    JsonObject *item = json_object_new();
    json_object_set_int_member(item, "seq", ++e->sequence);
    json_object_set_string_member(item, "time", time);
    json_object_set_string_member(item, "level", level);
    json_object_set_string_member(item, "event", event);
    json_object_set_string_member(item, "message", message);
    g_queue_push_tail(&e->events, item);
    if (e->events.length > 1000)
        json_object_unref(g_queue_pop_head(&e->events));
    if (e->verbose || !g_str_equal(level, "DEBUG")) {
        g_autofree char *line = json_serialize_object(item);
        g_printerr("%s\n", line);
    }
}

void engine_error(Engine *e, const char *message)
{
    g_free(e->last_error);
    e->last_error = g_strdup(message);
    e->busy = FALSE;
    engine_log(e, "ERROR", "backend_error", "%s", message);
}

JsonObject *engine_status(Engine *e)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "state", e->busy ? "transitioning" : e->last_error ? "error" : e->advertising ? "advertising" : "idle");
    json_object_set_boolean_member(o, "advertising_registered", e->advertising);
    json_object_set_boolean_member(o, "gatt_registered", e->gatt);
    json_object_set_boolean_member(o, "simulated", e->mock);
    json_object_set_string_member(o, "adapter", e->adapter_name);
    if (e->address) json_object_set_string_member(o, "local_address", e->address);
    else json_object_set_null_member(o, "local_address");
    if (e->address_type) json_object_set_string_member(o, "adapter_address_type", e->address_type);
    else json_object_set_null_member(o, "adapter_address_type");
    json_object_set_string_member(o, "identity", "Joy-Con 2 R (PID 0x2066)");
    json_object_set_string_member(o, "pairing", "not implemented");
    json_object_set_null_member(o, "encryption");
    json_object_set_null_member(o, "connection_interval_ms");
    json_object_set_int_member(o, "report_rate_hz", 0);
    json_object_set_boolean_member(o, "console_verified", FALSE);
    if (e->last_error) json_object_set_string_member(o, "error", e->last_error);
    else json_object_set_null_member(o, "error");
    JsonArray *peers = json_array_new();
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, e->peers);
    while (g_hash_table_iter_next(&it, NULL, &value))
        json_array_add_object_element(peers, json_object_ref(value));
    json_object_set_array_member(o, "peers", peers);
    return o;
}

static gboolean integer_member(JsonObject *o, const char *key, gint64 *out)
{
    JsonNode *n = json_object_get_member(o, key);
    if (!n || json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
    *out = json_node_get_int(n);
    return TRUE;
}

char *engine_request(Engine *e, const char *request)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonObject) response = json_object_new();
    JsonObject *result = NULL;
    const char *failure = NULL;
    gint64 version;
    if (!json_parser_load_from_data(parser, request, -1, NULL) ||
        !json_parser_get_root(parser) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        failure = "Invalid JSON object";
        goto done;
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(parser));
    JsonNode *method_node = json_object_get_member(o, "method");
    if (!integer_member(o, "version", &version) || version != 1 || !method_node ||
        json_node_get_value_type(method_node) != G_TYPE_STRING) {
        failure = "Expected version=1 and a string method";
        goto done;
    }
    const char *method = json_node_get_string(method_node);
    if (g_str_equal(method, "status")) {
        result = engine_status(e);
    } else if (g_str_equal(method, "logs")) {
        gint64 after = 0;
        if (json_object_has_member(o, "after") && (!integer_member(o, "after", &after) || after < 0)) {
            failure = "after must be a nonnegative integer";
            goto done;
        }
        result = json_object_new();
        JsonArray *events = json_array_new();
        for (GList *l = e->events.head; l; l = l->next) {
            JsonObject *event = l->data;
            if (json_object_get_int_member(event, "seq") > after)
                json_array_add_object_element(events, json_object_ref(event));
        }
        json_object_set_array_member(result, "events", events);
        json_object_set_int_member(result, "cursor", e->sequence);
        json_object_set_int_member(result, "oldest", e->events.head ? json_object_get_int_member(e->events.head->data, "seq") : 0);
    } else if (g_str_equal(method, "disconnect")) {
        if (e->busy) {
            failure = "Operation in progress; query status before retrying";
            goto done;
        }
        JsonNode *address_node = json_object_get_member(o, "address");
        if (!address_node || json_node_get_value_type(address_node) != G_TYPE_STRING ||
            !g_regex_match_simple("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$", json_node_get_string(address_node), 0, 0)) {
            failure = "disconnect requires an explicit Bluetooth address (AA:BB:CC:DD:EE:FF)";
            goto done;
        }
        const char *address = json_node_get_string(address_node);
        GHashTableIter it;
        gpointer key, value;
        const char *path = NULL;
        g_hash_table_iter_init(&it, e->peers);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            const char *candidate = json_object_get_string_member_with_default(value, "address", "");
            if (candidate && g_ascii_strcasecmp(candidate, address) == 0 &&
                json_object_get_boolean_member_with_default(value, "connected", FALSE)) {
                path = key;
                break;
            }
        }
        if (!path) {
            failure = "Address is not a currently observed connected peer on this adapter";
            goto done;
        }
        g_clear_pointer(&e->last_error, g_free);
        engine_log(e, "INFO", "disconnect_requested", "address=%s path=%s; explicit client request for this peer only", address, path);
        if (e->mock) g_hash_table_remove(e->peers, path);
        else bluez_disconnect(e->bluez, path);
        result = engine_status(e);
    } else if (g_str_equal(method, "start") || g_str_equal(method, "sync") || g_str_equal(method, "stop")) {
        if (e->busy) {
            failure = "Operation in progress; query status before retrying";
            goto done;
        }
        gboolean start = !g_str_equal(method, "stop");
        g_clear_pointer(&e->last_error, g_free);
        if (g_str_equal(method, "sync"))
            engine_log(e, "INFO", "sync_requested", "Standard discovery only; proprietary pairing is not implemented");
        if (e->mock) {
            e->advertising = start;
            engine_log(e, "INFO", start ? "mock_advertising_started" : "mock_advertising_stopped", "Simulation; no radio activity");
        } else if (start) {
            bluez_start(e->bluez);
        } else {
            bluez_stop(e->bluez);
        }
        result = engine_status(e);
        if (e->last_error) {
            failure = e->last_error;
            json_object_unref(result);
            result = NULL;
        }
    } else {
        failure = "Unknown method; supported: status, logs, start, stop, sync, disconnect";
    }
done:
    json_object_set_int_member(response, "version", 1);
    json_object_set_boolean_member(response, "ok", failure == NULL);
    if (failure) json_object_set_string_member(response, "error", failure);
    else json_object_set_object_member(response, "result", result);
    return json_serialize_object(response);
}
