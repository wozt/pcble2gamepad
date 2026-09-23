#pragma once
#include <glib.h>
#include <SDL.h>
#define INPUT_ACTIONS 26
#define INPUT_BUTTONS 18
typedef struct {
    guint keys[INPUT_ACTIONS];
    int buttons[INPUT_BUTTONS];
    int emulated_controller;
    double deadzone, sensitivity;
    gboolean invert[4], swap_sticks, background;
} InputProfile;
typedef struct { guint8 buttons[3]; guint16 sticks[4]; double axes[4]; gboolean active[INPUT_ACTIONS]; } InputFrame;
extern const char *input_action_names[INPUT_ACTIONS];
void input_profile_defaults(InputProfile *p, gboolean azerty);
gboolean input_profile_load(InputProfile *p,const char *path,GError **error);
gboolean input_profile_save(const InputProfile *p,const char *path,GError **error);
void input_compose(const InputProfile *p,const gboolean pressed[INPUT_ACTIONS],const double axes[4],InputFrame *frame);
void input_gamepad(const InputProfile *p,SDL_GameController *pad,gboolean pressed[INPUT_ACTIONS],double axes[4]);
