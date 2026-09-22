#include "bluez.h"
#include "protocol.h"
#include <string.h>

#define ROOT "/io/github/wozt/pcble2joycon2"
#define APP ROOT "/gatt"
#define ADV ROOT "/advertisement"
#define OM "org.freedesktop.DBus.ObjectManager"
#define PROPS "org.freedesktop.DBus.Properties"
#define AD "org.bluez.LEAdvertisement1"
#define SERVICE "org.bluez.GattService1"
#define CHARACTERISTIC "org.bluez.GattCharacteristic1"
#define DESCRIPTOR "org.bluez.GattDescriptor1"
#define DEVICE "org.bluez.Device1"
#define ADAPTER "org.bluez.Adapter1"
#define ADV_MANAGER "org.bluez.LEAdvertisingManager1"
#define GATT_MANAGER "org.bluez.GattManager1"

typedef enum { OBJ_ROOT, OBJ_ADV, OBJ_SERVICE, OBJ_CHAR, OBJ_DESC } ObjectKind;
typedef struct {
    Bluez *backend;
    ObjectKind kind;
    char *path;
    const char *interface;
    guint service;
    const JcCharacteristic *spec;
    guint registration;
    gboolean notifying;
} Object;

struct Bluez {
    Engine *engine;
    GDBusConnection *bus;
    GDBusNodeInfo *xml;
    GCancellable *cancel;
    GPtrArray *objects;
    char *adapter_path;
    guint changed_subscription, added_subscription, removed_subscription, owner_subscription;
    guint pending;
    gboolean closing;
    char *disconnect_path;
};

static const char xml[] =
"<node>"
"<interface name='org.freedesktop.DBus.ObjectManager'>"
"<method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
"</interface>"
"<interface name='org.bluez.LEAdvertisement1'>"
"<method name='Release'/>"
"<property name='Type' type='s' access='read'/>"
"<property name='ManufacturerData' type='a{qv}' access='read'/>"
"<property name='Discoverable' type='b' access='read'/>"
"<property name='DiscoverableTimeout' type='q' access='read'/>"
"<property name='Includes' type='as' access='read'/>"
"<property name='MinInterval' type='u' access='read'/>"
"<property name='MaxInterval' type='u' access='read'/>"
"</interface>"
"<interface name='org.bluez.GattService1'>"
"<property name='UUID' type='s' access='read'/>"
"<property name='Primary' type='b' access='read'/>"
"</interface>"
"<interface name='org.bluez.GattCharacteristic1'>"
"<method name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/></method>"
"<method name='WriteValue'><arg type='ay' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='StartNotify'/><method name='StopNotify'/>"
"<property name='UUID' type='s' access='read'/>"
"<property name='Service' type='o' access='read'/>"
"<property name='Flags' type='as' access='read'/>"
"<property name='Notifying' type='b' access='read'/>"
"</interface>"
"<interface name='org.bluez.GattDescriptor1'>"
"<method name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/></method>"
"<method name='WriteValue'><arg type='ay' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<property name='UUID' type='s' access='read'/>"
"<property name='Characteristic' type='o' access='read'/>"
"<property name='Flags' type='as' access='read'/>"
"</interface>"
"</node>";

static GVariant *object_properties(Object *o)
{
    GVariantBuilder p;
    g_variant_builder_init(&p, G_VARIANT_TYPE_VARDICT);
    if (o->kind == OBJ_ADV) {
        GVariantBuilder manufacturer;
        g_variant_builder_init(&manufacturer, G_VARIANT_TYPE("a{qv}"));
        g_variant_builder_add(&manufacturer, "{qv}", JC_COMPANY_ID,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, jc_advertisement + 7, 24, 1));
        g_variant_builder_add(&p, "{sv}", "Type", g_variant_new_string("peripheral"));
        g_variant_builder_add(&p, "{sv}", "ManufacturerData", g_variant_builder_end(&manufacturer));
        g_variant_builder_add(&p, "{sv}", "Discoverable", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&p, "{sv}", "DiscoverableTimeout", g_variant_new_uint16(0));
        g_variant_builder_add(&p, "{sv}", "Includes", g_variant_new_strv(NULL, 0));
        g_variant_builder_add(&p, "{sv}", "MinInterval", g_variant_new_uint32(20));
        g_variant_builder_add(&p, "{sv}", "MaxInterval", g_variant_new_uint32(20));
    } else if (o->kind == OBJ_SERVICE) {
        g_variant_builder_add(&p, "{sv}", "UUID", g_variant_new_string(o->service ? JC_SERVICE_REPORTS : JC_SERVICE_CONTROL));
        g_variant_builder_add(&p, "{sv}", "Primary", g_variant_new_boolean(TRUE));
    } else if (o->kind == OBJ_CHAR) {
        g_autofree char *service = g_strdup_printf(APP "/service%u", o->spec->service);
        g_variant_builder_add(&p, "{sv}", "UUID", g_variant_new_string(o->spec->uuid));
        g_variant_builder_add(&p, "{sv}", "Service", g_variant_new_object_path(service));
        g_variant_builder_add(&p, "{sv}", "Flags", g_variant_new_strv(o->spec->flags, -1));
        g_variant_builder_add(&p, "{sv}", "Notifying", g_variant_new_boolean(o->notifying));
    } else if (o->kind == OBJ_DESC) {
        g_autofree char *parent = g_path_get_dirname(o->path);
        const char *flags[] = {"read", "write", NULL};
        g_variant_builder_add(&p, "{sv}", "UUID", g_variant_new_string(o->spec->descriptor));
        g_variant_builder_add(&p, "{sv}", "Characteristic", g_variant_new_object_path(parent));
        g_variant_builder_add(&p, "{sv}", "Flags", g_variant_new_strv(flags, -1));
    }
    return g_variant_builder_end(&p);
}

static GVariant *get_property(GDBusConnection *connection G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
    const char *path G_GNUC_UNUSED, const char *interface G_GNUC_UNUSED, const char *property,
    GError **error G_GNUC_UNUSED, gpointer data)
{
    g_autoptr(GVariant) properties = g_variant_ref_sink(object_properties(data));
    return g_variant_lookup_value(properties, property, NULL);
}

static void method_call(GDBusConnection *connection G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
    const char *path G_GNUC_UNUSED, const char *interface G_GNUC_UNUSED, const char *method,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
    Object *o = data;
    Bluez *b = o->backend;
    Engine *e = b->engine;
    if (o->kind == OBJ_ROOT && g_str_equal(method, "GetManagedObjects")) {
        GVariantBuilder objects;
        g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        for (guint i = 0; i < b->objects->len; i++) {
            Object *child = g_ptr_array_index(b->objects, i);
            if (child->kind < OBJ_SERVICE) continue;
            GVariantBuilder interfaces;
            g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_add(&interfaces, "{s@a{sv}}", child->interface, object_properties(child));
            g_variant_builder_add(&objects, "{o@a{sa{sv}}}", child->path, g_variant_builder_end(&interfaces));
        }
        engine_log(e, "DEBUG", "gatt_export", "BlueZ requested application objects; this is not peer service discovery");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{oa{sa{sv}}})", &objects));
        return;
    }
    if (o->kind == OBJ_ADV && g_str_equal(method, "Release")) {
        e->advertising = FALSE;
        engine_log(e, "INFO", "advertisement_released", "BlueZ removed the advertisement");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (o->kind == OBJ_CHAR || o->kind == OBJ_DESC) {
        g_autofree char *options = NULL;
        if (g_str_equal(method, "ReadValue")) {
            g_autoptr(GVariant) opts = g_variant_get_child_value(parameters, 0);
            options = g_variant_print(opts, TRUE);
            engine_log(e, "INFO", "gatt_read", "path=%s reference_handle=0x%04x options=%s", o->path, o->spec->handle, options);
            if (o->kind == OBJ_CHAR && o->spec->handle == 0x03) {
                /* Capture frame 536: observed control value, semantics unknown. */
                static const guint8 value[] = {4,0,5,0,1,1,0};
                guint16 offset = 0;
                g_variant_lookup(opts, "offset", "q", &offset);
                if (offset > sizeof(value)) {
                    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset", "Offset exceeds the observed value");
                    return;
                }
                engine_log(e, "INFO", "gatt_read_response", "Observed control value 04 00 05 00 01 01 00; offset=%u", offset);
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)",
                    g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value + offset, sizeof(value) - offset, 1)));
                return;
            }
            engine_log(e, "INFO", "gatt_read_response", "NotSupported: value semantics are not implemented");
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported", "Probe has no implemented value for this attribute");
            return;
        }
        if (g_str_equal(method, "WriteValue")) {
            g_autoptr(GVariant) bytes = g_variant_get_child_value(parameters, 0);
            g_autoptr(GVariant) opts = g_variant_get_child_value(parameters, 1);
            gsize length;
            const guint8 *value = g_variant_get_fixed_array(bytes, &length, 1);
            g_autofree char *hex = jc_hex(value, length);
            options = g_variant_print(opts, TRUE);
            engine_log(e, "INFO", "gatt_write", "path=%s reference_handle=0x%04x value=%s options=%s", o->path, o->spec->handle, hex, options);
            g_autofree char *command = o->kind == OBJ_CHAR ? jc_describe_command(o->spec->handle, value, length) : NULL;
            if (command) engine_log(e, "INFO", "nintendo_command", "%s", command);
            engine_log(e, "DEBUG", "gatt_write_ack", "Observation only; no Nintendo response and no state mutation");
            g_dbus_method_invocation_return_value(invocation, NULL);
            return;
        }
        if (g_str_equal(method, "StartNotify") || g_str_equal(method, "StopNotify")) {
            o->notifying = g_str_equal(method, "StartNotify");
            engine_log(e, "INFO", "notification_subscription", "uuid=%s enabled=%s; BlueZ owns CCCD, peer and raw CCCD bytes unavailable here",
                       o->spec->uuid, o->notifying ? "true" : "false");
            GVariantBuilder changed;
            g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&changed, "{sv}", "Notifying", g_variant_new_boolean(o->notifying));
            g_dbus_connection_emit_signal(b->bus, NULL, o->path, PROPS, "PropertiesChanged",
                g_variant_new("(sa{sv}as)", CHARACTERISTIC, &changed, NULL), NULL);
            g_dbus_method_invocation_return_value(invocation, NULL);
            return;
        }
    }
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported", "Unsupported probe operation");
}

static const GDBusInterfaceVTable vtable = { .method_call = method_call, .get_property = get_property };

static void object_free(gpointer data)
{
    Object *o = data;
    if (o->registration) g_dbus_connection_unregister_object(o->backend->bus, o->registration);
    g_free(o->path);
    g_free(o);
}

static gboolean export_object(Bluez *b, ObjectKind kind, const char *path, guint service,
                              const JcCharacteristic *spec, GError **error)
{
    Object *o = g_new0(Object, 1);
    const char *interfaces[] = {OM, AD, SERVICE, CHARACTERISTIC, DESCRIPTOR};
    o->backend = b;
    o->kind = kind;
    o->path = g_strdup(path);
    o->service = service;
    o->spec = spec;
    o->interface = interfaces[kind];
    o->registration = g_dbus_connection_register_object(b->bus, path,
        g_dbus_node_info_lookup_interface(b->xml, o->interface), &vtable, o, NULL, error);
    if (!o->registration) { object_free(o); return FALSE; }
    g_ptr_array_add(b->objects, o);
    return TRUE;
}

static void observe_peer(Bluez *b, const char *path, GVariant *properties, gboolean initial)
{
    if (!g_str_has_prefix(path, b->adapter_path) || path[strlen(b->adapter_path)] != '/') return;
    Engine *e = b->engine;
    JsonObject *peer = g_hash_table_lookup(e->peers, path);
    gboolean connected = FALSE, has_connected = g_variant_lookup(properties, "Connected", "b", &connected);
    if (!peer && (!has_connected || !connected)) return;
    if (!peer) {
        peer = json_object_new();
        json_object_set_string_member(peer, "path", path);
        json_object_set_boolean_member(peer, "connected", FALSE);
        json_object_set_boolean_member(peer, "console_identity_verified", FALSE);
        json_object_set_boolean_member(peer, "first_seen_in_snapshot", initial);
        g_hash_table_insert(e->peers, g_strdup(path), peer);
    }
    const char *address, *type;
    if (g_variant_lookup(properties, "Address", "&s", &address)) json_object_set_string_member(peer, "address", address);
    else if (!json_object_has_member(peer, "address")) {
        const char *dev = strstr(path, "/dev_");
        if (dev) {
            g_autofree char *derived = g_strdup(dev + 5);
            g_strdelimit(derived, "_", ':');
            json_object_set_string_member(peer, "address", derived);
        }
    }
    if (g_variant_lookup(properties, "AddressType", "&s", &type)) json_object_set_string_member(peer, "address_type", type);
    const char *alias;
    if (g_variant_lookup(properties, "Alias", "&s", &alias)) json_object_set_string_member(peer, "alias", alias);
    gboolean paired;
    if (g_variant_lookup(properties, "Paired", "b", &paired)) json_object_set_boolean_member(peer, "bluez_paired", paired);
    gint16 rssi;
    if (g_variant_lookup(properties, "RSSI", "n", &rssi)) json_object_set_int_member(peer, "rssi", rssi);
    if (has_connected) {
        gboolean previous = json_object_get_boolean_member(peer, "connected");
        json_object_set_boolean_member(peer, "connected", connected);
        if (previous != connected)
            engine_log(e, "INFO", connected ? (initial ? "peer_already_connected" : "peer_connected") : "peer_disconnected",
                "path=%s initial=%s advertising_registered=%s; peer is not proven to be the Switch; HCI reason/interval/encryption require btmon",
                path, initial ? "true" : "false", e->advertising ? "true" : "false");
        if (!connected) g_hash_table_remove(e->peers, path);
    }
}

static void signal_received(GDBusConnection *connection G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
    const char *path, const char *interface G_GNUC_UNUSED, const char *signal, GVariant *parameters, gpointer data)
{
    Bluez *b = data;
    if (g_str_equal(signal, "PropertiesChanged")) {
        const char *changed_interface;
        g_autoptr(GVariant) properties = NULL;
        g_autoptr(GVariant) invalidated = NULL;
        g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &properties, &invalidated);
        if (g_str_equal(changed_interface, DEVICE)) observe_peer(b, path, properties, FALSE);
        if (g_str_equal(path, b->adapter_path) && g_str_equal(changed_interface, ADAPTER)) {
            g_autofree char *values = g_variant_print(properties, TRUE);
            engine_log(b->engine, "INFO", "adapter_changed", "%s", values);
            gboolean powered = TRUE;
            if (g_variant_lookup(properties, "Powered", "b", &powered) && !powered) {
                b->engine->advertising = FALSE;
                engine_error(b->engine, "Adapter powered off; stop and start after powering it on");
            }
        }
    } else if (g_str_equal(signal, "InterfacesAdded")) {
        const char *object_path;
        g_autoptr(GVariant) interfaces = NULL;
        g_variant_get(parameters, "(&o@a{sa{sv}})", &object_path, &interfaces);
        g_autoptr(GVariant) properties = g_variant_lookup_value(interfaces, DEVICE, G_VARIANT_TYPE_VARDICT);
        if (properties) observe_peer(b, object_path, properties, FALSE);
    } else if (g_str_equal(signal, "InterfacesRemoved")) {
        const char *object_path;
        g_autoptr(GVariant) removed = NULL;
        g_variant_get(parameters, "(&o@as)", &object_path, &removed);
        if (g_hash_table_remove(b->engine->peers, object_path))
            engine_log(b->engine, "INFO", "peer_removed", "%s", object_path);
        if (g_str_equal(object_path, b->adapter_path)) {
            b->engine->advertising = b->engine->gatt = FALSE;
            engine_error(b->engine, "Bluetooth adapter removed");
        }
    } else if (g_str_equal(signal, "NameOwnerChanged")) {
        const char *name, *old_owner, *new_owner;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        (void)name; (void)old_owner;
        b->engine->advertising = b->engine->gatt = FALSE;
        g_hash_table_remove_all(b->engine->peers);
        engine_error(b->engine, *new_owner ? "BlueZ owner changed; start again to re-register" : "BlueZ stopped");
    }
}

static gboolean inspect_adapter(Bluez *b, GError **error)
{
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(b->bus, "org.bluez", "/", OM, "GetManagedObjects",
        NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
    if (!reply) return FALSE;
    g_autoptr(GVariant) objects = g_variant_get_child_value(reply, 0);
    g_autoptr(GVariant) adapter = g_variant_lookup_value(objects, b->adapter_path, G_VARIANT_TYPE("a{sa{sv}}"));
    if (!adapter) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Adapter %s not found", b->adapter_path);
        return FALSE;
    }
    g_autoptr(GVariant) properties = g_variant_lookup_value(adapter, ADAPTER, G_VARIANT_TYPE_VARDICT);
    g_autoptr(GVariant) manager = g_variant_lookup_value(adapter, ADV_MANAGER, G_VARIANT_TYPE_VARDICT);
    g_autoptr(GVariant) gatt = g_variant_lookup_value(adapter, GATT_MANAGER, G_VARIANT_TYPE_VARDICT);
    gboolean powered = FALSE, discoverable = FALSE;
    if (!properties || !manager || !gatt) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Adapter requires BlueZ LEAdvertisingManager1 and GattManager1");
        return FALSE;
    }
    g_variant_lookup(properties, "Powered", "b", &powered);
    g_variant_lookup(properties, "Discoverable", "b", &discoverable);
    const char *address = "unknown", *type = "unknown";
    g_variant_lookup(properties, "Address", "&s", &address);
    g_variant_lookup(properties, "AddressType", "&s", &type);
    g_free(b->engine->address);
    g_free(b->engine->address_type);
    b->engine->address = g_strdup(address);
    b->engine->address_type = g_strdup(type);
    g_autofree char *caps = g_variant_print(manager, TRUE);
    engine_log(b->engine, "INFO", "adapter_initialized", "adapter=%s address=%s type=%s powered=%s discoverable=%s capabilities=%s",
        b->adapter_path, address, type, powered ? "true" : "false", discoverable ? "true" : "false", caps);
    if (!powered || discoverable) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, !powered ?
            "Adapter is off; power it on explicitly before starting" :
            "Adapter is globally discoverable; turn global discoverability off to let BlueZ generate Flags 0x06");
        return FALSE;
    }
    GVariantIter iter;
    const char *path;
    GVariant *interfaces;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        g_autoptr(GVariant) device = g_variant_lookup_value(interfaces, DEVICE, G_VARIANT_TYPE_VARDICT);
        if (device) observe_peer(b, path, device, TRUE);
        g_variant_unref(interfaces);
    }
    return TRUE;
}

static void register_advertisement(Bluez *b);
static void unregister_gatt(Bluez *b);

static void advertisement_registered(GObject *source, GAsyncResult *result, gpointer data)
{
    Bluez *b = data;
    b->pending--;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (b->closing) return;
    b->engine->busy = FALSE;
    if (!reply) {
        guint peers = g_hash_table_size(b->engine->peers);
        g_autofree char *message = g_strdup_printf("%s%s", error->message, peers ?
            ". Existing adapter peers may block connectable advertising on this controller. Check peer addresses and btmon; disconnect only the intended test peer, then retry Sync." :
            ". Capture btmon and the Bluetooth journal for the underlying management/HCI status.");
        engine_error(b->engine, message);
        engine_log(b->engine, "INFO", "advertising_failure_cleanup", "Removing probe GATT services; existing peer links are preserved");
        b->engine->busy = TRUE;
        unregister_gatt(b);
        return;
    }
    b->engine->advertising = TRUE;
    engine_log(b->engine, "INFO", "advertising_started", "BlueZ registered connectable discovery advertising; validate actual bytes/address using btmon");
}

static void register_advertisement(Bluez *b)
{
    b->pending++;
    g_dbus_connection_call(b->bus, "org.bluez", b->adapter_path, ADV_MANAGER, "RegisterAdvertisement",
        g_variant_new("(oa{sv})", ADV, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, b->cancel, advertisement_registered, b);
}

static void application_registered(GObject *source, GAsyncResult *result, gpointer data)
{
    Bluez *b = data;
    b->pending--;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (b->closing) return;
    if (!reply) { engine_error(b->engine, error->message); return; }
    b->engine->gatt = TRUE;
    engine_log(b->engine, "INFO", "gatt_registered", "Vendor UUIDs exported; handles allocated by BlueZ, not identical to real Joy-Con");
    register_advertisement(b);
}

static void application_unregistered(GObject *source, GAsyncResult *result, gpointer data)
{
    Bluez *b = data;
    b->pending--;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (b->closing) return;
    b->engine->busy = FALSE;
    if (!reply) { engine_error(b->engine, error->message); return; }
    b->engine->gatt = FALSE;
    engine_log(b->engine, "INFO", "gatt_unregistered", "Probe services removed");
}

static void unregister_gatt(Bluez *b)
{
    if (!b->engine->gatt) { b->engine->busy = FALSE; return; }
    b->pending++;
    g_dbus_connection_call(b->bus, "org.bluez", b->adapter_path, GATT_MANAGER, "UnregisterApplication",
        g_variant_new("(o)", APP), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, b->cancel, application_unregistered, b);
}

static void advertisement_unregistered(GObject *source, GAsyncResult *result, gpointer data)
{
    Bluez *b = data;
    b->pending--;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (b->closing) return;
    if (!reply) { engine_error(b->engine, error->message); return; }
    b->engine->advertising = FALSE;
    engine_log(b->engine, "INFO", "advertising_stopped", "Advertisement removed; existing adapter connections are not forcibly disconnected");
    unregister_gatt(b);
}

void bluez_start(Bluez *b)
{
    if (b->engine->advertising) return;
    g_autoptr(GError) error = NULL;
    if (!inspect_adapter(b, &error)) { engine_error(b->engine, error->message); return; }
    b->engine->busy = TRUE;
    g_autofree char *hex = jc_hex(jc_advertisement, sizeof(jc_advertisement));
    engine_log(b->engine, "INFO", "advertisement_requested", "reference_adv_data=%s (31 bytes); ADV_IND; requested_interval_ms=20; BlueZ may reorder AD structures; no name, UUID list, appearance or TX power", hex);
    if (b->engine->gatt) { register_advertisement(b); return; }
    b->pending++;
    g_dbus_connection_call(b->bus, "org.bluez", b->adapter_path, GATT_MANAGER, "RegisterApplication",
        g_variant_new("(oa{sv})", APP, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, b->cancel, application_registered, b);
}

void bluez_stop(Bluez *b)
{
    b->engine->busy = TRUE;
    if (!b->engine->advertising) { unregister_gatt(b); return; }
    b->pending++;
    g_dbus_connection_call(b->bus, "org.bluez", b->adapter_path, ADV_MANAGER, "UnregisterAdvertisement",
        g_variant_new("(o)", ADV), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, b->cancel, advertisement_unregistered, b);
}

static void peer_disconnected(GObject *source, GAsyncResult *result, gpointer data)
{
    Bluez *b = data;
    b->pending--;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (b->closing) return;
    b->engine->busy = FALSE;
    if (!reply) engine_error(b->engine, error->message);
    else {
        engine_log(b->engine, "INFO", "disconnect_completed", "path=%s; BlueZ completed the explicit disconnect request", b->disconnect_path);
        g_hash_table_remove(b->engine->peers, b->disconnect_path);
    }
    g_clear_pointer(&b->disconnect_path, g_free);
}

void bluez_disconnect(Bluez *b, const char *path)
{
    b->engine->busy = TRUE;
    b->disconnect_path = g_strdup(path);
    b->pending++;
    g_dbus_connection_call(b->bus, "org.bluez", path, DEVICE, "Disconnect", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 10000, b->cancel, peer_disconnected, b);
}

Bluez *bluez_new(Engine *engine, GError **error)
{
    g_autoptr(GError) inspect_error = NULL;
    Bluez *b = g_new0(Bluez, 1);
    b->engine = engine;
    b->cancel = g_cancellable_new();
    b->objects = g_ptr_array_new_with_free_func(object_free);
    b->adapter_path = g_strdup_printf("/org/bluez/%s", engine->adapter_name);
    g_autofree char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    if (!address) goto fail;
    b->bus = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, error);
    if (!b->bus) goto fail;
    g_dbus_connection_set_exit_on_close(b->bus, FALSE);
    b->xml = g_dbus_node_info_new_for_xml(xml, error);
    if (!b->xml) goto fail;
    if (!export_object(b, OBJ_ROOT, APP, 0, NULL, error) || !export_object(b, OBJ_ADV, ADV, 0, NULL, error)) goto fail;
    for (guint i = 0; i < 2; i++) {
        g_autofree char *path = g_strdup_printf(APP "/service%u", i);
        if (!export_object(b, OBJ_SERVICE, path, i, NULL, error)) goto fail;
    }
    for (gsize i = 0; i < jc_characteristic_count; i++) {
        const JcCharacteristic *spec = &jc_characteristics[i];
        g_autofree char *path = g_strdup_printf(APP "/service%u/char%04x", spec->service, spec->handle);
        if (!export_object(b, OBJ_CHAR, path, spec->service, spec, error)) goto fail;
        if (spec->descriptor) {
            g_autofree char *desc = g_strconcat(path, "/descriptor", NULL);
            if (!export_object(b, OBJ_DESC, desc, spec->service, spec, error)) goto fail;
        }
    }
    b->changed_subscription = g_dbus_connection_signal_subscribe(b->bus, "org.bluez", PROPS, "PropertiesChanged", NULL, NULL, 0, signal_received, b, NULL);
    b->added_subscription = g_dbus_connection_signal_subscribe(b->bus, "org.bluez", OM, "InterfacesAdded", NULL, NULL, 0, signal_received, b, NULL);
    b->removed_subscription = g_dbus_connection_signal_subscribe(b->bus, "org.bluez", OM, "InterfacesRemoved", NULL, NULL, 0, signal_received, b, NULL);
    b->owner_subscription = g_dbus_connection_signal_subscribe(b->bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", "org.bluez", 0, signal_received, b, NULL);
    /* Read-only inspection at startup; failures remain visible and can be retried. */
    if (!inspect_adapter(b, &inspect_error)) engine_error(engine, inspect_error->message);
    return b;
fail:
    bluez_free(b);
    return NULL;
}

void bluez_free(Bluez *b)
{
    if (!b) return;
    b->closing = TRUE;
    g_cancellable_cancel(b->cancel);
    while (b->pending) g_main_context_iteration(NULL, TRUE);
    if (b->bus) {
        guint subscriptions[] = {b->changed_subscription, b->added_subscription, b->removed_subscription, b->owner_subscription};
        for (guint i = 0; i < G_N_ELEMENTS(subscriptions); i++)
            if (subscriptions[i]) g_dbus_connection_signal_unsubscribe(b->bus, subscriptions[i]);
    }
    g_ptr_array_unref(b->objects);
    if (b->bus) {
        /* A private bus connection owns all registrations; close releases them even on errors. */
        g_dbus_connection_close_sync(b->bus, NULL, NULL);
        g_object_unref(b->bus);
    }
    g_clear_pointer(&b->xml, g_dbus_node_info_unref);
    g_object_unref(b->cancel);
    g_free(b->adapter_path);
    g_free(b->disconnect_path);
    g_free(b);
}
