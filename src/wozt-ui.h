#pragma once

#include <adwaita.h>

typedef struct WoztUi WoztUi;

/* GTK4 implementation of the WOZT GTK Standard's window and appearance layer. */
WoztUi *wozt_ui_new(GtkWindow *window);
void wozt_ui_free(WoztUi *ui);
GtkWidget *wozt_ui_header(WoztUi *ui);
void wozt_ui_pack_header(WoztUi *ui, GtkWidget *widget);
GtkWidget *wozt_ui_wrap(WoztUi *ui, GtkWidget *content);
GtkWidget *wozt_ui_compact_navigation(WoztUi *ui, GtkStack *stack);
void wozt_ui_set_responsive(WoztUi *ui, GtkWidget *columns,
                            GtkWidget *sidebar, GtkWidget *compact_navigation);
void wozt_ui_tick(WoztUi *ui, int width);
void wozt_ui_bind_text(WoztUi *ui, GObject *object,
                       const char *property, const char *english);
