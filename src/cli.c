#include "client.h"

int main(int argc, char **argv)
{
    g_autofree char *socket_path = NULL;
    gint64 after = 0;
    GOptionEntry options[] = {
        {"socket", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "Unix control socket", "PATH"},
        {"after", 0, 0, G_OPTION_ARG_INT64, &after, "Read logs after this sequence", "SEQ"},
        {NULL}
    };
    g_autoptr(GOptionContext) context = g_option_context_new("status|start|stop|sync|logs|disconnect ADDRESS");
    g_option_context_add_main_entries(context, options, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return 2;
    }
    gboolean disconnect = argc > 1 && g_str_equal(argv[1], "disconnect");
    if (argc != (disconnect ? 3 : 2) || after < 0) {
        g_autofree char *help = g_option_context_get_help(context, TRUE, NULL);
        g_printerr("%s", help);
        return 2;
    }
    if (!socket_path) socket_path = jc_socket_path();
    g_autoptr(JsonObject) response = jc_client_request_full(socket_path, argv[1], after,
                                                         disconnect ? argv[2] : NULL, &error);
    if (!response) { g_printerr("%s\n", error->message); return 1; }
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, response);
    g_autofree char *output = json_to_string(node, TRUE);
    g_print("%s\n", output);
    return json_object_get_boolean_member_with_default(response, "ok", FALSE) ? 0 : 1;
}
