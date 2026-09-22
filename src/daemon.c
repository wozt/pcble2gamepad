#include "core.h"
#include "bluez.h"
#include "ipc.h"
#include "client.h"
#include <glib-unix.h>
#include <signal.h>

static gboolean quit_loop(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    gboolean mock = FALSE, verbose = FALSE;
    g_autofree char *adapter = NULL;
    g_autofree char *socket_path = NULL;
    GOptionEntry options[] = {
        {"adapter", 0, 0, G_OPTION_ARG_STRING, &adapter, "Bluetooth adapter (default hci0)", "NAME"},
        {"socket", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "Unix control socket", "PATH"},
        {"mock", 0, 0, G_OPTION_ARG_NONE, &mock, "Simulate lifecycle without Bluetooth", NULL},
        {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Print detailed diagnostic events", NULL},
        {NULL}
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- experimental Joy-Con 2 discovery daemon");
    g_option_context_add_main_entries(context, options, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return 2;
    }
    if (!adapter) adapter = g_strdup("hci0");
    if (!g_regex_match_simple("^hci[0-9]+$", adapter, 0, 0) || argc != 1) {
        g_printerr("Expected --adapter hciN and no positional arguments\n");
        return 2;
    }
    if (!socket_path) socket_path = jc_socket_path();
    Engine *engine = engine_new(adapter, mock, verbose);
    IpcServer *ipc = ipc_server_new(engine, socket_path, &error);
    if (!ipc) {
        g_printerr("%s\n", error->message);
        engine_free(engine);
        return 1;
    }
    if (!mock) {
        engine->bluez = bluez_new(engine, &error);
        if (!engine->bluez) {
            g_printerr("%s\n", error->message);
            ipc_server_free(ipc);
            engine_free(engine);
            return 1;
        }
    }
    engine_log(engine, "INFO", "daemon_ready", "backend=%s; advertising starts only on an explicit API request", mock ? "mock" : "BlueZ D-Bus");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    guint sigint = g_unix_signal_add(SIGINT, quit_loop, loop);
    guint sigterm = g_unix_signal_add(SIGTERM, quit_loop, loop);
    g_main_loop_run(loop);
    g_source_remove(sigint);
    g_source_remove(sigterm);
    ipc_server_free(ipc);
    engine_log(engine, "INFO", "daemon_stopping", "Closing private D-Bus connection releases advertising and GATT registrations");
    engine_free(engine);
    return 0;
}
