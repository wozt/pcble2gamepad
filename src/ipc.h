#pragma once
#include "core.h"
typedef struct IpcServer IpcServer;
IpcServer *ipc_server_new(Engine *engine, const char *path, GError **error);
void ipc_server_free(IpcServer *server);
#include <sys/types.h>
typedef char *(*IpcDispatch)(gpointer data, const char *request);
IpcServer *ipc_server_new_full(IpcDispatch dispatch, gpointer data, const char *path,
                               uid_t owner, GError **error);
