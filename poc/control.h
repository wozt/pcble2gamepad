#pragma once
#include "protocol.h"
#include <glib.h>
#include <sys/types.h>
typedef struct ProControl ProControl;

typedef gboolean (*ProReconnectFunc)(const char *address,
                                     gpointer user_data,
                                     GError **error);

ProControl *pro_control_new(ProState *state, gint64 *release_at, gboolean *verbose, uid_t owner,
                            gboolean mock, const char *socket_name, GMainLoop *loop, GError **error);
void pro_control_set_reconnect(ProControl *c, ProReconnectFunc callback, gpointer user_data);
void pro_control_update(ProControl *c, const char *peer, gboolean initialized, unsigned tx, unsigned rx);
void pro_control_log(ProControl *c, const char *event, const char *detail);
gboolean pro_control_expired(ProControl *c);
void pro_control_free(ProControl *c);
