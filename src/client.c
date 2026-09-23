#include "client.h"
#include <gio/gunixsocketaddress.h>
#include <string.h>

char *jc_socket_path(void)
{
    const char *override = g_getenv("PCBLE2GAMEPAD_SOCKET");
    if (override && *override) return g_strdup(override);
    return g_build_filename(g_get_user_runtime_dir(), "pcble2gamepad", "control.sock", NULL);
}

JsonObject *jc_client_request(const char *path, const char *method, gint64 after, GError **error)
{
    return jc_client_request_full(path, method, after, NULL, error);
}

JsonObject *jc_client_request_full(const char *path, const char *method, gint64 after,
                                  const char *peer_address, GError **error)
{
    g_autoptr(JsonObject) request = json_object_new();
    json_object_set_int_member(request, "version", 1);
    json_object_set_string_member(request, "method", method);
    if (peer_address) json_object_set_string_member(request, "address", peer_address);
    if (g_str_equal(method, "logs")) json_object_set_int_member(request, "after", after);
    return jc_client_request_object(path, request, error);
}

JsonObject *jc_client_request_object(const char *path, JsonObject *request, GError **error)
{
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_socket_client_set_timeout(client, 2);
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    g_autoptr(GSocketConnection) connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, error);
    if (!connection) return NULL;
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, request);
    g_autofree char *json = json_to_string(node, FALSE);
    g_autofree char *line = g_strconcat(json, "\n", NULL);
    if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)), line, strlen(line), NULL, NULL, error)) return NULL;
    g_autoptr(GByteArray) buffer = g_byte_array_new();
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gboolean complete = FALSE;
    while (buffer->len < 2 * 1024 * 1024) {
        guint8 chunk[4096];
        gssize count = g_input_stream_read(input, chunk, sizeof(chunk), NULL, error);
        if (count < 0) return NULL;
        if (!count) break;
        guint8 *newline = memchr(chunk, '\n', (gsize)count);
        g_byte_array_append(buffer, chunk, newline ? (guint)(newline - chunk) : (guint)count);
        if (newline) { complete = TRUE; break; }
    }
    if (!complete) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Incomplete or oversized daemon response");
        return NULL;
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, (const char *)buffer->data, buffer->len, error)) return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Daemon response is not an object");
        return NULL;
    }
    return json_object_ref(json_node_get_object(root));
}
