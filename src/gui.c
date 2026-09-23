#include "client.h"
#include <adwaita.h>

typedef struct {
    GtkWindow *window;
    AdwActionRow *adapter, *state, *connection;
    GtkLabel *error;
    GtkTextBuffer *logs;
    GtkWidget *start, *stop, *sync, *disconnect;
    char *socket_path, *peer_address;
    gint64 cursor;
    gboolean busy, closed;
    guint timer;
    gint refs;
} Ui;

typedef struct {
    char *path, *method, *address;
    gint64 cursor;
} Request;

typedef struct {
    JsonObject *status, *logs;
} Snapshot;

static void ui_unref(Ui *ui)
{
    if (--ui->refs) return;
    g_free(ui->socket_path);
    g_free(ui->peer_address);
    g_free(ui);
}

static void request_free(gpointer data)
{
    Request *r = data;
    g_free(r->path);
    g_free(r->method);
    g_free(r->address);
    g_free(r);
}

static void snapshot_free(gpointer data)
{
    Snapshot *s = data;
    if (s->status) json_object_unref(s->status);
    if (s->logs) json_object_unref(s->logs);
    g_free(s);
}

static JsonObject *get_result(const char *path, const char *method, gint64 cursor, const char *address, GError **error)
{
    g_autoptr(JsonObject) response = jc_client_request_full(path, method, cursor, address, error);
    if (!response) return NULL;
    if (!json_object_get_boolean_member_with_default(response, "ok", FALSE)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
            json_object_get_string_member_with_default(response, "error", "Daemon rejected request"));
        return NULL;
    }
    JsonNode *result = json_object_get_member(response, "result");
    if (!result || !JSON_NODE_HOLDS_OBJECT(result)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Missing daemon result");
        return NULL;
    }
    return json_object_ref(json_node_get_object(result));
}

static void worker(GTask *task, gpointer source G_GNUC_UNUSED, gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    Request *r = task_data;
    g_autoptr(GError) error = NULL;
    Snapshot *s = g_new0(Snapshot, 1);
    s->status = get_result(r->path, r->method, 0, r->address, &error);
    if (s->status) s->logs = get_result(r->path, "logs", r->cursor, NULL, &error);
    if (error) {
        snapshot_free(s);
        g_task_return_error(task, g_steal_pointer(&error));
    } else g_task_return_pointer(task, s, snapshot_free);
}

static void update_complete(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
    Ui *ui = data;
    g_autoptr(GError) error = NULL;
    Snapshot *s = g_task_propagate_pointer(G_TASK(result), &error);
    ui->busy = FALSE;
    if (ui->closed) { if (s) snapshot_free(s); ui_unref(ui); return; }
    gtk_widget_set_sensitive(ui->start, TRUE);
    gtk_widget_set_sensitive(ui->stop, TRUE);
    gtk_widget_set_sensitive(ui->sync, TRUE);
    if (error) {
        gtk_widget_set_sensitive(ui->disconnect, FALSE);
        gtk_label_set_text(ui->error, error->message);
        adw_action_row_set_subtitle(ui->state, "Daemon unavailable or request failed");
        adw_action_row_set_subtitle(ui->connection, "Unknown");
        ui_unref(ui);
        return;
    }
    const char *backend_error = json_object_get_string_member_with_default(s->status, "error", "");
    gtk_label_set_text(ui->error, backend_error ? backend_error : "");
    const char *address = json_object_get_string_member_with_default(s->status, "local_address", "No address");
    const char *adapter = json_object_get_string_member_with_default(s->status, "adapter", "Unknown");
    gboolean simulated = json_object_get_boolean_member_with_default(s->status, "simulated", FALSE);
    g_autofree char *adapter_text = g_strdup_printf("%s · %s%s", adapter, address ? address : "No address", simulated ? " · Simulation" : "");
    adw_action_row_set_subtitle(ui->adapter, adapter_text);
    const char *state = json_object_get_string_member_with_default(s->status, "state", "unknown");
    adw_action_row_set_subtitle(ui->state, state);
    gboolean transitioning = g_str_equal(state, "transitioning");
    gboolean advertising = json_object_get_boolean_member_with_default(s->status, "advertising_registered", FALSE);
    gtk_widget_set_sensitive(ui->start, !transitioning && !advertising);
    gtk_widget_set_sensitive(ui->sync, !transitioning && !advertising);
    gtk_widget_set_sensitive(ui->stop, !transitioning);
    JsonArray *peers = json_object_get_array_member(s->status, "peers");
    guint count = peers ? json_array_get_length(peers) : 0;
    g_clear_pointer(&ui->peer_address, g_free);
    g_autoptr(GString) connection = g_string_new(count ? "" : "Disconnected");
    for (guint i = 0; i < count; i++) {
        JsonObject *peer = json_array_get_object_element(peers, i);
        const char *peer_address = json_object_get_string_member_with_default(peer, "address", "Unknown address");
        gboolean snapshot = json_object_get_boolean_member_with_default(peer, "first_seen_in_snapshot", FALSE);
        g_string_append_printf(connection, "%s%s · %s", i ? "\n" : "", peer_address,
                               snapshot ? "Already connected when observed" : "Connection observed by daemon");
        if (count == 1 && json_object_has_member(peer, "address")) ui->peer_address = g_strdup(peer_address);
    }
    if (count) g_string_append(connection, "\nConsole identity unverified");
    adw_action_row_set_subtitle(ui->connection, connection->str);
    gtk_widget_set_sensitive(ui->disconnect, !transitioning && ui->peer_address != NULL);
    g_autofree char *disconnect_tip = ui->peer_address ? g_strdup_printf("Disconnect only %s; this does not forget pairing", ui->peer_address) :
        g_strdup("With multiple peers, use the CLI to choose an explicit address");
    gtk_widget_set_tooltip_text(ui->disconnect, disconnect_tip);
    JsonArray *events = json_object_get_array_member(s->logs, "events");
    gint64 cursor = json_object_get_int_member(s->logs, "cursor");
    if (cursor < ui->cursor) gtk_text_buffer_set_text(ui->logs, "Daemon restarted; log cursor reset.\n", -1);
    if (events) for (guint i = 0; i < json_array_get_length(events); i++) {
        JsonObject *event = json_array_get_object_element(events, i);
        g_autofree char *line = g_strdup_printf("%s  %-5s  %s\n%s\n\n",
            json_object_get_string_member(event, "time"), json_object_get_string_member(event, "level"),
            json_object_get_string_member(event, "event"), json_object_get_string_member(event, "message"));
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(ui->logs, &end);
        gtk_text_buffer_insert(ui->logs, &end, line, -1);
    }
    ui->cursor = cursor;
    gint lines = gtk_text_buffer_get_line_count(ui->logs);
    if (lines > 3000) {
        GtkTextIter start, cutoff;
        gtk_text_buffer_get_start_iter(ui->logs, &start);
        gtk_text_buffer_get_iter_at_line(ui->logs, &cutoff, lines - 3000);
        gtk_text_buffer_delete(ui->logs, &start, &cutoff);
    }
    snapshot_free(s);
    ui_unref(ui);
}

static void request_update(Ui *ui, const char *method)
{
    if (ui->busy || ui->closed) return;
    if (g_str_equal(method, "disconnect") && !ui->peer_address) return;
    ui->busy = TRUE;
    ui->refs++;
    Request *r = g_new0(Request, 1);
    r->path = g_strdup(ui->socket_path);
    r->method = g_strdup(method);
    if (g_str_equal(method, "disconnect")) r->address = g_strdup(ui->peer_address);
    r->cursor = ui->cursor;
    gtk_widget_set_sensitive(ui->start, FALSE);
    gtk_widget_set_sensitive(ui->stop, FALSE);
    gtk_widget_set_sensitive(ui->sync, FALSE);
    gtk_widget_set_sensitive(ui->disconnect, FALSE);
    g_autoptr(GTask) task = g_task_new(NULL, NULL, update_complete, ui);
    g_task_set_task_data(task, r, request_free);
    g_task_run_in_thread(task, worker);
}

static gboolean poll_status(gpointer data)
{
    request_update(data, "status");
    return G_SOURCE_CONTINUE;
}

static void action_clicked(GtkButton *button, gpointer data)
{
    request_update(data, g_object_get_data(G_OBJECT(button), "method"));
}

static void copy_logs(GtkButton *button G_GNUC_UNUSED, gpointer data)
{
    Ui *ui = data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(ui->logs, &start, &end);
    g_autofree char *text = gtk_text_buffer_get_text(ui->logs, &start, &end, FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(ui->window)), text);
}

static gboolean close_window(GtkWindow *window G_GNUC_UNUSED, gpointer data)
{
    Ui *ui = data;
    ui->closed = TRUE;
    g_source_remove(ui->timer);
    ui_unref(ui);
    return FALSE;
}

static AdwActionRow *add_row(AdwPreferencesGroup *group, const char *title)
{
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, "Waiting for daemon");
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return row;
}

static void activate(GtkApplication *application, gpointer data G_GNUC_UNUSED)
{
    GtkWindow *existing = gtk_application_get_active_window(application);
    if (existing) { gtk_window_present(existing); return; }
    Ui *ui = g_new0(Ui, 1);
    ui->refs = 1;
    ui->socket_path = jc_socket_path();
    ui->window = GTK_WINDOW(adw_application_window_new(application));
    gtk_window_set_title(ui->window, "Joy-Con 2 Lab");
    gtk_window_set_default_size(ui->window, 760, 760);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("Joy-Con 2 Lab", "Linux BLE discovery probe"));
    gtk_box_append(GTK_BOX(outer), header);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(body, 24);
    gtk_widget_set_margin_end(body, 24);
    gtk_widget_set_margin_top(body, 12);
    gtk_widget_set_margin_bottom(body, 24);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_box_append(GTK_BOX(outer), body);
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Joy-Con 2 R");
    adw_preferences_group_set_description(group, "Discovery only. Pairing and input reports are not implemented.");
    ui->adapter = add_row(group, "Bluetooth adapter");
    ui->state = add_row(group, "Emulator state");
    ui->connection = add_row(group, "Connected adapter peers");
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(ui->connection), FALSE);
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(group));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui->start = gtk_button_new_with_label("Start advertising");
    ui->stop = gtk_button_new_with_label("Stop advertising");
    ui->sync = gtk_button_new_with_label("Sync");
    ui->disconnect = gtk_button_new_with_label("Disconnect peer");
    GtkWidget *actions[] = {ui->start, ui->stop, ui->sync, ui->disconnect};
    const char *methods[] = {"start", "stop", "sync", "disconnect"};
    for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
        g_object_set_data(G_OBJECT(actions[i]), "method", (gpointer)methods[i]);
        g_signal_connect(actions[i], "clicked", G_CALLBACK(action_clicked), ui);
        gtk_box_append(GTK_BOX(buttons), actions[i]);
    }
    gtk_widget_add_css_class(ui->start, "suggested-action");
    gtk_widget_set_tooltip_text(ui->sync, "Advertise for discovery; Nintendo pairing is not yet implemented");
    gtk_box_append(GTK_BOX(body), buttons);
    ui->error = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(ui->error, TRUE);
    gtk_label_set_xalign(ui->error, 0);
    gtk_widget_add_css_class(GTK_WIDGET(ui->error), "error");
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(ui->error));
    GtkWidget *log_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new("Live logs");
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(log_header), label);
    GtkWidget *copy = gtk_button_new_with_label("Copy logs");
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_logs), ui);
    gtk_box_append(GTK_BOX(log_header), copy);
    gtk_box_append(GTK_BOX(body), log_header);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
    ui->logs = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    gtk_widget_add_css_class(scroller, "card");
    gtk_box_append(GTK_BOX(body), scroller);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(ui->window), outer);
    g_signal_connect(ui->window, "close-request", G_CALLBACK(close_window), ui);
    ui->timer = g_timeout_add_seconds(1, poll_status, ui);
    request_update(ui, "status");
    gtk_window_present(ui->window);
}

int main(int argc, char **argv)
{
    g_autoptr(AdwApplication) application = adw_application_new("io.github.wozt.pcble2gamepad", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(application), argc, argv);
}
