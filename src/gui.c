#include "client.h"
#include "input-model.h"
#include "config.h"
#include <adwaita.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <glib/gstdio.h>

typedef struct {
    int refs;gboolean closed,closing,busy,online,connected,loading;
    GtkApplication *app;GtkWindow *window;AdwToastOverlay *toast;
    GtkStack *stack;GtkLabel *status,*peer,*error,*metrics,*source_hint,*controller_hint,*hero_title,*paired_console;
    GtkWidget *drawing,*start,*sync,*stop,*arm,*capture_hint,*secondary_row,*pro_color_panel;
    GtkDropDown *controllers,*adapters,*secondary,*devices,*source,*profiles;
    GtkStringList *adapter_names,*secondary_names,*device_names,*profile_names;
    GPtrArray *adapter_ids,*adapter_addresses,*secondary_ids,*secondary_addresses,*device_ids;
    GtkButton *key_buttons[INPUT_ACTIONS],*pad_buttons[INPUT_BUTTONS];
    GtkScale *deadzone,*sensitivity;GtkSwitch *invert[4],*swap,*background,*traffic_logs,*swap_face,*auto_select;
    GtkColorDialogButton *pro_body_color_button,*pro_button_color_button;
    GtkColorDialogButton *pro_left_grip_color_button,*pro_right_grip_color_button;
    GtkColorDialog *color_dialog;
    GdkRGBA pro_body_color,pro_button_color,pro_left_grip_color,pro_right_grip_color;
    GtkTextBuffer *logs;GHashTable *keys;
    InputProfile profile;InputFrame frame;
    SDL_GameController *pad;int joystick_count,learn_key,learn_pad;
    gboolean previous_pad[SDL_CONTROLLER_BUTTON_MAX+2];
    char *socket,*socket2,*config_dir,*profile_name,*pending,*settings_path,*preferred_gamepad_guid;
    char *preferred_adapter_address,*preferred_secondary_address;
    char *paired_switch_address,*paired_adapter_address;
    guint timer,ticks,saved_source;
    gboolean saved_arm,saved_auto_select,saved_traffic;
    GSubprocess *launcher;
    RsvgHandle *pro_svg,*joycon_left_svg,*joycon_right_svg;
} Ui;
typedef struct {char *path,*path2;JsonObject *request;} Request;
static void refresh_bindings(Ui *u);
static void request(Ui *u,const char *method);
static void devices_scan(Ui *u);
static void adapters_scan(GtkButton *b,Ui *u);
static void controller_selected(GObject *o,GParamSpec *p,Ui *u);
static void update_pro_svg_colors(Ui *u);
static void apply_profile_state(Ui *u);
static gboolean pair_mode(Ui *u){return u->controllers && gtk_drop_down_get_selected(u->controllers)==1;}
static gboolean paired_adapter_selected(Ui *u) {
    if(!u->adapters || !u->paired_adapter_address || !*u->paired_adapter_address)return FALSE;
    guint i=gtk_drop_down_get_selected(u->adapters);
    return i<u->adapter_addresses->len &&
        !g_ascii_strcasecmp(g_ptr_array_index(u->adapter_addresses,i),u->paired_adapter_address);
}
static void update_paired_console(Ui *u) {
    if(!u->paired_console)return;
    gtk_label_set_text(u->paired_console,
        u->paired_switch_address && *u->paired_switch_address
            ?u->paired_switch_address
            :"Not paired yet");
}
static void update_controls(Ui *u) {
    gboolean adapters_ok=u->adapter_ids->len>0 && (!pair_mode(u) || u->secondary_ids->len>1);
    gboolean can_reconnect=!pair_mode(u) &&
        u->paired_switch_address && *u->paired_switch_address &&
        paired_adapter_selected(u);
    gboolean reconnect_existing=
        u->online && !u->connected && !pair_mode(u) && can_reconnect;
    gboolean reconnect_new=
        !u->online && !u->launcher && can_reconnect;

    gtk_widget_set_sensitive(u->start,
        adapters_ok && (reconnect_existing || reconnect_new));
    gtk_widget_set_sensitive(u->sync,!u->online && !u->launcher && adapters_ok);
    gtk_widget_set_sensitive(u->stop,u->online);
    gtk_widget_set_sensitive(GTK_WIDGET(u->controllers),!u->online && !u->launcher);
    gtk_widget_set_sensitive(GTK_WIDGET(u->adapters),!u->online && !u->launcher);
    gtk_widget_set_sensitive(GTK_WIDGET(u->secondary),!u->online && !u->launcher);

    gboolean colors_enabled=
        !pair_mode(u) &&
        !u->online &&
        !u->launcher;

    if(u->pro_body_color_button)
        gtk_widget_set_sensitive(GTK_WIDGET(u->pro_body_color_button),colors_enabled);
    if(u->pro_button_color_button)
        gtk_widget_set_sensitive(GTK_WIDGET(u->pro_button_color_button),colors_enabled);
    if(u->pro_left_grip_color_button)
        gtk_widget_set_sensitive(GTK_WIDGET(u->pro_left_grip_color_button),colors_enabled);
    if(u->pro_right_grip_color_button)
        gtk_widget_set_sensitive(GTK_WIDGET(u->pro_right_grip_color_button),colors_enabled);
}
static void toast(Ui *u,const char *message) {adw_toast_overlay_add_toast(u->toast,adw_toast_new(message));}
static void ui_unref(Ui *u) {
    if(--u->refs)return;
    if(u->pad)SDL_GameControllerClose(u->pad);
    g_clear_object(&u->launcher);g_hash_table_unref(u->keys);
    g_ptr_array_unref(u->adapter_ids);
    g_ptr_array_unref(u->adapter_addresses);
    g_ptr_array_unref(u->secondary_ids);
    g_ptr_array_unref(u->secondary_addresses);
    g_ptr_array_unref(u->device_ids);

    g_free(u->socket);
    g_free(u->socket2);
    g_free(u->config_dir);
    g_free(u->profile_name);
    g_free(u->pending);
    g_free(u->settings_path);
    g_free(u->preferred_gamepad_guid);
    g_free(u->preferred_adapter_address);
    g_free(u->preferred_secondary_address);
    g_free(u->paired_switch_address);
    g_free(u->paired_adapter_address);

    g_clear_object(&u->color_dialog);
    g_clear_object(&u->pro_svg);
    g_clear_object(&u->joycon_left_svg);
    g_clear_object(&u->joycon_right_svg);
    g_free(u);
}
static void margin(GtkWidget *w,int m) {gtk_widget_set_margin_start(w,m);gtk_widget_set_margin_end(w,m);gtk_widget_set_margin_top(w,m);gtk_widget_set_margin_bottom(w,m);}
static GtkWidget *label(const char *s,const char *css) {GtkWidget *w=gtk_label_new(s);gtk_label_set_xalign(GTK_LABEL(w),0);gtk_label_set_wrap(GTK_LABEL(w),TRUE);if(css)gtk_widget_add_css_class(w,css);return w;}
static GtkWidget *row(AdwPreferencesGroup *g,const char *title,const char *subtitle,GtkWidget *suffix) {
    GtkWidget *r=adw_action_row_new();adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r),title);
    if(subtitle)adw_action_row_set_subtitle(ADW_ACTION_ROW(r),subtitle);
    if(suffix){gtk_widget_set_valign(suffix,GTK_ALIGN_CENTER);adw_action_row_add_suffix(ADW_ACTION_ROW(r),suffix);}
    adw_preferences_group_add(g,r);return r;
}
static AdwPreferencesGroup *group(GtkWidget *box,const char *title,const char *description) {
    AdwPreferencesGroup *g=ADW_PREFERENCES_GROUP(adw_preferences_group_new());adw_preferences_group_set_title(g,title);
    if(description)adw_preferences_group_set_description(g,description);
    gtk_box_append(GTK_BOX(box),GTK_WIDGET(g));return g;
}
static GtkWidget *page(Ui *u,const char *id,const char *title,const char *subtitle) {
    GtkWidget *scroll=gtk_scrolled_window_new(),*clamp=adw_clamp_new(),*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,24);
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp),860);margin(box,28);
    gtk_box_append(GTK_BOX(box),label(title,"title-1"));gtk_box_append(GTK_BOX(box),label(subtitle,"dim-label"));
    adw_clamp_set_child(ADW_CLAMP(clamp),box);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),clamp);
    gtk_stack_add_titled(u->stack,scroll,id,title);return box;
}
static void release_input(Ui *u) {
    g_hash_table_remove_all(u->keys);
    if(u->online)request(u,"release");
}
static void neutral(Ui *u) {
    /*
     * Neutral means "release all current buttons/sticks".
     *
     * Do not change the persistent Enable input preference here. Stopping a
     * Bluetooth session, editing a binding or reconnecting must not silently
     * turn input off.
     */
    release_input(u);
}
static char *profile_path(Ui *u,const char *name) {char *file=g_strconcat(name,".ini",NULL);char *path=g_build_filename(u->config_dir,file,NULL);g_free(file);return path;}

static char *rgba_hex(const GdkRGBA *color) {
    int r=CLAMP((int)lrint(color->red*255.0),0,255);
    int g=CLAMP((int)lrint(color->green*255.0),0,255);
    int b=CLAMP((int)lrint(color->blue*255.0),0,255);

    return g_strdup_printf("%02X%02X%02X",r,g,b);
}

static void load_color_setting(GKeyFile *file,
                               const char *key,
                               GdkRGBA *color) {
    if(!g_key_file_has_key(file,"Appearance",key,NULL))
        return;

    g_autofree char *value=
        g_key_file_get_string(file,"Appearance",key,NULL);

    GdkRGBA parsed;

    if(value && gdk_rgba_parse(&parsed,value))
        *color=parsed;
}

static void load_string_setting(GKeyFile *file,
                                const char *group,
                                const char *key,
                                char **target) {
    if(!g_key_file_has_key(file,group,key,NULL))
        return;

    g_autofree char *value=
        g_key_file_get_string(file,group,key,NULL);

    if(!value || !*value)
        return;

    g_free(*target);
    *target=g_strdup(value);
}

static void save_optional_string(GKeyFile *file,
                                 const char *group,
                                 const char *key,
                                 const char *value) {
    if(value && *value)
        g_key_file_set_string(file,group,key,value);
    else
        g_key_file_remove_key(file,group,key,NULL);
}

static void profile_state_defaults(Ui *u) {
    u->saved_source=0;
    u->saved_arm=FALSE;
    u->saved_auto_select=TRUE;
    u->saved_traffic=FALSE;

    g_clear_pointer(&u->preferred_gamepad_guid,g_free);
    g_clear_pointer(&u->preferred_adapter_address,g_free);
    g_clear_pointer(&u->preferred_secondary_address,g_free);
    g_clear_pointer(&u->paired_switch_address,g_free);
    g_clear_pointer(&u->paired_adapter_address,g_free);

    gdk_rgba_parse(&u->pro_body_color,"#828282");
    gdk_rgba_parse(&u->pro_button_color,"#0f0f0f");
    gdk_rgba_parse(&u->pro_left_grip_color,"#828282");
    gdk_rgba_parse(&u->pro_right_grip_color,"#828282");
}

static gboolean profile_state_load_path(Ui *u,const char *path) {
    if(!path || !g_file_test(path,G_FILE_TEST_EXISTS))
        return FALSE;

    g_autoptr(GKeyFile) k=g_key_file_new();

    if(!g_key_file_load_from_file(
            k,
            path,
            G_KEY_FILE_NONE,
            NULL))
        return FALSE;

    gboolean has_state=
        g_key_file_has_group(k,"Studio") ||
        g_key_file_has_group(k,"Bluetooth") ||
        g_key_file_has_group(k,"Appearance") ||
        g_key_file_has_group(k,"Diagnostics");

    if(g_key_file_has_key(k,"Studio","source",NULL)) {
        gint source=
            g_key_file_get_integer(k,"Studio","source",NULL);

        if(source>=0 && source<=1)
            u->saved_source=(guint)source;
    }

    if(g_key_file_has_key(k,"Studio","input_enabled",NULL))
        u->saved_arm=
            g_key_file_get_boolean(
                k,
                "Studio",
                "input_enabled",
                NULL);

    if(g_key_file_has_key(k,"Studio","auto_select_gamepad",NULL))
        u->saved_auto_select=
            g_key_file_get_boolean(
                k,
                "Studio",
                "auto_select_gamepad",
                NULL);

    load_string_setting(
        k,
        "Studio",
        "gamepad_guid",
        &u->preferred_gamepad_guid);

    load_string_setting(
        k,
        "Bluetooth",
        "primary_adapter",
        &u->preferred_adapter_address);

    load_string_setting(
        k,
        "Bluetooth",
        "secondary_adapter",
        &u->preferred_secondary_address);

    load_string_setting(
        k,
        "Bluetooth",
        "switch_address",
        &u->paired_switch_address);

    load_string_setting(
        k,
        "Bluetooth",
        "paired_adapter",
        &u->paired_adapter_address);

    if(g_key_file_has_key(
            k,
            "Diagnostics",
            "detailed_hid",
            NULL))
        u->saved_traffic=
            g_key_file_get_boolean(
                k,
                "Diagnostics",
                "detailed_hid",
                NULL);

    load_color_setting(k,"pro_body",&u->pro_body_color);
    load_color_setting(k,"pro_buttons",&u->pro_button_color);
    load_color_setting(k,"pro_left_grip",&u->pro_left_grip_color);
    load_color_setting(k,"pro_right_grip",&u->pro_right_grip_color);

    return has_state;
}

static gboolean profile_state_save_path(Ui *u,
                                        const char *path,
                                        gboolean active) {
    if(!path)
        return FALSE;

    g_autoptr(GKeyFile) k=g_key_file_new();

    if(g_file_test(path,G_FILE_TEST_EXISTS))
        g_key_file_load_from_file(
            k,
            path,
            G_KEY_FILE_NONE,
            NULL);

    g_key_file_set_boolean(k,"Studio","active",active);
    g_key_file_set_integer(
        k,
        "Studio",
        "source",
        (gint)u->saved_source);

    g_key_file_set_boolean(
        k,
        "Studio",
        "input_enabled",
        u->saved_arm);

    g_key_file_set_boolean(
        k,
        "Studio",
        "auto_select_gamepad",
        u->saved_auto_select);

    save_optional_string(
        k,
        "Studio",
        "gamepad_guid",
        u->preferred_gamepad_guid);

    save_optional_string(
        k,
        "Bluetooth",
        "primary_adapter",
        u->preferred_adapter_address);

    save_optional_string(
        k,
        "Bluetooth",
        "secondary_adapter",
        u->preferred_secondary_address);

    save_optional_string(
        k,
        "Bluetooth",
        "switch_address",
        u->paired_switch_address);

    save_optional_string(
        k,
        "Bluetooth",
        "paired_adapter",
        u->paired_adapter_address);

    g_key_file_set_boolean(
        k,
        "Diagnostics",
        "detailed_hid",
        u->saved_traffic);

    g_autofree char *body=rgba_hex(&u->pro_body_color);
    g_autofree char *buttons=rgba_hex(&u->pro_button_color);
    g_autofree char *left=rgba_hex(&u->pro_left_grip_color);
    g_autofree char *right=rgba_hex(&u->pro_right_grip_color);

    g_autofree char *body_css=g_strconcat("#",body,NULL);
    g_autofree char *buttons_css=g_strconcat("#",buttons,NULL);
    g_autofree char *left_css=g_strconcat("#",left,NULL);
    g_autofree char *right_css=g_strconcat("#",right,NULL);

    g_key_file_set_string(
        k,
        "Appearance",
        "pro_body",
        body_css);

    g_key_file_set_string(
        k,
        "Appearance",
        "pro_buttons",
        buttons_css);

    g_key_file_set_string(
        k,
        "Appearance",
        "pro_left_grip",
        left_css);

    g_key_file_set_string(
        k,
        "Appearance",
        "pro_right_grip",
        right_css);

    g_autoptr(GError) error=NULL;

    if(!g_key_file_save_to_file(k,path,&error)) {
        g_warning(
            "Could not save profile state: %s",
            error->message);
        return FALSE;
    }

    return TRUE;
}

static void mark_profile_active(Ui *u,const char *active_name) {
    GDir *dir=g_dir_open(u->config_dir,0,NULL);

    if(!dir)
        return;

    const char *filename;

    while((filename=g_dir_read_name(dir))) {
        if(!g_str_has_suffix(filename,".ini"))
            continue;

        g_autofree char *name=
            g_strndup(
                filename,
                strlen(filename)-4);

        g_autofree char *path=
            g_build_filename(
                u->config_dir,
                filename,
                NULL);

        g_autoptr(GKeyFile) k=g_key_file_new();

        if(!g_key_file_load_from_file(
                k,
                path,
                G_KEY_FILE_NONE,
                NULL))
            continue;

        g_key_file_set_boolean(
            k,
            "Studio",
            "active",
            !strcmp(name,active_name));

        g_key_file_save_to_file(k,path,NULL);
    }

    g_dir_close(dir);
}

static char *active_profile_name(Ui *u) {
    GDir *dir=g_dir_open(u->config_dir,0,NULL);

    if(!dir)
        return g_strdup("Default");

    char *fallback=NULL;
    gboolean have_default=FALSE;
    const char *filename;

    while((filename=g_dir_read_name(dir))) {
        if(!g_str_has_suffix(filename,".ini"))
            continue;

        g_autofree char *name=
            g_strndup(
                filename,
                strlen(filename)-4);

        if(!fallback)
            fallback=g_strdup(name);

        if(!strcmp(name,"Default"))
            have_default=TRUE;

        g_autofree char *path=
            g_build_filename(
                u->config_dir,
                filename,
                NULL);

        g_autoptr(GKeyFile) k=g_key_file_new();

        if(g_key_file_load_from_file(
                k,
                path,
                G_KEY_FILE_NONE,
                NULL) &&
           g_key_file_has_key(
                k,
                "Studio",
                "active",
                NULL) &&
           g_key_file_get_boolean(
                k,
                "Studio",
                "active",
                NULL)) {
            g_dir_close(dir);
            g_free(fallback);
            return g_strdup(name);
        }
    }

    g_dir_close(dir);

    if(have_default) {
        g_free(fallback);
        return g_strdup("Default");
    }

    return fallback?fallback:g_strdup("Default");
}

/*
 * One-time migration from the old ~/.config/pcble2gamepad/settings.ini.
 * Once successfully copied into the selected profile, settings.ini is
 * removed and never written again.
 */
static char *legacy_profile_name(Ui *u) {
    if(!u->settings_path ||
       !g_file_test(
            u->settings_path,
            G_FILE_TEST_EXISTS))
        return NULL;

    g_autoptr(GKeyFile) k=g_key_file_new();

    if(!g_key_file_load_from_file(
            k,
            u->settings_path,
            G_KEY_FILE_NONE,
            NULL))
        return NULL;

    if(!g_key_file_has_key(k,"UI","profile",NULL))
        return NULL;

    return g_key_file_get_string(
        k,
        "UI",
        "profile",
        NULL);
}

static void legacy_state_load(Ui *u) {
    if(!u->settings_path ||
       !g_file_test(
            u->settings_path,
            G_FILE_TEST_EXISTS))
        return;

    g_autoptr(GKeyFile) k=g_key_file_new();

    if(!g_key_file_load_from_file(
            k,
            u->settings_path,
            G_KEY_FILE_NONE,
            NULL))
        return;

    if(g_key_file_has_key(k,"Input","source",NULL)) {
        gint source=
            g_key_file_get_integer(
                k,
                "Input",
                "source",
                NULL);

        if(source>=0 && source<=1)
            u->saved_source=(guint)source;
    }

    if(g_key_file_has_key(k,"Input","enabled",NULL))
        u->saved_arm=
            g_key_file_get_boolean(
                k,
                "Input",
                "enabled",
                NULL);

    if(g_key_file_has_key(
            k,
            "Input",
            "auto_select_gamepad",
            NULL))
        u->saved_auto_select=
            g_key_file_get_boolean(
                k,
                "Input",
                "auto_select_gamepad",
                NULL);

    load_string_setting(
        k,
        "Input",
        "gamepad_guid",
        &u->preferred_gamepad_guid);

    load_string_setting(
        k,
        "Console",
        "switch_address",
        &u->paired_switch_address);

    load_string_setting(
        k,
        "Console",
        "adapter_address",
        &u->paired_adapter_address);

    if(u->paired_adapter_address) {
        g_free(u->preferred_adapter_address);
        u->preferred_adapter_address=
            g_strdup(u->paired_adapter_address);
    }

    if(g_key_file_has_key(
            k,
            "Diagnostics",
            "detailed_hid",
            NULL))
        u->saved_traffic=
            g_key_file_get_boolean(
                k,
                "Diagnostics",
                "detailed_hid",
                NULL);

    load_color_setting(k,"pro_body",&u->pro_body_color);
    load_color_setting(k,"pro_buttons",&u->pro_button_color);
    load_color_setting(k,"pro_left_grip",&u->pro_left_grip_color);
    load_color_setting(k,"pro_right_grip",&u->pro_right_grip_color);
}

static void app_state_save(Ui *u) {
    if(u->loading || !u->profile_name)
        return;

    g_autofree char *path=
        profile_path(u,u->profile_name);

    profile_state_save_path(u,path,TRUE);
}

static gboolean save_profile_quiet(Ui *u) {
    if(u->loading || !u->profile_name)
        return TRUE;

    g_autofree char *path=
        profile_path(u,u->profile_name);

    g_autoptr(GError) error=NULL;

    if(!input_profile_save(
            &u->profile,
            path,
            &error)) {
        g_warning(
            "Could not save input profile: %s",
            error->message);
        return FALSE;
    }

    return profile_state_save_path(
        u,
        path,
        TRUE);
}

static void profile_scan(Ui *u) {
    gboolean previous_loading=u->loading;
    u->loading=TRUE;

    gtk_string_list_splice(
        u->profile_names,
        0,
        g_list_model_get_n_items(
            G_LIST_MODEL(u->profile_names)),
        NULL);

    GDir *d=g_dir_open(u->config_dir,0,NULL);
    const char *name;
    guint selected=0;
    guint i=0;

    if(d) {
        while((name=g_dir_read_name(d))) {
            if(!g_str_has_suffix(name,".ini"))
                continue;

            char *profile=
                g_strndup(name,strlen(name)-4);

            if(!strcmp(profile,u->profile_name))
                selected=i;

            gtk_string_list_append(
                u->profile_names,
                profile);

            g_free(profile);
            i++;
        }

        g_dir_close(d);
    }

    gtk_drop_down_set_selected(
        u->profiles,
        selected);

    u->loading=previous_loading;
}

static void profile_saved(GtkButton *b,Ui *u) {
    (void)b;

    if(save_profile_quiet(u)) {
        mark_profile_active(u,u->profile_name);
        toast(u,"Profile saved");
    } else {
        toast(u,"Could not save profile");
    }
}

static void profile_selected(GObject *o,GParamSpec *p,Ui *u) {
    (void)o;
    (void)p;

    if(u->loading)
        return;

    guint i=gtk_drop_down_get_selected(u->profiles);
    const char *name=
        gtk_string_list_get_string(
            u->profile_names,
            i);

    if(!name)
        return;

    g_autofree char *path=
        profile_path(u,name);

    g_autoptr(GError) error=NULL;

    release_input(u);

    if(!input_profile_load(
            &u->profile,
            path,
            &error)) {
        toast(u,error->message);
        return;
    }

    g_free(u->profile_name);
    u->profile_name=g_strdup(name);

    profile_state_defaults(u);
    profile_state_load_path(u,path);

    mark_profile_active(
        u,
        u->profile_name);

    apply_profile_state(u);
}
static void new_profile_response(AdwAlertDialog *dialog,const char *response,Ui *u) {
    if(strcmp(response,"save"))return;
    GtkWidget *entry=adw_alert_dialog_get_extra_child(dialog);const char *name=gtk_editable_get_text(GTK_EDITABLE(entry));
    if(!g_regex_match_simple("^[A-Za-z0-9 _-]{1,40}$",name,0,0)){toast(u,"Use 1–40 letters, numbers, spaces, hyphens or underscores");return;}
    g_autofree char *path=profile_path(u,name);
    if(g_file_test(path,G_FILE_TEST_EXISTS)){toast(u,"A profile with this name already exists");return;}
    g_free(u->profile_name);u->profile_name=g_strdup(name);profile_saved(NULL,u);profile_scan(u);
}
static void new_profile(GtkButton *b,Ui *u) {
    (void)b;neutral(u);AdwDialog *d=adw_alert_dialog_new("Save a new profile","Copy the current bindings and stick settings.");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d),gtk_entry_new());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d),"cancel","Cancel","save","Save",NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d),"save",ADW_RESPONSE_SUGGESTED);
    g_signal_connect(d,"response",G_CALLBACK(new_profile_response),u);adw_dialog_present(d,GTK_WIDGET(u->window));
}
static void settings_changed(GObject *o,GParamSpec *p,Ui *u) {
    (void)o;(void)p;if(u->loading)return;
    u->profile.deadzone=gtk_range_get_value(GTK_RANGE(u->deadzone))/100.;
    u->profile.sensitivity=gtk_range_get_value(GTK_RANGE(u->sensitivity))/100.;
    for(int i=0;i<4;i++)u->profile.invert[i]=gtk_switch_get_active(u->invert[i]);
    u->profile.swap_sticks=gtk_switch_get_active(u->swap);u->profile.background=gtk_switch_get_active(u->background);
    u->profile.swap_face_buttons=gtk_switch_get_active(u->swap_face);
    save_profile_quiet(u);
}
static void scale_changed(GtkRange *r,Ui *u){settings_changed(G_OBJECT(r),NULL,u);}
static void refresh_bindings(Ui *u) {
    for(int i=0;i<INPUT_ACTIONS;i++) {
        const char *name=u->profile.keys[i]?gdk_keyval_name(u->profile.keys[i]):"Unbound";
        gtk_button_set_label(u->key_buttons[i],name?name:"Unbound");
    }
    for(int i=0;i<INPUT_BUTTONS;i++) {
        int b=u->profile.buttons[i];const char *name=b<0?"Unbound":b==SDL_CONTROLLER_BUTTON_MAX?"Left trigger":b==SDL_CONTROLLER_BUTTON_MAX+1?"Right trigger":SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
        gtk_button_set_label(u->pad_buttons[i],name?name:"Unbound");
    }
    gboolean previous_loading=u->loading;
    u->loading=TRUE;

    gtk_range_set_value(
        GTK_RANGE(u->deadzone),
        u->profile.deadzone*100);

    gtk_range_set_value(
        GTK_RANGE(u->sensitivity),
        u->profile.sensitivity*100);

    for(int i=0;i<4;i++)
        gtk_switch_set_active(
            u->invert[i],
            u->profile.invert[i]);

    gtk_switch_set_active(
        u->swap,
        u->profile.swap_sticks);

    gtk_switch_set_active(
        u->background,
        u->profile.background);

    if(u->swap_face)
        gtk_switch_set_active(
            u->swap_face,
            u->profile.swap_face_buttons);

    if(u->controllers)
        gtk_drop_down_set_selected(
            u->controllers,
            (guint)u->profile.emulated_controller);

    u->loading=previous_loading;
}

static void apply_profile_state(Ui *u) {
    gboolean previous_loading=u->loading;
    u->loading=TRUE;

    if(u->source)
        gtk_drop_down_set_selected(
            u->source,
            u->saved_source);

    if(u->arm)
        gtk_switch_set_active(
            GTK_SWITCH(u->arm),
            u->saved_arm);

    if(u->auto_select)
        gtk_switch_set_active(
            u->auto_select,
            u->saved_auto_select);

    if(u->traffic_logs)
        gtk_switch_set_active(
            u->traffic_logs,
            u->saved_traffic);

    if(u->pro_body_color_button)
        gtk_color_dialog_button_set_rgba(
            u->pro_body_color_button,
            &u->pro_body_color);

    if(u->pro_button_color_button)
        gtk_color_dialog_button_set_rgba(
            u->pro_button_color_button,
            &u->pro_button_color);

    if(u->pro_left_grip_color_button)
        gtk_color_dialog_button_set_rgba(
            u->pro_left_grip_color_button,
            &u->pro_left_grip_color);

    if(u->pro_right_grip_color_button)
        gtk_color_dialog_button_set_rgba(
            u->pro_right_grip_color_button,
            &u->pro_right_grip_color);

    refresh_bindings(u);

    if(u->source_hint)
        gtk_label_set_text(
            u->source_hint,
            u->saved_source==0
                ?"Keyboard input works while this window is focused. Escape pauses input."
                :"Standard SDL gamepad mapping. Customize buttons and stick settings below.");

    update_paired_console(u);

    u->loading=previous_loading;

    update_pro_svg_colors(u);

    devices_scan(u);
    adapters_scan(NULL,u);
    controller_selected(NULL,NULL,u);

    if(u->saved_arm && u->drawing)
        gtk_widget_grab_focus(u->drawing);

    update_controls(u);
}
static void bind_key(GtkButton *b,Ui *u) {
    neutral(u);u->learn_pad=-1;u->learn_key=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"index"));
    refresh_bindings(u);gtk_button_set_label(b,"Press a key…");gtk_label_set_text(GTK_LABEL(u->capture_hint),"Press a key. Escape cancels; Backspace clears this binding.");
}
static void bind_pad(GtkButton *b,Ui *u) {
    neutral(u);if(!u->pad){toast(u,"Connect and select a PC controller first");return;}
    u->learn_key=-1;u->learn_pad=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"index"));refresh_bindings(u);gtk_button_set_label(b,"Press a button…");
    toast(u,"Press a controller button or trigger. Escape cancels.");
}
static gboolean key_pressed(GtkEventControllerKey *c,guint key,guint code,GdkModifierType modifiers,Ui *u) {
    (void)c;(void)code;(void)modifiers;key=gdk_keyval_to_lower(key);
    if(u->learn_key>=0) {
        if(key!=GDK_KEY_Escape){guint value=key==GDK_KEY_BackSpace?0:key;for(int i=0;i<INPUT_ACTIONS;i++)if(value && u->profile.keys[i]==value)u->profile.keys[i]=0;u->profile.keys[u->learn_key]=value;}
        u->learn_key=-1;refresh_bindings(u);gtk_label_set_text(GTK_LABEL(u->capture_hint),"Click a binding to change it. Save your profile when finished.");return TRUE;
    }
    if(key==GDK_KEY_Escape) {
        u->learn_pad=-1;
        release_input(u);

        if(gtk_switch_get_active(GTK_SWITCH(u->arm)))
            gtk_switch_set_active(
                GTK_SWITCH(u->arm),
                FALSE);

        refresh_bindings(u);
        return TRUE;
    }
    if(!gtk_switch_get_active(GTK_SWITCH(u->arm)) || gtk_drop_down_get_selected(u->source)!=0)return FALSE;
    for(int i=0;i<INPUT_ACTIONS;i++)if(u->profile.keys[i]==key){g_hash_table_add(u->keys,GUINT_TO_POINTER(key));return TRUE;}
    return FALSE;
}
static void key_released(GtkEventControllerKey *c,guint key,guint code,GdkModifierType mod,Ui *u){(void)c;(void)code;(void)mod;g_hash_table_remove(u->keys,GUINT_TO_POINTER(gdk_keyval_to_lower(key)));}
static void focus_changed(GObject *o,GParamSpec *p,Ui *u){(void)o;(void)p;if(!gtk_window_is_active(u->window))release_input(u);}
static void armed_changed(GObject *o,GParamSpec *p,Ui *u){
    (void)o;(void)p;if(u->loading)return;
    release_input(u);
    u->saved_arm=gtk_switch_get_active(GTK_SWITCH(u->arm));
    if(u->saved_arm)gtk_widget_grab_focus(u->drawing);
    app_state_save(u);
}
static void source_changed(GObject *o,GParamSpec *p,Ui *u){
    (void)o;(void)p;if(u->loading)return;
    release_input(u);
    u->saved_source=gtk_drop_down_get_selected(u->source);
    gtk_label_set_text(u->source_hint,u->saved_source==0?"Keyboard input works while this window is focused. Escape pauses input.":"Standard SDL gamepad mapping. Customize buttons and stick settings below.");
    app_state_save(u);
}
static void auto_select_changed(GObject *o,GParamSpec *p,Ui *u){
    (void)o;(void)p;if(u->loading)return;
    u->saved_auto_select=gtk_switch_get_active(u->auto_select);
    app_state_save(u);
    if(u->saved_auto_select && !u->pad)devices_scan(u);
}
static char *gamepad_guid_for_index(int index) {
    char guid[33]={0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index),guid,sizeof(guid));
    return g_strdup(guid);
}
static void device_selected(GObject *o,GParamSpec *p,Ui *u) {
    (void)p;if(u->loading)return;
    release_input(u);
    if(u->pad){SDL_GameControllerClose(u->pad);u->pad=NULL;}
    guint i=gtk_drop_down_get_selected(u->devices);
    if(i>0 && i<u->device_ids->len) {
        int index=GPOINTER_TO_INT(g_ptr_array_index(u->device_ids,i));
        u->pad=SDL_GameControllerOpen(index);
        if(u->pad) {
            g_free(u->preferred_gamepad_guid);
            u->preferred_gamepad_guid=gamepad_guid_for_index(index);
        } else toast(u,SDL_GetError());
    } else if(o) {
        g_clear_pointer(&u->preferred_gamepad_guid,g_free);
    }
    app_state_save(u);
}
static void devices_scan(Ui *u) {
    SDL_JoystickID selected=u->pad?SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(u->pad)):-1;
    gboolean had_pad=u->pad!=NULL;
    guint current_choice=0,preferred_choice=0,first_choice=0;

    u->loading=TRUE;
    gtk_string_list_splice(u->device_names,0,g_list_model_get_n_items(G_LIST_MODEL(u->device_names)),NULL);
    g_ptr_array_set_size(u->device_ids,0);
    gtk_string_list_append(u->device_names,"Select a controller");
    g_ptr_array_add(u->device_ids,GINT_TO_POINTER(-1));

    for(int i=0;i<SDL_NumJoysticks();i++)if(SDL_IsGameController(i)) {
        gtk_string_list_append(u->device_names,SDL_GameControllerNameForIndex(i));
        g_ptr_array_add(u->device_ids,GINT_TO_POINTER(i));
        guint position=u->device_ids->len-1;
        if(!first_choice)first_choice=position;
        if(SDL_JoystickGetDeviceInstanceID(i)==selected)current_choice=position;
        g_autofree char *guid=gamepad_guid_for_index(i);
        if(u->preferred_gamepad_guid && !strcmp(guid,u->preferred_gamepad_guid))
            preferred_choice=position;
    }

    guint choice=current_choice?current_choice:
        preferred_choice?preferred_choice:
        u->saved_auto_select?first_choice:0;

    gtk_drop_down_set_selected(u->devices,choice);
    u->loading=FALSE;
    u->joystick_count=SDL_NumJoysticks();
    device_selected(NULL,NULL,u);

    if(had_pad && !u->pad)
        toast(u,"Controller disconnected; inputs released");
}
static char *controller_asset_path(const char *name) {
    const char *override=g_getenv("PCBLE2GAMEPAD_ASSET_DIR");

    if(override && *override) {
        char *path=g_build_filename(override,name,NULL);

        if(g_file_test(path,G_FILE_TEST_IS_REGULAR))
            return path;

        g_free(path);
    }

    /*
     * Development build:
     *
     *   repository/
     *     build/pcble2gamepad
     *     assets/svg/...
     */
    g_autofree char *exe=g_file_read_link("/proc/self/exe",NULL);

    if(exe) {
        g_autofree char *binary_dir=g_path_get_dirname(exe);
        g_autofree char *root=g_path_get_dirname(binary_dir);
        char *path=g_build_filename(
            root,
            "assets",
            "svg",
            name,
            NULL);

        if(g_file_test(path,G_FILE_TEST_IS_REGULAR))
            return path;

        g_free(path);
    }

    /*
     * Installed build.
     */
    return g_build_filename(
        PCBLE2GAMEPAD_ASSET_DIR,
        name,
        NULL);
}

static RsvgHandle *load_controller_svg(const char *name) {
    g_autofree char *path=controller_asset_path(name);
    g_autoptr(GError) error=NULL;

    RsvgHandle *handle=rsvg_handle_new_from_file(path,&error);

    if(!handle) {
        g_warning(
            "Could not load controller SVG %s: %s",
            path,
            error?error->message:"unknown error");
    }

    return handle;
}

static void update_pro_svg_colors(Ui *u) {
    if(!u->pro_svg)
        return;

    g_autofree char *body=rgba_hex(&u->pro_body_color);
    g_autofree char *buttons=rgba_hex(&u->pro_button_color);
    g_autofree char *left=rgba_hex(&u->pro_left_grip_color);
    g_autofree char *right=rgba_hex(&u->pro_right_grip_color);

    /*
     * The imported VSCView SVG already exposes stable element IDs.
     *
     * Keep outlines/text untouched and recolor only the physical surfaces
     * represented by the Switch SPI color fields.
     */
    g_autofree char *css=g_strdup_printf(
        "#path9073,#path1675-3,#path1685-5 { fill:#%s !important; }"
        "#path9067 { fill:#%s !important; }"
        "#path9077 { fill:#%s !important; }"
        "#path1458,#path8519,#path5183,#path2365,"
        "#path1717-9,#path1701-7,"
        "#path1737-4,#path1733-7,#path1725-4,#path1729-5,"
        "#path4084,#path1759-9,#path1767-5,#path1773-8,"
        "#path1779-7 { fill:#%s !important; }",
        body,
        left,
        right,
        buttons);

    g_autoptr(GError) error=NULL;

    if(!rsvg_handle_set_stylesheet(
            u->pro_svg,
            (const guint8 *)css,
            strlen(css),
            &error)) {
        g_warning(
            "Could not update Pro Controller SVG colors: %s",
            error?error->message:"unknown error");
        return;
    }

    if(u->drawing)
        gtk_widget_queue_draw(u->drawing);
}

static void render_controller_svg(RsvgHandle *handle,
                                  cairo_t *cr,
                                  double x,
                                  double y,
                                  double width,
                                  double height) {
    if(!handle)
        return;

    RsvgRectangle viewport={
        .x=x,
        .y=y,
        .width=width,
        .height=height
    };

    g_autoptr(GError) error=NULL;

    if(!rsvg_handle_render_document(
            handle,
            cr,
            &viewport,
            &error)) {
        g_warning(
            "Could not render controller SVG: %s",
            error?error->message:"unknown error");
    }
}

static void draw_missing_controller(cairo_t *cr) {
    cairo_save(cr);

    cairo_set_source_rgba(cr,1.0,1.0,1.0,0.45);
    cairo_select_font_face(
        cr,
        "Sans",
        CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,16);
    cairo_move_to(cr,225,140);
    cairo_show_text(cr,"Controller SVG unavailable");

    cairo_restore(cr);
}

static void draw(GtkDrawingArea *area,
                 cairo_t *cr,
                 int width,
                 int height,
                 gpointer data) {
    (void)area;

    Ui *u=data;

    /*
     * Keep a stable logical 640x270 canvas. SVG rendering stays sharp at
     * every GTK scale factor because librsvg paints directly into Cairo.
     */
    double scale=MIN(width/640.0,height/270.0);

    cairo_save(cr);
    cairo_translate(
        cr,
        (width-640.0*scale)/2.0,
        (height-270.0*scale)/2.0);
    cairo_scale(cr,scale,scale);

    if(pair_mode(u)) {
        /*
         * Original SVG ratio:
         *   85.9 x 246.1
         *
         * Render the Joy-Con pair separately rather than stretching a
         * pre-composed bitmap. This also gives us independent layers for
         * input highlighting later.
         */
        if(u->joycon_left_svg && u->joycon_right_svg) {
            render_controller_svg(
                u->joycon_left_svg,
                cr,
                220,12,
                84,241);

            render_controller_svg(
                u->joycon_right_svg,
                cr,
                336,12,
                84,241);
        } else {
            draw_missing_controller(cr);
        }
    } else {
        /*
         * Original SVG ratio:
         *   419.1 x 304.5
         *
         * 344 x 250 keeps that aspect ratio almost exactly and leaves some
         * breathing room around the controller in the hero card.
         */
        if(u->pro_svg) {
            render_controller_svg(
                u->pro_svg,
                cr,
                148,10,
                344,250);
        } else {
            draw_missing_controller(cr);
        }
    }

    cairo_restore(cr);
}

static guint adapter_index_for_address(Ui *u,
                                       const char *address,
                                       gboolean *found) {
    if(found)
        *found=FALSE;

    if(!address || !*address)
        return 0;

    for(guint i=0;i<u->adapter_addresses->len;i++) {
        const char *candidate=
            g_ptr_array_index(
                u->adapter_addresses,
                i);

        if(!g_ascii_strcasecmp(candidate,address)) {
            if(found)
                *found=TRUE;
            return i;
        }
    }

    return 0;
}

static void adapter_selection_changed(GObject *object,
                                      GParamSpec *pspec,
                                      Ui *u) {
    (void)pspec;

    if(u->loading)
        return;

    if(object==G_OBJECT(u->adapters)) {
        guint i=
            gtk_drop_down_get_selected(
                u->adapters);

        if(i<u->adapter_addresses->len) {
            const char *address=
                g_ptr_array_index(
                    u->adapter_addresses,
                    i);

            if(!u->preferred_adapter_address ||
               g_ascii_strcasecmp(
                    u->preferred_adapter_address,
                    address)) {
                g_free(u->preferred_adapter_address);
                u->preferred_adapter_address=
                    g_strdup(address);
            }
        }
    } else if(object==G_OBJECT(u->secondary)) {
        guint i=
            gtk_drop_down_get_selected(
                u->secondary);

        if(i<u->secondary_addresses->len) {
            const char *address=
                g_ptr_array_index(
                    u->secondary_addresses,
                    i);

            if(!u->preferred_secondary_address ||
               g_ascii_strcasecmp(
                    u->preferred_secondary_address,
                    address)) {
                g_free(u->preferred_secondary_address);
                u->preferred_secondary_address=
                    g_strdup(address);
            }
        }
    }

    app_state_save(u);
    update_controls(u);
}

static void adapters_scan(GtkButton *b,Ui *u) {
    (void)b;

    g_autoptr(GError) error=NULL;

    g_autoptr(GDBusConnection) bus=
        g_bus_get_sync(
            G_BUS_TYPE_SYSTEM,
            NULL,
            &error);

    if(!bus) {
        toast(u,error->message);
        return;
    }

    g_autoptr(GVariant) reply=
        g_dbus_connection_call_sync(
            bus,
            "org.bluez",
            "/",
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
            0,
            1500,
            NULL,
            &error);

    if(!reply) {
        toast(u,error->message);
        return;
    }

    gboolean previous_loading=u->loading;
    u->loading=TRUE;

    gtk_string_list_splice(
        u->adapter_names,
        0,
        g_list_model_get_n_items(
            G_LIST_MODEL(u->adapter_names)),
        NULL);

    gtk_string_list_splice(
        u->secondary_names,
        0,
        g_list_model_get_n_items(
            G_LIST_MODEL(u->secondary_names)),
        NULL);

    g_ptr_array_set_size(u->adapter_ids,0);
    g_ptr_array_set_size(u->adapter_addresses,0);
    g_ptr_array_set_size(u->secondary_ids,0);
    g_ptr_array_set_size(u->secondary_addresses,0);

    GVariantIter *objects;
    g_variant_get(
        reply,
        "(a{oa{sa{sv}}})",
        &objects);

    char *path;
    GVariant *interfaces;

    while(g_variant_iter_next(
            objects,
            "{o@a{sa{sv}}}",
            &path,
            &interfaces)) {
        GVariant *props=
            g_variant_lookup_value(
                interfaces,
                "org.bluez.Adapter1",
                G_VARIANT_TYPE_VARDICT);

        if(props) {
            const char *address="";

            g_variant_lookup(
                props,
                "Address",
                "&s",
                &address);

            char *id=g_path_get_basename(path);

            /*
             * Deliberately omit Adapter1.Alias here. During emulation the
             * backend changes it to Pro Controller / Joy-Con (L/R), so it is
             * not a stable identifier for a physical dongle.
             */
            char *name=
                g_strdup_printf(
                    "%s · %s",
                    id,
                    address);

            gtk_string_list_append(
                u->adapter_names,
                name);

            gtk_string_list_append(
                u->secondary_names,
                name);

            g_ptr_array_add(
                u->adapter_ids,
                id);

            g_ptr_array_add(
                u->adapter_addresses,
                g_strdup(address));

            g_ptr_array_add(
                u->secondary_ids,
                g_strdup(id));

            g_ptr_array_add(
                u->secondary_addresses,
                g_strdup(address));

            g_free(name);
            g_variant_unref(props);
        }

        g_free(path);
        g_variant_unref(interfaces);
    }

    g_variant_iter_free(objects);

    guint primary=0;
    gboolean found=FALSE;

    if(u->preferred_adapter_address)
        primary=
            adapter_index_for_address(
                u,
                u->preferred_adapter_address,
                &found);

    if(!found && u->paired_adapter_address)
        primary=
            adapter_index_for_address(
                u,
                u->paired_adapter_address,
                &found);

    if(!found)
        primary=
            adapter_index_for_address(
                u,
                "E0:AD:47:40:70:D9",
                &found);

    if(primary>=u->adapter_ids->len)
        primary=0;

    guint secondary=0;
    gboolean secondary_found=FALSE;

    if(u->preferred_secondary_address)
        secondary=
            adapter_index_for_address(
                u,
                u->preferred_secondary_address,
                &secondary_found);

    if(!secondary_found ||
       secondary==primary) {
        secondary=0;

        for(guint i=0;i<u->secondary_ids->len;i++) {
            if(i!=primary) {
                secondary=i;
                break;
            }
        }
    }

    gtk_drop_down_set_selected(
        u->adapters,
        primary);

    if(u->secondary_ids->len)
        gtk_drop_down_set_selected(
            u->secondary,
            secondary);

    u->loading=previous_loading;

    update_controls(u);
}

static void request_free(gpointer data){Request *r=data;g_free(r->path);g_free(r->path2);json_object_unref(r->request);g_free(r);}
static JsonObject *merge_pair(JsonObject *left,JsonObject *right) {
    if(!json_object_get_boolean_member_with_default(left,"ok",FALSE))return json_object_ref(left);
    if(!json_object_get_boolean_member_with_default(right,"ok",FALSE))return json_object_ref(right);
    JsonObject *a=json_object_get_object_member(left,"result"),*b=json_object_get_object_member(right,"result");
    JsonObject *root=json_object_new(),*result=json_object_new();json_object_set_int_member(root,"version",1);json_object_set_boolean_member(root,"ok",TRUE);json_object_set_object_member(root,"result",result);
    const char *as=json_object_get_string_member_with_default(a,"state","waiting"),*bs=json_object_get_string_member_with_default(b,"state","waiting");
    json_object_set_string_member(result,"state",!strcmp(as,"connected")&&!strcmp(bs,"connected")?"connected":"waiting");
    json_object_set_boolean_member(result,"initialized",json_object_get_boolean_member_with_default(a,"initialized",FALSE)&&json_object_get_boolean_member_with_default(b,"initialized",FALSE));
    json_object_set_boolean_member(result,"simulated",json_object_get_boolean_member_with_default(a,"simulated",FALSE)||json_object_get_boolean_member_with_default(b,"simulated",FALSE));
    g_autofree char *peers=g_strdup_printf("L: %s  ·  R: %s",json_object_get_string_member_with_default(a,"peer","waiting"),json_object_get_string_member_with_default(b,"peer","waiting"));json_object_set_string_member(result,"peer",peers);
    json_object_set_string_member(result,"profile","Nintendo Joy-Con Pair");
    json_object_set_int_member(result,"tx",json_object_get_int_member_with_default(a,"tx",0)+json_object_get_int_member_with_default(b,"tx",0));
    json_object_set_int_member(result,"rx",json_object_get_int_member_with_default(a,"rx",0)+json_object_get_int_member_with_default(b,"rx",0));
    json_object_set_int_member(result,"player_lights",json_object_get_int_member_with_default(a,"player_lights",0)|json_object_get_int_member_with_default(b,"player_lights",0));
    for(int field=0;field<2;field++) {const char *name=field?"sticks":"buttons";JsonArray *source=json_object_get_array_member(a,name),*copy=json_array_new();if(source)for(guint i=0;i<json_array_get_length(source);i++)json_array_add_int_element(copy,json_array_get_int_element(source,i));json_object_set_array_member(result,name,copy);}
    JsonArray *logs=json_array_new();JsonObject *sides[]={a,b};const char *prefix[]={"L","R"};
    for(int side=0;side<2;side++){JsonArray *source=json_object_get_array_member(sides[side],"logs");if(source)for(guint i=0;i<json_array_get_length(source);i++){g_autofree char *line=g_strdup_printf("[%s] %s",prefix[side],json_array_get_string_element(source,i));json_array_add_string_element(logs,line);}}
    json_object_set_array_member(result,"logs",logs);return root;
}
static void worker(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source;(void)cancel;Request *r=data;GError *e=NULL;JsonObject *o=jc_client_request_object(r->path,r->request,&e);
    if(!o){g_task_return_error(task,e);return;}
    if(r->path2){GError *second_error=NULL;JsonObject *second=jc_client_request_object(r->path2,r->request,&second_error);if(!second){json_object_unref(o);g_task_return_error(task,second_error);return;}JsonObject *merged=merge_pair(o,second);json_object_unref(o);json_object_unref(second);o=merged;}
    g_task_return_pointer(task,o,(GDestroyNotify)json_object_unref);
}
static void finish_close(Ui *u){u->closed=TRUE;gtk_window_destroy(u->window);g_application_quit(G_APPLICATION(u->app));ui_unref(u);}
static void complete(GObject *source,GAsyncResult *result,gpointer data) {
    (void)source;Ui *u=data;g_autoptr(GError)e=NULL;g_autoptr(JsonObject)o=g_task_propagate_pointer(G_TASK(result),&e);u->busy=FALSE;
    Request *r=g_task_get_task_data(G_TASK(result));const char *method=json_object_get_string_member(r->request,"method");
    if(u->closed){ui_unref(u);return;}
    if(u->closing && (!u->launcher && (!strcmp(method,"stop") || e))){finish_close(u);ui_unref(u);return;}
    u->online=o && json_object_get_boolean_member_with_default(o,"ok",FALSE);
    if(u->online) {
        JsonObject *s=json_object_get_object_member(o,"result");const char *state=json_object_get_string_member_with_default(s,"state","waiting");
        u->connected=!strcmp(state,"connected");
        gboolean simulated=json_object_get_boolean_member_with_default(s,"simulated",FALSE);
        gtk_label_set_text(u->status,simulated?"SIMULATION":!strcmp(state,"connected")?"CONNECTED":"WAITING FOR CONSOLE");
        const char *peer_address=json_object_get_string_member_with_default(s,"peer","");
        gtk_label_set_text(u->peer,peer_address);

        if(!simulated && !pair_mode(u) && !strcmp(state,"connected") &&
           g_regex_match_simple("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$",peer_address,0,0)) {
            guint adapter_index=gtk_drop_down_get_selected(u->adapters);
            if(adapter_index<u->adapter_addresses->len) {
                const char *adapter_address=g_ptr_array_index(u->adapter_addresses,adapter_index);
                gboolean changed=
                    !u->paired_switch_address ||
                    g_ascii_strcasecmp(
                        u->paired_switch_address,
                        peer_address) ||
                    !u->paired_adapter_address ||
                    g_ascii_strcasecmp(
                        u->paired_adapter_address,
                        adapter_address) ||
                    !u->preferred_adapter_address ||
                    g_ascii_strcasecmp(
                        u->preferred_adapter_address,
                        adapter_address);

                if(changed) {
                    g_free(u->paired_switch_address);
                    g_free(u->paired_adapter_address);
                    g_free(u->preferred_adapter_address);

                    u->paired_switch_address=
                        g_strdup(peer_address);

                    u->paired_adapter_address=
                        g_strdup(adapter_address);

                    u->preferred_adapter_address=
                        g_strdup(adapter_address);

                    app_state_save(u);
                    update_paired_console(u);
                }
            }
        }

        char *metrics=g_strdup_printf("%"G_GINT64_FORMAT" reports sent  ·  %"G_GINT64_FORMAT" received",json_object_get_int_member_with_default(s,"tx",0),json_object_get_int_member_with_default(s,"rx",0));gtk_label_set_text(u->metrics,metrics);g_free(metrics);
        gtk_label_set_text(u->error,"");
        JsonArray *logs=json_object_get_array_member(s,"logs");GString *text=g_string_new(NULL);
        if(logs)for(guint i=0;i<json_array_get_length(logs);i++)g_string_append_printf(text,"%s\n",json_array_get_string_element(logs,i));
        gtk_text_buffer_set_text(u->logs,text->str,-1);g_string_free(text,TRUE);
    } else {
        u->connected=FALSE;
        gtk_label_set_text(u->status,"OFFLINE");gtk_label_set_text(u->peer,"Start a Bluetooth session to connect your console.");
        if(o)gtk_label_set_text(u->error,json_object_get_string_member_with_default(o,"error","Request failed"));
        else if(strcmp(method,"status"))gtk_label_set_text(u->error,e?e->message:"Backend unavailable");
    }
    update_controls(u);
    if(u->closing){request(u,"stop");}else if(u->pending){char *pending=g_steal_pointer(&u->pending);request(u,pending);g_free(pending);}
    ui_unref(u);
}
static void request(Ui *u,const char *method) {
    if(u->closed)return;
    if(u->busy){if(strcmp(method,"status") && strcmp(method,"input")){g_free(u->pending);u->pending=g_strdup(method);}return;}
    u->busy=TRUE;Request *r=g_new0(Request,1);r->path=g_strdup(u->socket);r->path2=g_strdup(u->socket2);r->request=json_object_new();
    json_object_set_int_member(r->request,"version",1);json_object_set_string_member(r->request,"method",method);
    if(!strcmp(method,"input")){
        JsonArray *a=json_array_new();for(int i=0;i<3;i++)json_array_add_int_element(a,u->frame.buttons[i]);json_object_set_array_member(r->request,"buttons",a);
        a=json_array_new();for(int i=0;i<4;i++)json_array_add_int_element(a,u->frame.sticks[i]);json_object_set_array_member(r->request,"sticks",a);
    } else if(!strcmp(method,"reconnect")) {
        if(u->paired_switch_address)
            json_object_set_string_member(r->request,"address",u->paired_switch_address);
    } else if(!strcmp(method,"logging"))
        json_object_set_boolean_member(r->request,"enabled",gtk_switch_get_active(u->traffic_logs));
    u->refs++;GTask *task=g_task_new(NULL,NULL,complete,u);g_task_set_task_data(task,r,request_free);g_task_run_in_thread(task,worker);g_object_unref(task);
}
static gboolean tick(gpointer data) {
    Ui *u=data;if(u->closed || u->closing)return G_SOURCE_REMOVE;
    SDL_Event event;gboolean rescan=FALSE;while(SDL_PollEvent(&event))if(event.type==SDL_CONTROLLERDEVICEADDED || event.type==SDL_CONTROLLERDEVICEREMOVED)rescan=TRUE;
    if(rescan || SDL_NumJoysticks()!=u->joystick_count)devices_scan(u);
    gboolean pressed[INPUT_ACTIONS]={0};double axes[4]={0};
    if(gtk_drop_down_get_selected(u->source)==0){for(int i=0;i<INPUT_ACTIONS;i++)pressed[i]=g_hash_table_contains(u->keys,GUINT_TO_POINTER(u->profile.keys[i]));}
    else input_gamepad(&u->profile,u->pad,pressed,axes);
    if(u->pad)for(int b=0;b<SDL_CONTROLLER_BUTTON_MAX+2;b++) {
        gboolean down=b<SDL_CONTROLLER_BUTTON_MAX?SDL_GameControllerGetButton(u->pad,(SDL_GameControllerButton)b):SDL_GameControllerGetAxis(u->pad,b==SDL_CONTROLLER_BUTTON_MAX?SDL_CONTROLLER_AXIS_TRIGGERLEFT:SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000;
        if(u->learn_pad>=0 && down && !u->previous_pad[b]){u->profile.buttons[u->learn_pad]=b;u->learn_pad=-1;refresh_bindings(u);toast(u,"Controller binding updated");}
        u->previous_pad[b]=down;
    }
    input_compose(&u->profile,pressed,axes,&u->frame);gtk_widget_queue_draw(u->drawing);
    gboolean allowed=gtk_window_is_active(u->window) || (u->profile.background && gtk_drop_down_get_selected(u->source)==1);
    if(u->online && gtk_switch_get_active(GTK_SWITCH(u->arm)) && allowed)request(u,"input");
    else if(++u->ticks%20==0)request(u,"status");
    return G_SOURCE_CONTINUE;
}
static void launcher_done(GObject *source,GAsyncResult *result,gpointer data) {
    Ui *u=data;g_autoptr(GError)e=NULL;
    GSubprocess *process=G_SUBPROCESS(source);
    gboolean ok=g_subprocess_wait_check_finish(process,result,&e);
    g_clear_object(&u->launcher);

    if(u->closing){finish_close(u);ui_unref(u);return;}

    if(!u->closed) {
        if(!ok) {
            g_autofree char *message=NULL;
            if(g_subprocess_get_if_exited(process))
                message=g_strdup_printf(
                    "Bluetooth session process exited with code %d.",
                    g_subprocess_get_exit_status(process));
            else if(g_subprocess_get_if_signaled(process))
                message=g_strdup_printf(
                    "Bluetooth session process was terminated by signal %d.",
                    g_subprocess_get_term_sig(process));
            else
                message=g_strdup("Bluetooth session process failed.");

            gtk_label_set_text(u->error,message);
        }
        update_controls(u);
        request(u,"status");
    }
    ui_unref(u);
}
static void controller_selected(GObject *o,GParamSpec *p,Ui *u) {
    (void)p;if(u->loading || u->online || u->launcher)return;
    if(o)release_input(u);
    u->profile.emulated_controller=pair_mode(u)?1:0;
    save_profile_quiet(u);
    g_free(u->socket);g_free(u->socket2);u->socket2=NULL;
    if(pair_mode(u)) {
        const char *mock_dir=g_getenv("PCBLE2GAMEPAD_JOYCON_SOCKET_DIR");g_autofree char *runtime=mock_dir?g_strdup(mock_dir):g_strdup_printf("/run/pcble2gamepad/%u",(unsigned)getuid());
        u->socket=g_build_filename(runtime,"joycon-left.sock",NULL);u->socket2=g_build_filename(runtime,"joycon-right.sock",NULL);
        gtk_widget_set_visible(u->secondary_row,TRUE);gtk_label_set_text(u->hero_title,"Nintendo Joy-Con Pair");
        gtk_label_set_text(u->controller_hint,u->adapter_ids->len<2?"A Joy-Con pair needs two Bluetooth identities. Connect a second adapter to enable this profile.":"Left and right Joy-Con use separate Bluetooth adapters and one combined input profile.");
    } else {
        const char *override=g_getenv("PCBLE2GAMEPAD_PRO_SOCKET");u->socket=override?g_strdup(override):g_strdup_printf("/run/pcble2gamepad/%u/pro.sock",(unsigned)getuid());
        gtk_widget_set_visible(u->secondary_row,FALSE);gtk_label_set_text(u->hero_title,"Nintendo Switch Pro Controller");gtk_label_set_text(u->controller_hint,"One Bluetooth adapter exposes one Classic HID controller. After pairing, press A once on the virtual controller to leave Change Grip/Order.");
    }
    if(u->pro_color_panel)
        gtk_widget_set_visible(
            u->pro_color_panel,
            !pair_mode(u));

    gtk_label_set_text(u->status,"OFFLINE");
    gtk_label_set_text(
        u->peer,
        "Start a Bluetooth session to connect your console.");
    gtk_label_set_text(u->error,"");

    gtk_widget_queue_draw(u->drawing);
    update_controls(u);
    request(u,"status");
}
static void traffic_logs_changed(GObject *o,GParamSpec *p,Ui *u) {
    (void)o;(void)p;if(u->loading)return;
    u->saved_traffic=gtk_switch_get_active(u->traffic_logs);
    app_state_save(u);
    if(u->online)request(u,"logging");
}

static void pro_colors_changed(GObject *object,
                               GParamSpec *pspec,
                               Ui *u) {
    (void)pspec;

    if(u->loading || u->closing || u->closed)
        return;

    GtkColorDialogButton *button=
        GTK_COLOR_DIALOG_BUTTON(object);

    const GdkRGBA *color=
        gtk_color_dialog_button_get_rgba(button);

    if(!color)
        return;

    if(button==u->pro_body_color_button)
        u->pro_body_color=*color;
    else if(button==u->pro_button_color_button)
        u->pro_button_color=*color;
    else if(button==u->pro_left_grip_color_button)
        u->pro_left_grip_color=*color;
    else if(button==u->pro_right_grip_color_button)
        u->pro_right_grip_color=*color;
    else
        return;

    update_pro_svg_colors(u);
    app_state_save(u);
}

static GtkColorDialogButton *make_color_button(const char *title,
                                                const GdkRGBA *color,
                                                Ui *u) {
    (void)title;

    GtkColorDialogButton *button=
        GTK_COLOR_DIALOG_BUTTON(
            gtk_color_dialog_button_new(
                u->color_dialog));

    gtk_color_dialog_button_set_rgba(
        button,
        color);

    g_signal_connect(
        button,
        "notify::rgba",
        G_CALLBACK(pro_colors_changed),
        u);

    return button;
}

static GtkWidget *color_control(const char *title,
                                GtkColorDialogButton *button) {
    GtkWidget *box=
        gtk_box_new(GTK_ORIENTATION_VERTICAL,4);

    GtkWidget *text=label(title,"dim-label");

    gtk_label_set_xalign(GTK_LABEL(text),0.5);
    gtk_widget_set_halign(GTK_WIDGET(button),GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box),text);
    gtk_box_append(GTK_BOX(box),GTK_WIDGET(button));

    return box;
}

static void launch_switch_session(Ui *u,gboolean reconnect) {
    if(u->launcher || u->online)return;

    guint i=gtk_drop_down_get_selected(u->adapters);
    if(i>=u->adapter_ids->len){toast(u,"Select a Bluetooth adapter first");return;}

    if(reconnect) {
        if(pair_mode(u)) {
            toast(u,"Automatic reconnect is currently implemented for Pro Controller first");
            return;
        }
        if(!u->paired_switch_address || !*u->paired_switch_address) {
            toast(u,"No paired Switch saved yet. Use Pair / Sync new Switch first.");
            return;
        }
        if(!paired_adapter_selected(u)) {
            toast(u,"Reconnect requires the Bluetooth adapter used for the original pairing.");
            return;
        }
    }

    guint j=gtk_drop_down_get_selected(u->secondary);
    if(pair_mode(u) &&
       (j>=u->secondary_ids->len ||
        !strcmp(g_ptr_array_index(u->adapter_ids,i),
                g_ptr_array_index(u->secondary_ids,j)))) {
        toast(u,"Choose two distinct Bluetooth adapters for the Joy-Con pair");
        return;
    }

    const char *override=g_getenv("PCBLE2GAMEPAD_RUNNER");
    g_autofree char *exe=g_file_read_link("/proc/self/exe",NULL),
        *build=exe?g_path_get_dirname(exe):NULL,
        *root=build?g_path_get_dirname(build):NULL;
    g_autofree char *checkout=root?g_build_filename(root,"poc","run-classic.sh",NULL):NULL;
    g_autofree char *checkout_backend=build?
        g_build_filename(build,"pcble2gamepad-controller-backend",NULL):NULL;
    g_autofree char *installed_dir=g_path_get_dirname(PCBLE2GAMEPAD_PRO_RUNNER);
    g_autofree char *installed_backend=
        g_build_filename(installed_dir,"pcble2gamepad-controller-backend",NULL);

    /*
     * Runtime privilege is deliberately granted only to the installed,
     * root-owned launcher through Polkit. Never grant passwordless root
     * execution to a launcher writable by the desktop user.
     *
     * Development builds therefore use the installed launcher too. Refuse
     * to silently run an older installed backend when the checkout was
     * rebuilt more recently.
     */
    if(!override &&
       g_file_test(PCBLE2GAMEPAD_PRO_RUNNER,G_FILE_TEST_IS_EXECUTABLE) &&
       checkout_backend &&
       g_file_test(checkout_backend,G_FILE_TEST_IS_EXECUTABLE) &&
       g_file_test(installed_backend,G_FILE_TEST_IS_EXECUTABLE)) {
        GStatBuf checkout_stat,installed_stat;
        if(!g_stat(checkout_backend,&checkout_stat) &&
           !g_stat(installed_backend,&installed_stat) &&
           checkout_stat.st_mtime>installed_stat.st_mtime) {
            toast(u,
                "Installed Bluetooth backend is older than this build. Run: sudo meson install -C build");
            return;
        }
    }

    g_autofree char *runner=override?g_strdup(override):
        g_file_test(PCBLE2GAMEPAD_PRO_RUNNER,G_FILE_TEST_IS_EXECUTABLE)?
            g_strdup(PCBLE2GAMEPAD_PRO_RUNNER):
        checkout&&g_file_test(checkout,G_FILE_TEST_IS_EXECUTABLE)?
            g_strdup(checkout):
            g_strdup(PCBLE2GAMEPAD_PRO_RUNNER);

    if(!g_file_test(runner,G_FILE_TEST_IS_EXECUTABLE)) {
        toast(u,"Bluetooth backend launcher is not installed");
        return;
    }

    g_autoptr(GError)e=NULL;

    g_autofree char *body_color=
        rgba_hex(&u->pro_body_color);
    g_autofree char *button_color=
        rgba_hex(&u->pro_button_color);
    g_autofree char *left_grip_color=
        rgba_hex(&u->pro_left_grip_color);
    g_autofree char *right_grip_color=
        rgba_hex(&u->pro_right_grip_color);

    const char *args[28];
    guint n=0;
    args[n++]="pkexec";
    args[n++]=runner;
    args[n++]=g_ptr_array_index(u->adapter_ids,i);
    args[n++]="--desktop";
    args[n++]="--profile";

    if(pair_mode(u)) {
        args[n++]="joycon-pair";
        args[n++]="--secondary";
        args[n++]=g_ptr_array_index(u->secondary_ids,j);
    } else {
        args[n++]="pro";

        args[n++]="--body-color";
        args[n++]=body_color;

        args[n++]="--button-color";
        args[n++]=button_color;

        args[n++]="--left-grip-color";
        args[n++]=left_grip_color;

        args[n++]="--right-grip-color";
        args[n++]=right_grip_color;
    }

    if(reconnect) {
        args[n++]="--reconnect";
        args[n++]=u->paired_switch_address;
    }

    if(gtk_switch_get_active(u->traffic_logs))
        args[n++]="--verbose";

    args[n]=NULL;

    u->launcher=g_subprocess_newv(args,G_SUBPROCESS_FLAGS_NONE,&e);
    if(!u->launcher) {
        toast(u,e->message);
        return;
    }

    u->refs++;
    g_subprocess_wait_check_async(u->launcher,NULL,launcher_done,u);
    update_controls(u);

    gtk_label_set_text(u->status,reconnect?"RECONNECTING":"WAITING FOR PAIRING");
    gtk_label_set_text(u->error,"");

    if(!reconnect)
        toast(u,"Open Controllers → Change Grip/Order on the Switch for first pairing.");
}

static void start(GtkButton *b,Ui *u) {
    (void)b;

    /*
     * Prefer reconnecting inside the existing privileged backend. This keeps
     * the Bluetooth identity and BlueZ state established during pairing.
     */
    if(u->online && !u->connected) {
        gtk_label_set_text(u->status,"RECONNECTING");
        gtk_label_set_text(u->error,"");
        request(u,"reconnect");
        return;
    }

    launch_switch_session(u,TRUE);
}
static void sync_clicked(GtkButton *b,Ui *u) {
    (void)b;
    launch_switch_session(u,FALSE);
}
static void stop_clicked(GtkButton *b,Ui *u){(void)b;neutral(u);request(u,"stop");}
static gboolean close_window(GtkWindow *w,Ui *u) {
    if(u->closing)return TRUE;
    app_state_save(u);u->closing=TRUE;g_source_remove(u->timer);gtk_widget_set_visible(GTK_WIDGET(w),FALSE);
    if(u->launcher && !u->online)g_subprocess_send_signal(u->launcher,SIGTERM);
    request(u,"stop");return TRUE;
}
static void copy_logs(GtkButton *b,Ui *u){(void)b;GtkTextIter a,z;gtk_text_buffer_get_bounds(u->logs,&a,&z);char *text=gtk_text_buffer_get_text(u->logs,&a,&z,FALSE);gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(u->window)),text);g_free(text);toast(u,"Diagnostics copied");}
static void defaults(GtkButton *b,Ui *u){neutral(u);int controller=u->profile.emulated_controller;input_profile_defaults(&u->profile,GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"azerty")));u->profile.emulated_controller=controller;refresh_bindings(u);toast(u,"Defaults restored; save to keep them");}
static GtkWidget *button(const char *text,GCallback callback,Ui *u){GtkWidget *b=gtk_button_new_with_label(text);g_signal_connect(b,"clicked",callback,u);return b;}
static void activate(GtkApplication *app,gpointer unused) {
    (void)unused;GtkWindow *existing=gtk_application_get_active_window(app);if(existing){gtk_window_present(existing);return;}
    Ui *u=g_new0(Ui,1);u->refs=1;u->app=app;u->learn_key=u->learn_pad=-1;u->joystick_count=-1;
    u->keys=g_hash_table_new(g_direct_hash,g_direct_equal);
    u->adapter_ids=g_ptr_array_new_with_free_func(g_free);
    u->adapter_addresses=g_ptr_array_new_with_free_func(g_free);
    u->secondary_ids=g_ptr_array_new_with_free_func(g_free);
    u->secondary_addresses=g_ptr_array_new_with_free_func(g_free);
    u->device_ids=g_ptr_array_new();
    g_autofree char *config_root=g_build_filename(g_get_user_config_dir(),"pcble2gamepad",NULL);
    g_mkdir_with_parents(config_root,0700);
    u->config_dir=g_build_filename(config_root,"profiles",NULL);g_mkdir_with_parents(u->config_dir,0700);
    /*
     * settings.ini is now migration-only. All persistent application state is
     * stored in the active profile.
     */
    u->settings_path=
        g_build_filename(
            config_root,
            "settings.ini",
            NULL);

    g_autofree char *legacy_profile=
        legacy_profile_name(u);

    g_autofree char *active_profile=
        active_profile_name(u);

    const char *initial_profile=
        active_profile?active_profile:"Default";

    if(legacy_profile && *legacy_profile) {
        g_autofree char *legacy_path=
            profile_path(u,legacy_profile);

        if(g_file_test(
                legacy_path,
                G_FILE_TEST_EXISTS))
            initial_profile=legacy_profile;
    }

    u->profile_name=
        g_strdup(initial_profile);

    profile_state_defaults(u);
    input_profile_defaults(&u->profile,FALSE);

    g_autofree char *path=
        profile_path(
            u,
            u->profile_name);

    if(!g_file_test(
            path,
            G_FILE_TEST_EXISTS) &&
       strcmp(
            u->profile_name,
            "Default")) {
        g_free(u->profile_name);
        u->profile_name=g_strdup("Default");

        g_clear_pointer(&path,g_free);
        path=profile_path(
            u,
            u->profile_name);
    }

    if(g_file_test(path,G_FILE_TEST_EXISTS))
        input_profile_load(
            &u->profile,
            path,
            NULL);
    else
        input_profile_save(
            &u->profile,
            path,
            NULL);

    gboolean had_profile_state=
        profile_state_load_path(
            u,
            path);

    if(!had_profile_state &&
       g_file_test(
            u->settings_path,
            G_FILE_TEST_EXISTS))
        legacy_state_load(u);

    /*
     * Write the migrated/current state into the profile before deleting the
     * legacy file.
     */
    if(profile_state_save_path(
            u,
            path,
            TRUE)) {
        mark_profile_active(
            u,
            u->profile_name);

        if(g_file_test(
                u->settings_path,
                G_FILE_TEST_EXISTS))
            g_remove(u->settings_path);
    }

    u->pro_svg=
        load_controller_svg(
            "pro-controller.svg");

    u->joycon_left_svg=
        load_controller_svg(
            "joycon-left.svg");

    u->joycon_right_svg=
        load_controller_svg(
            "joycon-right.svg");

    update_pro_svg_colors(u);

    /*
     * One shared dialog object avoids per-button lifetime races while the
     * application window is being destroyed.
     */
    u->color_dialog=gtk_color_dialog_new();

    gtk_color_dialog_set_title(
        u->color_dialog,
        "Controller color");

    gtk_color_dialog_set_with_alpha(
        u->color_dialog,
        FALSE);
    const char *override=g_getenv("PCBLE2GAMEPAD_PRO_SOCKET");u->socket=override?g_strdup(override):g_strdup_printf("/run/pcble2gamepad/%u/pro.sock",(unsigned)getuid());
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    u->window=GTK_WINDOW(adw_application_window_new(app));gtk_window_set_title(u->window,"pcble2gamepad — Controller Studio");gtk_window_set_default_size(u->window,1120,840);
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),ADW_COLOR_SCHEME_PREFER_DARK);
    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,".studio-nav { background: alpha(@window_fg_color,.035); padding: 12px; } .studio-nav row { border-radius: 10px; margin: 3px 0; padding: 8px; } .connection-badge { color: #54d6ac; font-size: 12px; font-weight: 800; letter-spacing: 1px; } .hero { background: alpha(@window_fg_color,.025); border: 1px solid alpha(@window_fg_color,.07); border-radius: 22px; padding: 18px; } .title-1 { font-size: 30px; } .binding { min-width: 105px; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(u->window)),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(css);
    GtkWidget *outer=gtk_box_new(GTK_ORIENTATION_VERTICAL,0),*header=adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),adw_window_title_new("pcble2gamepad","Controller Studio"));gtk_box_append(GTK_BOX(outer),header);
    GtkWidget *profile_bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);u->profile_names=gtk_string_list_new(NULL);u->profiles=GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(u->profile_names),NULL));
    gtk_widget_set_tooltip_text(GTK_WIDGET(u->profiles),"Saved input profile");gtk_box_append(GTK_BOX(profile_bar),GTK_WIDGET(u->profiles));
    gtk_box_append(GTK_BOX(profile_bar),button("Save",G_CALLBACK(profile_saved),u));gtk_box_append(GTK_BOX(profile_bar),button("Save as…",G_CALLBACK(new_profile),u));adw_header_bar_pack_end(ADW_HEADER_BAR(header),profile_bar);
    GtkWidget *content=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);gtk_widget_set_vexpand(content,TRUE);gtk_box_append(GTK_BOX(outer),content);
    u->stack=GTK_STACK(gtk_stack_new());gtk_stack_set_transition_type(u->stack,GTK_STACK_TRANSITION_TYPE_CROSSFADE);gtk_widget_set_hexpand(GTK_WIDGET(u->stack),TRUE);
    GtkWidget *sidebar=gtk_stack_sidebar_new();gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar),u->stack);gtk_widget_set_size_request(sidebar,190,-1);gtk_widget_add_css_class(sidebar,"studio-nav");gtk_box_append(GTK_BOX(content),sidebar);gtk_box_append(GTK_BOX(content),GTK_WIDGET(u->stack));
    u->toast=ADW_TOAST_OVERLAY(adw_toast_overlay_new());adw_toast_overlay_set_child(u->toast,outer);adw_application_window_set_content(ADW_APPLICATION_WINDOW(u->window),GTK_WIDGET(u->toast));
    GtkWidget *box=page(u,"overview","Connection","Choose the controller presented by your PC, connect it to Switch 2, then route keyboard or gamepad input.");
    GtkWidget *hero=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);gtk_widget_add_css_class(hero,"hero");
    u->status=GTK_LABEL(label("OFFLINE","connection-badge"));gtk_box_append(GTK_BOX(hero),GTK_WIDGET(u->status));u->hero_title=GTK_LABEL(label("Nintendo Switch Pro Controller","title-2"));gtk_box_append(GTK_BOX(hero),GTK_WIDGET(u->hero_title));
    u->peer=GTK_LABEL(label("Start a Bluetooth session to connect your console.","dim-label"));gtk_box_append(GTK_BOX(hero),GTK_WIDGET(u->peer));
    u->drawing=gtk_drawing_area_new();
    gtk_widget_set_size_request(u->drawing,-1,260);
    gtk_widget_set_focusable(u->drawing,TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(u->drawing),
        draw,
        u,
        NULL);
    gtk_box_append(GTK_BOX(hero),u->drawing);

    u->pro_color_panel=
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL,24);

    gtk_widget_set_halign(
        u->pro_color_panel,
        GTK_ALIGN_CENTER);

    u->pro_body_color_button=
        make_color_button(
            "Pro Controller body color",
            &u->pro_body_color,
            u);

    u->pro_button_color_button=
        make_color_button(
            "Pro Controller button color",
            &u->pro_button_color,
            u);

    u->pro_left_grip_color_button=
        make_color_button(
            "Pro Controller left grip color",
            &u->pro_left_grip_color,
            u);

    u->pro_right_grip_color_button=
        make_color_button(
            "Pro Controller right grip color",
            &u->pro_right_grip_color,
            u);

    gtk_box_append(
        GTK_BOX(u->pro_color_panel),
        color_control(
            "Body",
            u->pro_body_color_button));

    gtk_box_append(
        GTK_BOX(u->pro_color_panel),
        color_control(
            "Buttons",
            u->pro_button_color_button));

    gtk_box_append(
        GTK_BOX(u->pro_color_panel),
        color_control(
            "Left grip",
            u->pro_left_grip_color_button));

    gtk_box_append(
        GTK_BOX(u->pro_color_panel),
        color_control(
            "Right grip",
            u->pro_right_grip_color_button));

    gtk_box_append(
        GTK_BOX(hero),
        u->pro_color_panel);

    u->metrics=GTK_LABEL(
        label(
            "Live input preview · no physical controller required",
            "dim-label"));

    gtk_box_append(
        GTK_BOX(hero),
        GTK_WIDGET(u->metrics));

    gtk_box_append(GTK_BOX(box),hero);
    AdwPreferencesGroup *g=group(box,"Bluetooth session","Starting a session requests administrator authentication and temporarily pauses other Bluetooth services.");
    const char *controllers[]={"Nintendo Switch Pro Controller","Nintendo Joy-Con Pair",NULL};u->controllers=GTK_DROP_DOWN(gtk_drop_down_new_from_strings(controllers));row(g,"Emulated controller","Sony and Microsoft profiles can be added to the same controller catalog later.",GTK_WIDGET(u->controllers));
    u->adapter_names=gtk_string_list_new(NULL);u->adapters=GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(u->adapter_names),NULL));gtk_widget_set_size_request(GTK_WIDGET(u->adapters),280,-1);
    row(g,"Primary adapter","Pro Controller, or left Joy-Con. Choose by address; hci numbers can change after reboot.",GTK_WIDGET(u->adapters));
    u->secondary_names=gtk_string_list_new(NULL);u->secondary=GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(u->secondary_names),NULL));gtk_widget_set_size_request(GTK_WIDGET(u->secondary),280,-1);
    u->secondary_row=row(g,"Right Joy-Con adapter","A pair requires a second, distinct Classic Bluetooth identity.",GTK_WIDGET(u->secondary));gtk_widget_set_visible(u->secondary_row,FALSE);
    u->controller_hint=GTK_LABEL(label("Pair / Sync works from Change Grip/Order. Persistent reconnect is experimental because the Switch requests a temporary No Bonding key.","dim-label"));gtk_box_append(GTK_BOX(box),GTK_WIDGET(u->controller_hint));

    u->paired_console=GTK_LABEL(label("Not paired yet","dim-label"));
    row(g,"Paired Switch","Stored after the first successful pairing. Reconnect uses the same Bluetooth adapter identity and console address.",GTK_WIDGET(u->paired_console));
    update_paired_console(u);

    GtkWidget *actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    u->start=button("Reconnect paired Switch (experimental)",G_CALLBACK(start),u);
    gtk_widget_add_css_class(u->start,"suggested-action");
    u->sync=button("Pair / Sync new Switch",G_CALLBACK(sync_clicked),u);
    u->stop=button("Stop session",G_CALLBACK(stop_clicked),u);
    gtk_widget_set_sensitive(u->stop,FALSE);

    gtk_box_append(GTK_BOX(actions),u->start);
    gtk_box_append(GTK_BOX(actions),u->sync);
    gtk_box_append(GTK_BOX(actions),u->stop);
    gtk_box_append(GTK_BOX(actions),button("Refresh adapters",G_CALLBACK(adapters_scan),u));
    gtk_box_append(GTK_BOX(box),actions);
    u->error=GTK_LABEL(label("","error"));gtk_box_append(GTK_BOX(box),GTK_WIDGET(u->error));
    g=group(box,"Input routing","Pair / Sync is validated. Reconnect remains available for diagnostics while persistent bonding is unresolved.");
    const char *sources[]={"Keyboard","PC controller",NULL};u->source=GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sources));gtk_drop_down_set_selected(u->source,u->saved_source);row(g,"Input source",NULL,GTK_WIDGET(u->source));
    u->arm=gtk_switch_new();gtk_switch_set_active(GTK_SWITCH(u->arm),u->saved_arm);row(g,"Enable input","Remembered between launches. Escape pauses keyboard input.",u->arm);
    u->source_hint=GTK_LABEL(label(u->saved_source==0?"Keyboard input works while this window is focused. Escape pauses input.":"Standard SDL gamepad mapping. Customize buttons and stick settings below.","dim-label"));gtk_box_append(GTK_BOX(box),GTK_WIDGET(u->source_hint));
    box=page(u,"keyboard","Keyboard bindings","Map keys to controller buttons and both sticks. Duplicate bindings are moved to the new action.");
    u->capture_hint=label("Click a binding to change it. Escape cancels; Backspace clears it.","dim-label");gtk_box_append(GTK_BOX(box),u->capture_hint);
    GtkWidget *presets=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);GtkWidget *wasd=button("WASD defaults",G_CALLBACK(defaults),u),*azerty=button("ZQSD defaults",G_CALLBACK(defaults),u);g_object_set_data(G_OBJECT(azerty),"azerty",GINT_TO_POINTER(1));gtk_box_append(GTK_BOX(presets),wasd);gtk_box_append(GTK_BOX(presets),azerty);gtk_box_append(GTK_BOX(box),presets);
    g=group(box,"Buttons",NULL);
    for(int i=0;i<INPUT_ACTIONS;i++){if(i==18)g=group(box,"Stick directions",NULL);GtkWidget *b=button("",G_CALLBACK(bind_key),u);u->key_buttons[i]=GTK_BUTTON(b);g_object_set_data(G_OBJECT(b),"index",GINT_TO_POINTER(i));gtk_widget_add_css_class(b,"binding");row(g,input_action_names[i],NULL,b);}
    box=page(u,"controller","PC controller","Forward a USB or Bluetooth gamepad through the selected emulated controller. No physical Nintendo controller is needed.");
    g=group(box,"Source device","SDL standard gamepad mapping supports Xbox, PlayStation and many compatible controllers.");
    u->device_names=gtk_string_list_new(NULL);u->devices=GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(u->device_names),NULL));row(g,"Connected controller","Devices update automatically when plugged in or removed.",GTK_WIDGET(u->devices));
    g=group(box,"Button mapping","Default mapping follows button labels. Click a binding and press the source button or trigger to change it.");
    for(int i=0;i<INPUT_BUTTONS;i++){GtkWidget *b=button("",G_CALLBACK(bind_pad),u);u->pad_buttons[i]=GTK_BUTTON(b);g_object_set_data(G_OBJECT(b),"index",GINT_TO_POINTER(i));gtk_widget_add_css_class(b,"binding");row(g,input_action_names[i],NULL,b);}
    box=page(u,"settings","Settings","Fine-tune input response and configure the emulated controller.");
    g=group(box,"Stick response","A radial dead zone avoids drift while preserving direction.");
    u->deadzone=GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,50,1));gtk_widget_set_size_request(GTK_WIDGET(u->deadzone),230,-1);gtk_scale_set_digits(u->deadzone,0);row(g,"Dead zone (%)",NULL,GTK_WIDGET(u->deadzone));
    u->sensitivity=GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,25,200,5));gtk_widget_set_size_request(GTK_WIDGET(u->sensitivity),230,-1);gtk_scale_set_digits(u->sensitivity,0);row(g,"Sensitivity (%)",NULL,GTK_WIDGET(u->sensitivity));
    const char *axes[]={"Invert left X","Invert left Y","Invert right X","Invert right Y"};
    for(int i=0;i<4;i++){u->invert[i]=GTK_SWITCH(gtk_switch_new());row(g,axes[i],NULL,GTK_WIDGET(u->invert[i]));g_signal_connect(u->invert[i],"notify::active",G_CALLBACK(settings_changed),u);}
    u->swap=GTK_SWITCH(gtk_switch_new());row(g,"Swap sticks","Swap the two physical gamepad sticks.",GTK_WIDGET(u->swap));

    g=group(box,"Button layout","Translate SDL gamepad buttons to the Nintendo face-button convention.");
    u->swap_face=GTK_SWITCH(gtk_switch_new());
    row(g,"Nintendo face-button layout","Swap A/B and X/Y for PC gamepads. Keyboard bindings keep their explicit Nintendo labels.",GTK_WIDGET(u->swap_face));

    g=group(box,"Startup and routing","Controller Studio remembers these application preferences automatically.");
    u->auto_select=GTK_SWITCH(gtk_switch_new());gtk_switch_set_active(u->auto_select,u->saved_auto_select);
    row(g,"Auto-select PC controller","Prefer the last physical controller by GUID; otherwise select the first available gamepad.",GTK_WIDGET(u->auto_select));

    g=group(box,"Focus and recovery",NULL);u->background=GTK_SWITCH(gtk_switch_new());row(g,"Gamepad input in background","Keyboard input always requires window focus.",GTK_WIDGET(u->background));
    row(g,"Automatic release","Buttons and sticks return to neutral within 500 ms if input updates stop.",NULL);
    row(g,"Session lifetime","Stopping or closing this window stops the backend and restores normal Bluetooth.",NULL);
    box=page(u,"diagnostics","Diagnostics","Live session events, association requests and HID initialization. Simulation is always labeled.");
    u->traffic_logs=GTK_SWITCH(gtk_switch_new());gtk_switch_set_active(u->traffic_logs,u->saved_traffic);row(group(box,"Logging",NULL),"Detailed HID traffic","Show repetitive hid_rx packets and periodic status lines. Remembered between launches.",GTK_WIDGET(u->traffic_logs));
    gtk_box_append(GTK_BOX(box),button("Copy diagnostics",G_CALLBACK(copy_logs),u));GtkWidget *view=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(view),FALSE);gtk_text_view_set_monospace(GTK_TEXT_VIEW(view),TRUE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_WORD_CHAR);u->logs=gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));gtk_widget_set_size_request(view,-1,430);margin(view,10);gtk_box_append(GTK_BOX(box),view);
    row(group(box,"Current scope",NULL),"Nintendo controller profiles","Switch Pro Controller is verified on Switch 2. Joy-Con (L/R) wire formats and dual-adapter routing are implemented but cannot be hardware-tested until a second adapter is connected. Sony and Microsoft profiles remain future additions.",NULL);
    refresh_bindings(u);profile_scan(u);devices_scan(u);adapters_scan(NULL,u);
    g_signal_connect(u->profiles,"notify::selected",G_CALLBACK(profile_selected),u);
    g_signal_connect(u->devices,"notify::selected",G_CALLBACK(device_selected),u);
    g_signal_connect(u->source,"notify::selected",G_CALLBACK(source_changed),u);
    g_signal_connect(u->controllers,"notify::selected",G_CALLBACK(controller_selected),u);
    g_signal_connect(u->adapters,"notify::selected",G_CALLBACK(adapter_selection_changed),u);
    g_signal_connect(u->secondary,"notify::selected",G_CALLBACK(adapter_selection_changed),u);
    g_signal_connect(u->arm,"notify::active",G_CALLBACK(armed_changed),u);g_signal_connect(u->window,"notify::is-active",G_CALLBACK(focus_changed),u);g_signal_connect(u->window,"close-request",G_CALLBACK(close_window),u);
    g_signal_connect(u->deadzone,"value-changed",G_CALLBACK(scale_changed),u);g_signal_connect(u->sensitivity,"value-changed",G_CALLBACK(scale_changed),u);g_signal_connect(u->swap,"notify::active",G_CALLBACK(settings_changed),u);g_signal_connect(u->background,"notify::active",G_CALLBACK(settings_changed),u);g_signal_connect(u->swap_face,"notify::active",G_CALLBACK(settings_changed),u);
    g_signal_connect(u->auto_select,"notify::active",G_CALLBACK(auto_select_changed),u);
    g_signal_connect(u->traffic_logs,"notify::active",G_CALLBACK(traffic_logs_changed),u);
    GtkEventController *keys=gtk_event_controller_key_new();gtk_event_controller_set_propagation_phase(keys,GTK_PHASE_CAPTURE);g_signal_connect(keys,"key-pressed",G_CALLBACK(key_pressed),u);g_signal_connect(keys,"key-released",G_CALLBACK(key_released),u);gtk_widget_add_controller(GTK_WIDGET(u->window),keys);
    controller_selected(NULL,NULL,u);u->timer=g_timeout_add(16,tick,u);request(u,"status");gtk_window_present(u->window);
}
int main(int argc,char **argv){AdwApplication *app=adw_application_new("io.github.wozt.pcble2gamepad",G_APPLICATION_DEFAULT_FLAGS);g_signal_connect(app,"activate",G_CALLBACK(activate),NULL);int result=g_application_run(G_APPLICATION(app),argc,argv);g_object_unref(app);SDL_Quit();return result;}
