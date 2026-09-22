/* Isolated D-Bus fixture: an existing peer blocks advertising until explicitly disconnected. */
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <string.h>

#define ADAPTER_PATH "/org/bluez/hci0"
#define PEER_PATH ADAPTER_PATH "/dev_12_34_56_78_9A_BC"
static gboolean connected = TRUE, gatt = FALSE, advertising = FALSE;

static const char xml[] =
"<node>"
"<interface name='org.freedesktop.DBus.ObjectManager'>"
"<method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
"</interface>"
"<interface name='org.bluez.GattManager1'>"
"<method name='RegisterApplication'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='UnregisterApplication'><arg type='o' direction='in'/></method>"
"</interface>"
"<interface name='org.bluez.LEAdvertisingManager1'>"
"<method name='RegisterAdvertisement'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='UnregisterAdvertisement'><arg type='o' direction='in'/></method>"
"</interface>"
"<interface name='org.bluez.Device1'><method name='Disconnect'/></interface>"
"<interface name='io.github.wozt.TestBluez'>"
"<method name='GetState'><arg type='b' direction='out'/><arg type='b' direction='out'/><arg type='b' direction='out'/></method>"
"</interface>"
"</node>";

static void call(GDBusConnection *bus, const char *sender G_GNUC_UNUSED,
                 const char *path G_GNUC_UNUSED, const char *interface G_GNUC_UNUSED,
                 const char *method, GVariant *parameters G_GNUC_UNUSED,
                 GDBusMethodInvocation *invocation, gpointer data G_GNUC_UNUSED)
{
    if (g_str_equal(method, "GetState")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(bbb)", connected, gatt, advertising));
        return;
    }
    if (g_str_equal(method, "GetManagedObjects")) {
        GVariantBuilder objects, interfaces, props;
        g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&props, "{sv}", "Powered", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&props, "{sv}", "Discoverable", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&props, "{sv}", "Address", g_variant_new_string("00:11:22:33:44:55"));
        g_variant_builder_add(&props, "{sv}", "AddressType", g_variant_new_string("public"));
        g_variant_builder_add(&interfaces, "{sa{sv}}", "org.bluez.Adapter1", &props);
        g_variant_builder_add(&interfaces, "{sa{sv}}", "org.bluez.GattManager1", NULL);
        g_variant_builder_add(&interfaces, "{sa{sv}}", "org.bluez.LEAdvertisingManager1", NULL);
        g_variant_builder_add(&objects, "{oa{sa{sv}}}", ADAPTER_PATH, &interfaces);
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&props, "{sv}", "Connected", g_variant_new_boolean(connected));
        g_variant_builder_add(&props, "{sv}", "Address", g_variant_new_string("12:34:56:78:9A:BC"));
        g_variant_builder_add(&props, "{sv}", "Alias", g_variant_new_string("Test peer"));
        g_variant_builder_add(&interfaces, "{sa{sv}}", "org.bluez.Device1", &props);
        g_variant_builder_add(&objects, "{oa{sa{sv}}}", PEER_PATH, &interfaces);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{oa{sa{sv}}})", &objects));
        return;
    }
    if (g_str_equal(method, "RegisterApplication")) gatt = TRUE;
    else if (g_str_equal(method, "UnregisterApplication")) gatt = FALSE;
    else if (g_str_equal(method, "RegisterAdvertisement")) {
        if (connected) {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "Failed to register advertisement");
            return;
        }
        advertising = TRUE;
    } else if (g_str_equal(method, "UnregisterAdvertisement")) advertising = FALSE;
    else if (g_str_equal(method, "Disconnect")) {
        connected = FALSE;
        GVariantBuilder changed;
        g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&changed, "{sv}", "Connected", g_variant_new_boolean(FALSE));
        g_dbus_connection_emit_signal(bus, NULL, PEER_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
            g_variant_new("(sa{sv}as)", "org.bluez.Device1", &changed, NULL), NULL);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static gboolean quit(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_CONTINUE;
}

int main(void)
{
    static const GDBusInterfaceVTable vtable = {.method_call = call};
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error(error);
    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, &error);
    g_assert_no_error(error);
    const char *paths[] = {"/", ADAPTER_PATH, ADAPTER_PATH, PEER_PATH, "/"};
    for (guint i = 0; i < G_N_ELEMENTS(paths); i++) {
        g_dbus_connection_register_object(bus, paths[i], info->interfaces[i], &vtable, NULL, NULL, &error);
        g_assert_no_error(error);
    }
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.bluez", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    g_assert_no_error(error);
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    guint sig = g_unix_signal_add(SIGTERM, quit, loop);
    g_main_loop_run(loop);
    g_source_remove(sig);
    return 0;
}
