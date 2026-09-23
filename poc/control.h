#pragma once
#include "protocol.h"
#include <glib.h>
#include <sys/types.h>
typedef struct ProControl ProControl;
ProControl *pro_control_new(ProState *state, gint64 *release_at, uid_t owner, gboolean mock,
                            GMainLoop *loop, GError **error);
void pro_control_update(ProControl *c, const char *peer, gboolean initialized, unsigned tx, unsigned rx);
void pro_control_log(ProControl *c, const char *event, const char *detail);
gboolean pro_control_expired(ProControl *c);
void pro_control_free(ProControl *c);
