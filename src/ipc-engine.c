#include "ipc.h"
#include <unistd.h>
static char *dispatch(gpointer data, const char *request) { return engine_request(data, request); }
IpcServer *ipc_server_new(Engine *engine, const char *path, GError **error)
{
    IpcServer *server = ipc_server_new_full(dispatch, engine, path, getuid(), error);
    if (server) engine_log(engine, "INFO", "ipc_listening", "path=%s mode=0600 same-user-only", path);
    return server;
}
