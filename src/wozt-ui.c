#include "wozt-ui.h"
#include "config.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
/* GTK4/X11: expose actual WM_CLASS and EWMH icon independently of the
 * in-window title bar. Wayland retains GtkApplication desktop identity. */
#if defined(PCBLE2GAMEPAD_HAVE_X11) && defined(GDK_WINDOWING_X11)
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#endif

#define WOZT_APP_ID "io.github.wozt.pcble2gamepad"

static const char *const palette_keys[] = {
    "background", "panel", "entry", "border", "text", "muted",
    "accent", "button", "hover", "active", "negative"
};

typedef struct {
    char *id, *name;
    char *colors[G_N_ELEMENTS(palette_keys)];
} WoztTheme;

typedef struct {
    char *id, *name;
    JsonObject *translations;
} WoztLanguage;

typedef struct {
    GWeakRef object;
    char *property, *original, *rendered;
} WoztBinding;

struct WoztUi {
    GtkWindow *window; /* Owned by the application's existing UI lifecycle. */
    GdkDisplay *display;
    GtkCssProvider *css;
    GtkStringList *theme_names, *language_names;
    GtkDropDown *theme_selector, *language_selector;
    GtkWidget *header_actions, *columns, *sidebar, *compact_navigation;
    GtkStack *stack;
    GPtrArray *themes, *languages, *bindings;
    char *preferences_path, *theme_id, *language_id, *resources;
    gboolean loading;
    guint last_width_class, last_language_update;
};

static void theme_free(gpointer p) {
    WoztTheme *theme=p;
    g_free(theme->id); g_free(theme->name);
    for(guint i=0;i<G_N_ELEMENTS(theme->colors);i++)g_free(theme->colors[i]);
    g_free(theme);
}
static void language_free(gpointer p) {
    WoztLanguage *language=p;
    g_free(language->id);g_free(language->name);
    if(language->translations)json_object_unref(language->translations);
    g_free(language);
}
static void binding_free(gpointer p) {
    WoztBinding *binding=p;
    g_weak_ref_clear(&binding->object);
    g_free(binding->property);g_free(binding->original);
    g_free(binding->rendered);g_free(binding);
}
static gboolean valid_id(const char *value) {
    if(!value || !*value)return FALSE;
    for(const char *p=value;*p;p++)
        if(!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p!='-' && *p!='_')
            return FALSE;
    return TRUE;
}
static gboolean valid_hex(const char *value) {
    if(!value || strlen(value)!=7 || value[0]!='#')return FALSE;
    for(int i=1;i<7;i++)if(!g_ascii_isxdigit(value[i]))return FALSE;
    return TRUE;
}
static gint compare_filename(gconstpointer aa,gconstpointer bb) {
    const char *a=*(char *const *)aa,*b=*(char *const *)bb;
    return g_strcmp0(a,b);
}
static GPtrArray *sorted_json_files(const char *directory) {
    GPtrArray *files=g_ptr_array_new_with_free_func(g_free);
    GDir *dir=g_dir_open(directory,0,NULL);
    if(!dir)return files;
    const char *name;
    while((name=g_dir_read_name(dir)))
        if(g_str_has_suffix(name,".json"))g_ptr_array_add(files,g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort(files,compare_filename);
    return files;
}
static JsonObject *read_json_object(const char *path) {
    g_autoptr(JsonParser) parser=json_parser_new();
    GError *error=NULL;
    if(!json_parser_load_from_file(parser,path,&error)) {
        g_warning("WOZT JSON %s: %s",path,error->message);
        g_clear_error(&error);return NULL;
    }
    JsonNode *root=json_parser_get_root(parser);
    if(!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("WOZT JSON %s: object expected",path);return NULL;
    }
    return json_object_ref(json_node_get_object(root));
}
static WoztTheme *read_theme(const char *path) {
    JsonObject *object=read_json_object(path);
    if(!object)return NULL;
    const char *id=json_object_get_string_member_with_default(object,"id","");
    const char *name=json_object_get_string_member_with_default(object,"name","");
    JsonObject *colors=json_object_get_object_member(object,"colors");
    WoztTheme *theme=NULL;
    if(valid_id(id) && *name && colors) {
        theme=g_new0(WoztTheme,1);
        theme->id=g_strdup(id);theme->name=g_strdup(name);
        for(guint i=0;i<G_N_ELEMENTS(palette_keys);i++) {
            const char *hex=json_object_get_string_member_with_default(
                colors,palette_keys[i],"");
            if(!valid_hex(hex)) {theme_free(theme);theme=NULL;break;}
            theme->colors[i]=g_strdup(hex);
        }
    }
    if(!theme)g_warning("Invalid WOZT theme skipped: %s",path);
    json_object_unref(object);
    return theme;
}
static WoztLanguage *read_language(const char *path) {
    JsonObject *object=read_json_object(path);
    if(!object)return NULL;
    const char *id=json_object_get_string_member_with_default(object,"id","");
    const char *name=json_object_get_string_member_with_default(object,"name","");
    JsonObject *translations=json_object_get_object_member(object,"translations");
    WoztLanguage *language=NULL;
    if(valid_id(id) && *name && translations) {
        language=g_new0(WoztLanguage,1);
        language->id=g_strdup(id);language->name=g_strdup(name);
        language->translations=json_object_ref(translations);
    } else g_warning("Invalid WOZT language skipped: %s",path);
    json_object_unref(object);
    return language;
}
static void upsert_theme(GPtrArray *themes,WoztTheme *theme) {
    if(!theme)return;
    for(guint i=0;i<themes->len;i++) {
        WoztTheme *old=g_ptr_array_index(themes,i);
        if(!strcmp(old->id,theme->id)) {
            theme_free(old);g_ptr_array_index(themes,i)=theme;return;
        }
    }
    g_ptr_array_add(themes,theme);
}
static void upsert_language(GPtrArray *languages,WoztLanguage *language) {
    if(!language)return;
    for(guint i=0;i<languages->len;i++) {
        WoztLanguage *old=g_ptr_array_index(languages,i);
        if(!strcmp(old->id,language->id)) {
            language_free(old);g_ptr_array_index(languages,i)=language;return;
        }
    }
    g_ptr_array_add(languages,language);
}
static void load_directory(WoztUi *ui,const char *directory,gboolean themes) {
    g_autoptr(GPtrArray) filenames=sorted_json_files(directory);
    for(guint i=0;i<filenames->len;i++) {
        g_autofree char *path=g_build_filename(directory,
            g_ptr_array_index(filenames,i),NULL);
        if(themes)upsert_theme(ui->themes,read_theme(path));
        else upsert_language(ui->languages,read_language(path));
    }
}
static void save_preferences(WoztUi *ui) {
    g_autoptr(GKeyFile) file=g_key_file_new();
    if(ui->theme_id)g_key_file_set_string(file,"Appearance","theme",ui->theme_id);
    if(ui->language_id)g_key_file_set_string(file,"Appearance","language",ui->language_id);
    GError *error=NULL;
    if(!g_key_file_save_to_file(file,ui->preferences_path,&error)) {
        g_warning("Cannot save WOZT preferences: %s",error->message);
        g_clear_error(&error);
    }
}
static const WoztTheme *selected_theme(WoztUi *ui) {
    for(guint i=0;i<ui->themes->len;i++) {
        WoztTheme *theme=g_ptr_array_index(ui->themes,i);
        if(!g_strcmp0(ui->theme_id,theme->id))return theme;
    }
    return ui->themes->len?g_ptr_array_index(ui->themes,0):NULL;
}
static const WoztLanguage *selected_language(WoztUi *ui) {
    for(guint i=0;i<ui->languages->len;i++) {
        WoztLanguage *language=g_ptr_array_index(ui->languages,i);
        if(!g_strcmp0(ui->language_id,language->id))return language;
    }
    return NULL;
}
static const char *translated(WoztUi *ui,const char *english) {
    const WoztLanguage *language=selected_language(ui);
    if(!language || !language->translations || !english)return english;
    JsonNode *entry=json_object_get_member(language->translations,english);
    if(entry && JSON_NODE_HOLDS_VALUE(entry) &&
       json_node_get_value_type(entry)==G_TYPE_STRING) {
        const char *value=json_node_get_string(entry);
        if(value && *value)return value;
    }
    return english;
}
const char *wozt_ui_translate(WoztUi *ui,const char *english) {
    return ui?translated(ui,english):english;
}
static void apply_theme(WoztUi *ui) {
    const WoztTheme *theme=selected_theme(ui);
    if(!theme)return;
    const char *const *p=(const char *const *)theme->colors;
    g_autofree char *css=g_strdup_printf(
        "window.wozt-root, .wozt-root { background-color:%s; color:%s; border-radius:0; }"
        ".wozt-root * { font-size:11px; }"
        ".wozt-root .wozt-bar { background-color:%s; border-bottom:1px solid %s; padding:2px 6px; }"
        ".wozt-root .wozt-brand { color:%s; font-size:12px; font-weight:700; }"
        ".wozt-root .studio-nav { background-color:%s; padding:2px; }"
        ".wozt-root .studio-nav row { padding:0; margin:0; border-radius:0; }"
        ".wozt-root .hero { background-color:%s; border:1px solid %s; border-radius:0; padding:4px; }"
        ".wozt-root .title-1, .wozt-root .title-2 { color:%s; font-size:12px; font-weight:700; }"
        ".wozt-root .connection-badge { color:%s; font-size:11px; font-weight:700; }"
        ".wozt-root .dim-label { color:%s; }"
        ".wozt-root button { background-color:%s; color:%s; border:1px solid %s;"
        " border-radius:0; min-height:17px; padding:1px 5px; }"
        ".wozt-root button:hover { background-color:%s; }"
        ".wozt-root button:active, .wozt-root button:checked { background-color:%s; }"
        ".wozt-root button:focus-visible, .wozt-root entry:focus-visible { outline-color:%s; }"
        ".wozt-root button.suggested-action { background-color:%s; color:%s; }"
        ".wozt-root .wozt-window-button { min-width:24px; min-height:22px; padding:1px; border-radius:0; }"
        ".wozt-root .wozt-close:hover { background-color:%s; }"
        ".wozt-root entry, .wozt-root dropdown, .wozt-root spinbutton {"
        " background-color:%s; color:%s; border:1px solid %s; border-radius:0;"
        " min-height:17px; padding:0 3px; }"
        ".wozt-root preferencesgroup dropdown, .wozt-root .card dropdown { border:0; background-color:transparent; }"
        ".wozt-root frame, .wozt-root preferencesgroup, .wozt-root .card {"
        " border-radius:0; border-color:%s; margin:0; padding:1px; }"
        ".wozt-root .binding { min-width:72px; }"
        /* Override oversized libadwaita row padding while keeping its
         * accessibility, callbacks and the seven existing theme colors. */
        ".wozt-root .wozt-compact-row { min-height:24px; padding:0 3px; margin:0; border-radius:0; }"
        ".wozt-root .wozt-compact-row > box { min-height:0; padding:0; margin:0; }"
        ".wozt-root preferencesgroup { margin:0; padding:0; }"
        ".wozt-root .wozt-dashboard-section { margin:0; padding:0; }"
        ".wozt-root .wozt-dashboard-section > box { margin:0; padding:0; }"
        /* GTK4/libadwaita: flatten *both* dropdown boundaries and all
         * inherited rounded backgrounds without changing palette data. */
        ".wozt-root * { border-radius:0; }"
        ".wozt-root .wozt-bar dropdown { border:0; background-color:transparent; box-shadow:none; padding:0; }"
        ".wozt-root .wozt-bar dropdown > button { border-radius:0; box-shadow:none; }"
        ".wozt-root .wozt-bar dropdown > button > box { border-radius:0; }"
        ".wozt-root dropdown, .wozt-root popover, .wozt-root switch, .wozt-root trough, .wozt-root slider, .wozt-root row, .wozt-root frame, .wozt-root .card { border-radius:0; box-shadow:none; }"
        ".wozt-root preferencesgroup dropdown, .wozt-root .card dropdown { border:0; background-color:transparent; }"
        "popover > contents, tooltip, dialog { border-radius:0; }"
        ".wozt-root .error { color:%s; }"
        "popover > contents, tooltip, dialog { background-color:%s; color:%s; }"
        "popover button:hover { background-color:%s; }",
        p[0],p[4], p[1],p[3],p[6], p[1], p[1],p[3],p[6],p[6],p[5],
        p[7],p[4],p[3],p[8],p[9],p[6],p[9],p[4],p[10],
        p[2],p[4],p[3],p[3],p[10],p[1],p[4],p[8]);
    /* Widget colors must come from the active JSON palette, never the
     * default libadwaita blue (especially switch:checked and scale). */
    g_autofree char *controls_css=g_strdup_printf(
        ".wozt-root switch { background-color:%s; border:1px solid %s; border-radius:0; }"
        ".wozt-root switch:checked { background-color:%s; border-color:%s; }"
        ".wozt-root switch > slider { background-color:%s; border:1px solid %s; border-radius:0; box-shadow:none; }"
        ".wozt-root switch:checked > slider { background-color:%s; }"
        ".wozt-root scale trough { background-color:%s; border:1px solid %s; border-radius:0; min-height:5px; }"
        ".wozt-root scale highlight { background-color:%s; border-radius:0; }"
        ".wozt-root scale slider { background-color:%s; border:1px solid %s; border-radius:0; min-width:12px; min-height:12px; box-shadow:none; }"
        ".wozt-root scale slider:hover { background-color:%s; }"
        ".wozt-root scale:focus-within slider { outline:1px solid %s; }",
        p[7],p[3],p[9],p[6],p[4],p[3],p[4],
        p[2],p[3],p[6],p[6],p[3],p[8],p[6]);
    g_autofree char *complete_css=g_strconcat(css,controls_css,NULL);
    gtk_css_provider_load_from_string(ui->css,complete_css);
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
        !strcmp(theme->id,"light") ? ADW_COLOR_SCHEME_FORCE_LIGHT :
                                       ADW_COLOR_SCHEME_FORCE_DARK);
}

void wozt_ui_bind_text(WoztUi *ui,GObject *object,
                       const char *property,const char *english) {
    if(!ui || !object || !property || !english)return;
    WoztBinding *binding=g_new0(WoztBinding,1);
    g_weak_ref_init(&binding->object,object);
    binding->property=g_strdup(property);
    binding->original=g_strdup(english);
    g_ptr_array_add(ui->bindings,binding);
}
static void refresh_translations(WoztUi *ui);
static gboolean binding_exists(WoztUi *ui,GObject *object,const char *property) {
    for(guint i=0;i<ui->bindings->len;i++) {
        WoztBinding *b=g_ptr_array_index(ui->bindings,i);
        GObject *bound=g_weak_ref_get(&b->object);
        if(bound==object && !g_strcmp0(b->property,property)) {
            if(bound)g_object_unref(bound);
            return TRUE;
        }
        if(bound)g_object_unref(bound);
    }
    return FALSE;
}
static void maybe_bind_property(WoztUi *ui,GObject *object,const char *property) {
    if(!ui || !object || !property || !*property)return;
    if(binding_exists(ui,object,property))return;
    GParamSpec *pspec=g_object_class_find_property(G_OBJECT_GET_CLASS(object),property);
    if(!pspec || !(pspec->flags&G_PARAM_READABLE) || !(pspec->flags&G_PARAM_WRITABLE) ||
       !G_IS_PARAM_SPEC_STRING(pspec))return;
    char *value=NULL;
    g_object_get(object,property,&value,NULL);
    if(value && *value)wozt_ui_bind_text(ui,object,property,value);
    g_free(value);
}
static void autobind_widget(WoztUi *ui,GtkWidget *widget) {
    if(!ui || !widget)return;
    GObject *object=G_OBJECT(widget);
    maybe_bind_property(ui,object,"label");
    maybe_bind_property(ui,object,"title");
    maybe_bind_property(ui,object,"subtitle");
    maybe_bind_property(ui,object,"description");
    maybe_bind_property(ui,object,"tooltip-text");
    maybe_bind_property(ui,object,"placeholder-text");
    for(GtkWidget *child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))
        autobind_widget(ui,child);
}
void wozt_ui_autobind_tree(WoztUi *ui,GtkWidget *root) {
    autobind_widget(ui,root);
    refresh_translations(ui);
}
static void refresh_translations(WoztUi *ui) {
    for(guint i=0;i<ui->bindings->len;i++) {
        WoztBinding *b=g_ptr_array_index(ui->bindings,i);
        GObject *object=g_weak_ref_get(&b->object);
        if(!object)continue;
        char *current=NULL;
        g_object_get(object,b->property,&current,NULL);
        if(current && g_strcmp0(current,b->rendered) &&
           g_strcmp0(current,b->original)) {
            g_free(b->original);b->original=g_strdup(current);
        }
        const char *value=translated(ui,b->original);
        if(g_strcmp0(current,value))g_object_set(object,b->property,value,NULL);
        g_free(b->rendered);b->rendered=g_strdup(value);
        g_free(current);
        g_object_unref(object);
    }
}
static void change_theme(GObject *obj,GParamSpec *param,gpointer user) {
    (void)obj;(void)param;
    WoztUi *ui=user;
    if(ui->loading)return;
    guint idx=gtk_drop_down_get_selected(ui->theme_selector);
    if(idx>=ui->themes->len)return;
    WoztTheme *theme=g_ptr_array_index(ui->themes,idx);
    g_free(ui->theme_id);ui->theme_id=g_strdup(theme->id);
    apply_theme(ui);save_preferences(ui);
}
static void change_language(GObject *obj,GParamSpec *param,gpointer user) {
    (void)obj;(void)param;
    WoztUi *ui=user;
    if(ui->loading)return;
    guint idx=gtk_drop_down_get_selected(ui->language_selector);
    if(idx>ui->languages->len)return;
    g_free(ui->language_id);
    ui->language_id=g_strdup(idx?((WoztLanguage *)g_ptr_array_index(ui->languages,idx-1))->id:"en");
    refresh_translations(ui);save_preferences(ui);
}
static void reload_resources(WoztUi *ui) {
    ui->loading=TRUE;
    g_ptr_array_set_size(ui->themes,0);
    g_ptr_array_set_size(ui->languages,0);
    g_autofree char *themes=g_build_filename(ui->resources,"themes",NULL);
    g_autofree char *languages=g_build_filename(ui->resources,"languages",NULL);
    load_directory(ui,themes,TRUE);
    load_directory(ui,languages,FALSE);
    g_autofree char *config_root=g_build_filename(g_get_user_config_dir(),"pcble2gamepad",NULL);
    g_autofree char *custom_themes=g_build_filename(config_root,"themes",NULL);
    g_autofree char *custom_languages=g_build_filename(config_root,"languages",NULL);
    load_directory(ui,custom_themes,TRUE);
    load_directory(ui,custom_languages,FALSE);
    if(ui->theme_names) {
        while(g_list_model_get_n_items(G_LIST_MODEL(ui->theme_names)))
            gtk_string_list_remove(ui->theme_names,0);
        guint selected=0;
        for(guint i=0;i<ui->themes->len;i++) {
            WoztTheme *theme=g_ptr_array_index(ui->themes,i);
            gtk_string_list_append(ui->theme_names,theme->name);
            if(!g_strcmp0(ui->theme_id,theme->id))selected=i;
        }
        gtk_drop_down_set_selected(ui->theme_selector,selected);
        if(ui->themes->len) {
            const WoztTheme *theme=g_ptr_array_index(ui->themes,selected);
            g_free(ui->theme_id);ui->theme_id=g_strdup(theme->id);
        }
    }
    if(ui->language_names) {
        while(g_list_model_get_n_items(G_LIST_MODEL(ui->language_names)))
            gtk_string_list_remove(ui->language_names,0);
        gtk_string_list_append(ui->language_names,"English");
        guint selected=0;
        for(guint i=0;i<ui->languages->len;i++) {
            WoztLanguage *language=g_ptr_array_index(ui->languages,i);
            gtk_string_list_append(ui->language_names,language->name);
            if(!g_strcmp0(ui->language_id,language->id))selected=i+1;
        }
        gtk_drop_down_set_selected(ui->language_selector,selected);
        if(!selected && g_strcmp0(ui->language_id,"en")) {
            g_free(ui->language_id);ui->language_id=g_strdup("en");
        }
    }
    ui->loading=FALSE;
    apply_theme(ui);refresh_translations(ui);
}
static void reload_clicked(GtkButton *button,gpointer data) {
    (void)button;reload_resources(data);
}
static void minimized(GtkButton *button,gpointer data) {
    (void)button;WoztUi *ui=data;gtk_window_minimize(ui->window);
}
static void maximized(GtkButton *button,gpointer data) {
    (void)button;WoztUi *ui=data;
    if(gtk_window_is_maximized(ui->window))gtk_window_unmaximize(ui->window);
    else gtk_window_maximize(ui->window);
}
static void closed(GtkButton *button,gpointer data) {
    (void)button;WoztUi *ui=data;gtk_window_close(ui->window);
}
static GtkWidget *icon_button(const char *name,const char *tooltip,
                              GCallback callback,gpointer data) {
    GtkWidget *button=gtk_button_new_from_icon_name(name);
    gtk_widget_set_tooltip_text(button,tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button),
        GTK_ACCESSIBLE_PROPERTY_LABEL,tooltip,-1);
    gtk_widget_add_css_class(button,"wozt-window-button");
    g_signal_connect(button,"clicked",callback,data);
    return button;
}

#if defined(PCBLE2GAMEPAD_HAVE_X11) && defined(GDK_WINDOWING_X11)
static char *wozt_icon_file(int size) {
    g_autofree char *folder=g_strdup_printf("%dx%d",size,size);
    const char *filename=WOZT_APP_ID ".png";
    const char *appdir=g_getenv("APPDIR");
    if(appdir && *appdir) {
        char *path=g_build_filename(appdir,"usr","share","icons","hicolor",
                                    folder,"apps",filename,NULL);
        if(g_file_test(path,G_FILE_TEST_IS_REGULAR))return path;
        g_free(path);
    }
    g_autofree char *exe=g_file_read_link("/proc/self/exe",NULL);
    if(exe) {
        g_autofree char *binary_dir=g_path_get_dirname(exe);
        g_autofree char *prefix=g_path_get_dirname(binary_dir);
        char *path=g_build_filename(prefix,"data","icons","hicolor",
                                    folder,"apps",filename,NULL);
        if(g_file_test(path,G_FILE_TEST_IS_REGULAR))return path;
        g_free(path);
    }
    for(const char *const *p=g_get_system_data_dirs();*p;p++) {
        char *path=g_build_filename(*p,"icons","hicolor",folder,"apps",
                                    filename,NULL);
        if(g_file_test(path,G_FILE_TEST_IS_REGULAR))return path;
        g_free(path);
    }
    return NULL;
}
static void wozt_x11_window_identity(GtkWidget *widget,gpointer unused) {
    (void)unused;
    GdkSurface *surface=gtk_native_get_surface(GTK_NATIVE(widget));
    if(!surface || !GDK_IS_X11_SURFACE(surface))return;
    /* GTK 4.18 marks these X11 accessors as deprecated. The desktop icon
     * bridge still needs the native X11 Display and Window on X11. Keep
     * -Werror enabled everywhere else, including the rest of this function. */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display *display=gdk_x11_display_get_xdisplay(gtk_widget_get_display(widget));
    Window xid=gdk_x11_surface_get_xid(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if(!display || !xid)return;

    /* Match StartupWMClass from the installed .desktop file. */
    XClassHint wm_class={.res_name=(char *)WOZT_APP_ID,
                         .res_class=(char *)WOZT_APP_ID};
    XSetClassHint(display,xid,&wm_class);

    /* KDE Plasma's task manager first resolves a window to its desktop
     * launcher, then takes the icon from that launcher's KService.  WM_CLASS
     * and _NET_WM_ICON alone may still leave a generic document icon.
     * Publish the same desktop-file ID used by GtkApplication and
     * data/io.github.wozt.pcble2gamepad.desktop, without the .desktop suffix.
     * Keep this on X11 only; GTK handles the Wayland app_id itself. */
    Atom desktop_file=XInternAtom(display,"_KDE_NET_WM_DESKTOP_FILE",False);
    Atom utf8_string=XInternAtom(display,"UTF8_STRING",False);
    XChangeProperty(display,xid,desktop_file,utf8_string,8,PropModeReplace,
                    (const unsigned char *)WOZT_APP_ID,
                    (int)strlen(WOZT_APP_ID));

    /* WUP_V915_TASKBAR_ICON, ported to GTK4/X11: KDE receives the same
     * seven icon sizes as WUP Stream, now via _NET_WM_ICON because GTK4
     * removed gtk_window_set_icon_list(). Load once and scale as needed. */
    GArray *argb=g_array_new(FALSE,FALSE,sizeof(gulong));
    static const int sizes[]={16,24,32,48,64,128,256};
    const int source_sizes[]={128,64};
    g_autoptr(GdkPixbuf) source=NULL;
    for(guint i=0;i<G_N_ELEMENTS(source_sizes) && !source;i++) {
        g_autofree char *path=wozt_icon_file(source_sizes[i]);
        if(!path)continue;
        g_autoptr(GError) error=NULL;
        source=gdk_pixbuf_new_from_file(path,&error);
        if(!source)
            g_warning("Taskbar icon: could not read %s: %s",path,
                      error?error->message:"unknown error");
    }
    for(guint i=0;source && i<G_N_ELEMENTS(sizes);i++) {
        const int size=sizes[i];
        g_autoptr(GdkPixbuf) image=
            gdk_pixbuf_get_width(source)==size &&
            gdk_pixbuf_get_height(source)==size
                ?g_object_ref(source)
                :gdk_pixbuf_scale_simple(source,size,size,GDK_INTERP_BILINEAR);
        if(!image)continue;
        int width=gdk_pixbuf_get_width(image);
        int height=gdk_pixbuf_get_height(image);
        int channels=gdk_pixbuf_get_n_channels(image);
        int stride=gdk_pixbuf_get_rowstride(image);
        gboolean alpha=gdk_pixbuf_get_has_alpha(image);
        const guchar *pixels=gdk_pixbuf_read_pixels(image);
        gulong val=(gulong)width;
        g_array_append_val(argb,val);
        val=(gulong)height;
        g_array_append_val(argb,val);
        for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
            const guchar *px=pixels+(gsize)y*(gsize)stride+(gsize)x*(gsize)channels;
            gulong a=alpha?(gulong)px[3]:255UL;
            val=(a<<24)|((gulong)px[0]<<16)|((gulong)px[1]<<8)|(gulong)px[2];
            g_array_append_val(argb,val);
        }
    }
    if(argb->len) {
        Atom property=XInternAtom(display,"_NET_WM_ICON",False);
        XChangeProperty(display,xid,property,XA_CARDINAL,32,PropModeReplace,
                        (const unsigned char *)argb->data,(int)argb->len);
    } else {
        g_warning("No taskbar PNG icons found for %s",WOZT_APP_ID);
    }
    g_array_unref(argb);
    XFlush(display);
}
#endif
WoztUi *wozt_ui_new(GtkWindow *window) {
    WoztUi *ui=g_new0(WoztUi,1);ui->window=window;
    ui->display=g_object_ref(gtk_widget_get_display(GTK_WIDGET(window)));
    ui->themes=g_ptr_array_new_with_free_func(theme_free);
    ui->languages=g_ptr_array_new_with_free_func(language_free);
    ui->bindings=g_ptr_array_new_with_free_func(binding_free);
    g_autofree char *root=g_build_filename(g_get_user_config_dir(),"pcble2gamepad",NULL);
    g_mkdir_with_parents(root,0700);
    ui->preferences_path=g_build_filename(root,"wozt-ui.ini",NULL);
    g_autoptr(GKeyFile) file=g_key_file_new();
    g_key_file_load_from_file(file,ui->preferences_path,G_KEY_FILE_NONE,NULL);
    ui->theme_id=g_key_file_get_string(file,"Appearance","theme",NULL);
    if(!ui->theme_id)ui->theme_id=g_strdup("graphite");
    ui->language_id=g_key_file_get_string(file,"Appearance","language",NULL);
    if(!ui->language_id)ui->language_id=g_strdup("en");
    const char *override=g_getenv("PCBLE2GAMEPAD_WOZT_DATA_DIR");
    if(override && g_file_test(override,G_FILE_TEST_IS_DIR))
        ui->resources=g_strdup(override);
    else if(g_getenv("APPDIR")) {
        g_autofree char *appdir_path=g_build_filename(g_getenv("APPDIR"),
            "usr","share","pcble2gamepad","wozt",NULL);
        ui->resources=g_file_test(appdir_path,G_FILE_TEST_IS_DIR)
            ?g_strdup(appdir_path):g_strdup(PCBLE2GAMEPAD_WOZT_DATA_DIR);
    } else if(g_file_test("data/wozt/themes",G_FILE_TEST_IS_DIR))
        ui->resources=g_strdup("data/wozt");
    else ui->resources=g_strdup(PCBLE2GAMEPAD_WOZT_DATA_DIR);
    gtk_widget_add_css_class(GTK_WIDGET(window),"wozt-root");
    gtk_window_set_decorated(window,FALSE);
    gtk_window_set_resizable(window,TRUE);
#if defined(PCBLE2GAMEPAD_HAVE_X11) && defined(GDK_WINDOWING_X11)
    g_signal_connect(window,"realize",G_CALLBACK(wozt_x11_window_identity),NULL);
    g_signal_connect(window,"map",G_CALLBACK(wozt_x11_window_identity),NULL);
#endif
    /* Install icon resources and also publish an actual X11 _NET_WM_ICON. */
    g_autofree char *dev_icon_dir=g_build_filename(g_get_current_dir(),"data",NULL);
    gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(ui->display),
                                   dev_icon_dir);
    gtk_window_set_default_size(window,960,703);
    gtk_widget_set_size_request(GTK_WIDGET(window),680,530);
    ui->css=gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(ui->display,
        GTK_STYLE_PROVIDER(ui->css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    reload_resources(ui);
    return ui;
}
GtkWidget *wozt_ui_header(WoztUi *ui) {
    GtkWidget *handle=gtk_window_handle_new();
    GtkWidget *bar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_widget_add_css_class(bar,"wozt-bar");
    GtkWidget *icon=gtk_image_new_from_icon_name(WOZT_APP_ID);
    gtk_image_set_pixel_size(GTK_IMAGE(icon),22);
    gtk_box_append(GTK_BOX(bar),icon);
    GtkWidget *name=gtk_label_new("pcble2gamepad");
    gtk_widget_add_css_class(name,"wozt-brand");
    gtk_box_append(GTK_BOX(bar),name);
    GtkWidget *drag=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_widget_set_hexpand(drag,TRUE);
    gtk_box_append(GTK_BOX(bar),drag);
    ui->header_actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_box_append(GTK_BOX(bar),ui->header_actions);
    ui->theme_names=gtk_string_list_new(NULL);
    ui->theme_selector=GTK_DROP_DOWN(gtk_drop_down_new(
        G_LIST_MODEL(ui->theme_names),NULL));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui->theme_selector),"Theme (JSON)");
    gtk_widget_set_size_request(GTK_WIDGET(ui->theme_selector),90,-1);
    gtk_box_append(GTK_BOX(bar),GTK_WIDGET(ui->theme_selector));
    g_signal_connect(ui->theme_selector,"notify::selected",G_CALLBACK(change_theme),ui);
    ui->language_names=gtk_string_list_new(NULL);
    ui->language_selector=GTK_DROP_DOWN(gtk_drop_down_new(
        G_LIST_MODEL(ui->language_names),NULL));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ui->language_selector),"Language (JSON)");
    gtk_widget_set_size_request(GTK_WIDGET(ui->language_selector),83,-1);
    gtk_box_append(GTK_BOX(bar),GTK_WIDGET(ui->language_selector));
    g_signal_connect(ui->language_selector,"notify::selected",G_CALLBACK(change_language),ui);
    gtk_box_append(GTK_BOX(bar),icon_button("view-refresh-symbolic",
        "Reload JSON themes and languages",G_CALLBACK(reload_clicked),ui));
    gtk_box_append(GTK_BOX(bar),icon_button("window-minimize-symbolic",
        "Minimize",G_CALLBACK(minimized),ui));
    gtk_box_append(GTK_BOX(bar),icon_button("window-maximize-symbolic",
        "Maximize / restore",G_CALLBACK(maximized),ui));
    GtkWidget *close=icon_button("window-close-symbolic",
        "Close",G_CALLBACK(closed),ui);
    gtk_widget_add_css_class(close,"wozt-close");
    gtk_box_append(GTK_BOX(bar),close);
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle),bar);
    reload_resources(ui);
    return handle;
}
void wozt_ui_pack_header(WoztUi *ui,GtkWidget *widget) {
    if(ui && ui->header_actions && widget)
        gtk_box_append(GTK_BOX(ui->header_actions),widget);
}
static void resize_press(GtkGestureClick *gesture,int n,double x,double y,gpointer data) {
    (void)n;
    WoztUi *ui=data;
    if(gtk_window_is_maximized(ui->window))return;
    GtkWidget *widget=gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GdkSurface *surface=gtk_native_get_surface(GTK_NATIVE(ui->window));
    if(!surface || !GDK_IS_TOPLEVEL(surface))return;
    int edge=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"wozt-edge"))-1;
    if(edge<0)return;
    graphene_point_t point,translated_point;
    graphene_point_init(&point,(float)x,(float)y);
    if(!gtk_widget_compute_point(widget,GTK_WIDGET(ui->window),
                                 &point,&translated_point))return;
    GdkEvent *event=gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface),(GdkSurfaceEdge)edge,
        event?gdk_event_get_device(event):NULL,
        gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)),
        translated_point.x,translated_point.y,
        event?gdk_event_get_time(event):GDK_CURRENT_TIME);
}
static void resize_zone(WoztUi *ui,GtkOverlay *overlay,GdkSurfaceEdge edge,
                        GtkAlign halign,GtkAlign valign,gboolean horizontal) {
    GtkWidget *hit=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_widget_add_css_class(hit,"wozt-resize-hit");
    gtk_widget_set_halign(hit,halign);
    gtk_widget_set_valign(hit,valign);
    gtk_widget_set_size_request(hit,horizontal?12:5,horizontal?12:5);
    if(halign==GTK_ALIGN_FILL)gtk_widget_set_hexpand(hit,TRUE);
    if(valign==GTK_ALIGN_FILL)gtk_widget_set_vexpand(hit,TRUE);
    const char *cursor="default";
    switch(edge) {
    case GDK_SURFACE_EDGE_NORTH:case GDK_SURFACE_EDGE_SOUTH:cursor="ns-resize";break;
    case GDK_SURFACE_EDGE_EAST:case GDK_SURFACE_EDGE_WEST:cursor="ew-resize";break;
    case GDK_SURFACE_EDGE_NORTH_WEST:case GDK_SURFACE_EDGE_SOUTH_EAST:cursor="nwse-resize";break;
    case GDK_SURFACE_EDGE_NORTH_EAST:case GDK_SURFACE_EDGE_SOUTH_WEST:cursor="nesw-resize";break;
    default:break;
    }
    gtk_widget_set_cursor_from_name(hit,cursor);
    g_object_set_data(G_OBJECT(hit),"wozt-edge",GINT_TO_POINTER((int)edge+1));
    GtkGesture *gesture=gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),GDK_BUTTON_PRIMARY);
    g_signal_connect(gesture,"pressed",G_CALLBACK(resize_press),ui);
    gtk_widget_add_controller(hit,GTK_EVENT_CONTROLLER(gesture));
    gtk_overlay_add_overlay(overlay,hit);
}
GtkWidget *wozt_ui_wrap(WoztUi *ui,GtkWidget *content) {
    GtkWidget *overlay=gtk_overlay_new();
    gtk_widget_add_css_class(overlay,"wozt-root");
    gtk_overlay_set_child(GTK_OVERLAY(overlay),content);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_NORTH,
                GTK_ALIGN_FILL,GTK_ALIGN_START,FALSE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_SOUTH,
                GTK_ALIGN_FILL,GTK_ALIGN_END,FALSE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_WEST,
                GTK_ALIGN_START,GTK_ALIGN_FILL,FALSE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_EAST,
                GTK_ALIGN_END,GTK_ALIGN_FILL,FALSE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_NORTH_WEST,
                GTK_ALIGN_START,GTK_ALIGN_START,TRUE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_NORTH_EAST,
                GTK_ALIGN_END,GTK_ALIGN_START,TRUE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_SOUTH_WEST,
                GTK_ALIGN_START,GTK_ALIGN_END,TRUE);
    resize_zone(ui,GTK_OVERLAY(overlay),GDK_SURFACE_EDGE_SOUTH_EAST,
                GTK_ALIGN_END,GTK_ALIGN_END,TRUE);
    return overlay;
}
static void navigation_selected(GObject *obj,GParamSpec *pspec,gpointer data) {
    (void)obj;(void)pspec;
    WoztUi *ui=data;
    if(!ui->stack || !ui->compact_navigation)return;
    static const char *const pages[]={"overview","keyboard","controller","settings","diagnostics"};
    guint selected=gtk_drop_down_get_selected(GTK_DROP_DOWN(ui->compact_navigation));
    if(selected<G_N_ELEMENTS(pages))gtk_stack_set_visible_child_name(ui->stack,pages[selected]);
}
GtkWidget *wozt_ui_compact_navigation(WoztUi *ui,GtkStack *stack) {
    static const char *const labels[]={
        "Connection","Keyboard","PC controller","Settings","Diagnostics",NULL
    };
    GtkWidget *dropdown=gtk_drop_down_new_from_strings(labels);
    gtk_widget_set_margin_start(dropdown,8);
    gtk_widget_set_margin_end(dropdown,8);
    gtk_widget_set_visible(dropdown,FALSE);
    gtk_widget_set_tooltip_text(dropdown,"Page navigation at narrow window sizes");
    ui->stack=stack;ui->compact_navigation=dropdown;
    g_signal_connect(dropdown,"notify::selected",G_CALLBACK(navigation_selected),ui);
    return dropdown;
}
void wozt_ui_set_responsive(WoztUi *ui,GtkWidget *columns,
                            GtkWidget *sidebar,GtkWidget *compact_navigation) {
    ui->columns=columns;ui->sidebar=sidebar;
    ui->compact_navigation=compact_navigation;
}
void wozt_ui_tick(WoztUi *ui,int width) {
    if(!ui)return;
    guint layout_class=(width<930?2:0)+(width<1050?1:0);
    if(ui->last_width_class!=layout_class+1) {
        ui->last_width_class=layout_class+1;
        if(ui->columns)
            gtk_orientable_set_orientation(GTK_ORIENTABLE(ui->columns),
                GTK_ORIENTATION_VERTICAL);
        if(ui->sidebar)gtk_widget_set_visible(ui->sidebar,width>=1050);
        if(ui->compact_navigation)
            gtk_widget_set_visible(ui->compact_navigation,width<1050);
    }
    if(++ui->last_language_update%20==0)refresh_translations(ui);
}
void wozt_ui_free(WoztUi *ui) {
    if(!ui)return;
    gtk_style_context_remove_provider_for_display(
        ui->display,GTK_STYLE_PROVIDER(ui->css));
    g_clear_object(&ui->css);g_clear_object(&ui->display);
    g_clear_object(&ui->theme_names);g_clear_object(&ui->language_names);
    g_ptr_array_unref(ui->themes);g_ptr_array_unref(ui->languages);
    g_ptr_array_unref(ui->bindings);
    g_free(ui->resources);g_free(ui->preferences_path);
    g_free(ui->theme_id);g_free(ui->language_id);g_free(ui);
}
