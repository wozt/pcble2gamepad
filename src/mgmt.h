#pragma once
#include <gio/gio.h>

gboolean mgmt_set_preferred_phy(const char *adapter, gboolean le_2m, GError **error);
