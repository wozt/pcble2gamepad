#pragma once
#include <gio/gio.h>
#include <json-glib/json-glib.h>
char *jc_socket_path(void);
JsonObject *jc_client_request(const char *path, const char *method, gint64 after, GError **error);
JsonObject *jc_client_request_full(const char *path, const char *method, gint64 after,
                                  const char *peer_address, GError **error);
