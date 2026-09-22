#pragma once
#include "core.h"
typedef struct IpcServer IpcServer;
IpcServer *ipc_server_new(Engine *engine, const char *path, GError **error);
void ipc_server_free(IpcServer *server);
