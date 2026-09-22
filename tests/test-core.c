#include "core.h"

static JsonObject *request(Engine *e, const char *text)
{
    g_autofree char *reply = engine_request(e, text);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, reply, -1, NULL));
    return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static void invalid_requests(void)
{
    Engine *e = engine_new("hci0", TRUE, FALSE);
    const char *invalid[] = {"", "null", "[]", "{", "{}", "{\"version\":2,\"method\":\"start\"}",
        "{\"version\":1,\"method\":false}", "{\"version\":1.0,\"method\":\"start\"}",
        "{\"version\":1,\"method\":\"logs\",\"after\":-1}",
        "{\"version\":1,\"method\":\"logs\",\"after\":\"1\"}", "{\"version\":1,\"method\":\"button\"}"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(JsonObject) reply = request(e, invalid[i]);
        g_assert_false(json_object_get_boolean_member(reply, "ok"));
        g_assert_false(e->advertising);
    }
    engine_free(e);
}

static void lifecycle(void)
{
    Engine *e = engine_new("hci0", TRUE, FALSE);
    g_autoptr(JsonObject) start = request(e, "{\"version\":1,\"method\":\"start\"}");
    g_assert_true(json_object_get_boolean_member(start, "ok"));
    g_assert_true(e->advertising);
    e->busy = TRUE;
    g_autoptr(JsonObject) rejected = request(e, "{\"version\":1,\"method\":\"stop\"}");
    g_assert_false(json_object_get_boolean_member(rejected, "ok"));
    g_assert_true(e->advertising);
    e->busy = FALSE;
    g_autoptr(JsonObject) stop = request(e, "{\"version\":1,\"method\":\"stop\"}");
    g_assert_true(json_object_get_boolean_member(stop, "ok"));
    g_assert_false(e->advertising);
    engine_free(e);
}

static void log_cursor(void)
{
    Engine *e = engine_new("hci0", TRUE, FALSE);
    for (guint i = 0; i < 1002; i++) engine_log(e, "DEBUG", "sample", "%u", i);
    g_assert_cmpuint(e->events.length, ==, 1000);
    g_autoptr(JsonObject) reply = request(e, "{\"version\":1,\"method\":\"logs\",\"after\":1000}");
    JsonObject *result = json_object_get_object_member(reply, "result");
    g_assert_cmpint(json_object_get_int_member(result, "oldest"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(result, "cursor"), ==, 1002);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(result, "events")), ==, 2);
    engine_free(e);
}

static void targeted_disconnect(void)
{
    Engine *e = engine_new("hci0", TRUE, FALSE);
    const char *addresses[] = {"12:34:56:78:9A:BC", "22:34:56:78:9A:BC"};
    for (guint i = 0; i < G_N_ELEMENTS(addresses); i++) {
        JsonObject *peer = json_object_new();
        json_object_set_string_member(peer, "address", addresses[i]);
        json_object_set_boolean_member(peer, "connected", TRUE);
        g_hash_table_insert(e->peers, g_strdup(addresses[i]), peer);
    }
    const char *invalid[] = {
        "{\"version\":1,\"method\":\"disconnect\"}",
        "{\"version\":1,\"method\":\"disconnect\",\"address\":false}",
        "{\"version\":1,\"method\":\"disconnect\",\"address\":\"all\"}",
        "{\"version\":1,\"method\":\"disconnect\",\"address\":\"32:34:56:78:9A:BC\"}"
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(JsonObject) reply = request(e, invalid[i]);
        g_assert_false(json_object_get_boolean_member(reply, "ok"));
        g_assert_cmpuint(g_hash_table_size(e->peers), ==, 2);
    }
    const char *valid = "{\"version\":1,\"method\":\"disconnect\",\"address\":\"12:34:56:78:9a:bc\"}";
    e->busy = TRUE;
    g_autoptr(JsonObject) busy = request(e, valid);
    g_assert_false(json_object_get_boolean_member(busy, "ok"));
    g_assert_cmpuint(g_hash_table_size(e->peers), ==, 2);
    e->busy = FALSE;
    g_autoptr(JsonObject) reply = request(e, valid);
    g_assert_true(json_object_get_boolean_member(reply, "ok"));
    g_assert_cmpuint(g_hash_table_size(e->peers), ==, 1);
    g_assert_true(g_hash_table_contains(e->peers, addresses[1]));
    g_autoptr(JsonObject) absent = request(e, valid);
    g_assert_false(json_object_get_boolean_member(absent, "ok"));
    engine_free(e);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/core/invalid-requests", invalid_requests);
    g_test_add_func("/core/lifecycle", lifecycle);
    g_test_add_func("/core/log-cursor", log_cursor);
    g_test_add_func("/core/targeted-disconnect", targeted_disconnect);
    return g_test_run();
}
