#pragma once
#include "core.h"
Bluez *bluez_new(Engine *engine, GError **error);
void bluez_start(Bluez *b);
void bluez_stop(Bluez *b);
void bluez_disconnect(Bluez *b, const char *path);
void bluez_free(Bluez *b);
