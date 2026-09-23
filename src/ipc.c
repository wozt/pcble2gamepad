#define _GNU_SOURCE
#include "ipc.h"
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

struct IpcServer {
    IpcDispatch dispatch;
    gpointer data;
    uid_t owner;
    GSocketService *service;
    GList *clients;
    char *path;
    int lock_fd;
};

typedef struct {
    IpcServer *server;
    GSocketConnection *connection;
    GCancellable *cancel;
    GByteArray *input;
    char *output;
    guint timeout;
} Client;

static gboolean client_timeout(gpointer data)
{
    Client *c = data;
    c->timeout = 0;
    g_cancellable_cancel(c->cancel);
    return G_SOURCE_REMOVE;
}

static void client_free(Client *c)
{
    c->server->clients = g_list_remove(c->server->clients, c);
    if (c->timeout) g_source_remove(c->timeout);
    g_io_stream_close(G_IO_STREAM(c->connection), NULL, NULL);
    g_object_unref(c->connection);
    g_object_unref(c->cancel);
    g_byte_array_unref(c->input);
    g_free(c->output);
    g_free(c);
}

static void written(GObject *source, GAsyncResult *result, gpointer data)
{
    g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);
    client_free(data);
}

static void respond(Client *c, const char *response)
{
    c->output = g_strconcat(response, "\n", NULL);
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(c->connection)),
        c->output, strlen(c->output), G_PRIORITY_DEFAULT, c->cancel, written, c);
}

static void read_request(GObject *source, GAsyncResult *result, gpointer data)
{
    Client *c = data;
    g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, NULL);
    if (!bytes || !g_bytes_get_size(bytes)) { client_free(c); return; }
    gsize count;
    const guint8 *chunk = g_bytes_get_data(bytes, &count);
    const guint8 *newline = memchr(chunk, '\n', count);
    gsize length = newline ? (gsize)(newline - chunk) : count;
    if (c->input->len + length > 4096 || memchr(chunk, '\0', length)) {
        respond(c, "{\"version\":1,\"ok\":false,\"error\":\"Request exceeds 4096 bytes or contains NUL\"}");
        return;
    }
    g_byte_array_append(c->input, chunk, (guint)length);
    if (newline) {
        g_byte_array_append(c->input, (const guint8 *)"", 1);
        g_autofree char *response = c->server->dispatch(c->server->data, (const char *)c->input->data);
        respond(c, response);
    } else {
        g_input_stream_read_bytes_async(G_INPUT_STREAM(source), 1024, G_PRIORITY_DEFAULT, c->cancel, read_request, c);
    }
}

static gboolean incoming(GSocketService *service G_GNUC_UNUSED, GSocketConnection *connection,
                         GObject *source G_GNUC_UNUSED, gpointer data)
{
    IpcServer *s = data;
    g_autoptr(GCredentials) credentials = g_socket_get_credentials(g_socket_connection_get_socket(connection), NULL);
    if (g_list_length(s->clients) >= 32 || !credentials || g_credentials_get_unix_user(credentials, NULL) != s->owner) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
    }
    Client *c = g_new0(Client, 1);
    c->server = s;
    c->connection = g_object_ref(connection);
    c->cancel = g_cancellable_new();
    c->input = g_byte_array_new();
    c->timeout = g_timeout_add_seconds(10, client_timeout, c);
    s->clients = g_list_prepend(s->clients, c);
    g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(connection)), 1024,
        G_PRIORITY_DEFAULT, c->cancel, read_request, c);
    return TRUE;
}

IpcServer *ipc_server_new_full(IpcDispatch dispatch, gpointer data, const char *path, uid_t owner, GError **error)
{
    g_autoptr(GSocketAddress) address = NULL;
    g_autofree char *directory = g_path_get_dirname(path);
    struct stat st;
    if (g_mkdir_with_parents(directory, 0700) < 0 || lstat(directory, &st) < 0 ||
        !S_ISDIR(st.st_mode) || st.st_uid != owner || (st.st_mode & 0077)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Socket parent must be an owned private directory (mode 0700)");
        return NULL;
    }
    IpcServer *s = g_new0(IpcServer, 1);
    s->dispatch = dispatch; s->data = data; s->owner = owner;
    s->lock_fd = -1;
    g_autofree char *lock_path = g_strconcat(path, ".lock", NULL);
    s->lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (s->lock_fd < 0 || flock(s->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Cannot acquire socket lock; another daemon may be running");
        goto fail;
    }
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != owner) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Refusing to replace a non-socket or another user's socket");
            goto fail;
        }
        /* Lock ownership ensures this is a stale socket from this application. */
        if (g_unlink(path) < 0) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot remove stale socket");
            goto fail;
        }
    }
    s->service = g_socket_service_new();
    address = g_unix_socket_address_new(path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(s->service), address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error)) goto fail;
    s->path = g_strdup(path);
    if (g_chmod(path, 0600) < 0 || (owner != getuid() && chown(path, owner, (gid_t)-1) < 0)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot set socket permissions");
        goto fail;
    }
    g_signal_connect(s->service, "incoming", G_CALLBACK(incoming), s);
    g_socket_service_start(s->service);
    return s;
fail:
    ipc_server_free(s);
    return NULL;
}

void ipc_server_free(IpcServer *s)
{
    if (!s) return;
    if (s->service) {
        g_socket_service_stop(s->service);
        g_socket_listener_close(G_SOCKET_LISTENER(s->service));
    }
    for (GList *l = s->clients; l; l = l->next) g_cancellable_cancel(((Client *)l->data)->cancel);
    while (s->clients) g_main_context_iteration(NULL, TRUE);
    g_clear_object(&s->service);
    if (s->path) g_unlink(s->path);
    if (s->lock_fd >= 0) close(s->lock_fd);
    g_free(s->path);
    g_free(s);
}
