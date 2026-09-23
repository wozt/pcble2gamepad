#include "input-model.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>

static void test_neutral_and_buttons(void)
{
    InputProfile profile;
    InputFrame frame;
    gboolean pressed[INPUT_ACTIONS] = {0};
    const double axes[4] = {0};
    input_profile_defaults(&profile, FALSE);
    input_compose(&profile, pressed, axes, &frame);
    g_assert_cmpint(frame.sticks[0], ==, 2159);
    g_assert_cmpint(frame.sticks[1], ==, 1916);
    g_assert_cmpint(frame.sticks[2], ==, 2070);
    g_assert_cmpint(frame.sticks[3], ==, 2013);
    pressed[0] = TRUE;   /* A */
    pressed[8] = TRUE;   /* Minus */
    pressed[14] = TRUE;  /* D-pad up */
    input_compose(&profile, pressed, axes, &frame);
    g_assert_cmphex(frame.buttons[0], ==, 0x08);
    g_assert_cmphex(frame.buttons[1], ==, 0x01);
    g_assert_cmphex(frame.buttons[2], ==, 0x02);
}

static void test_sticks(void)
{
    InputProfile profile;
    InputFrame frame;
    gboolean pressed[INPUT_ACTIONS] = {0};
    double axes[4] = {1.0, 0.0, 0.0, -1.0};
    input_profile_defaults(&profile, FALSE);
    input_compose(&profile, pressed, axes, &frame);
    g_assert_cmpint(frame.sticks[0], ==, 3625);
    g_assert_cmpint(frame.sticks[1], ==, 1916);
    g_assert_cmpint(frame.sticks[2], ==, 2070);
    g_assert_cmpint(frame.sticks[3], ==, 482);
    profile.invert[0] = TRUE;
    input_compose(&profile, pressed, axes, &frame);
    g_assert_cmpint(frame.sticks[0], ==, 642);
    axes[0] = profile.deadzone / 2;
    axes[3] = 0;
    input_compose(&profile, pressed, axes, &frame);
    g_assert_cmpint(frame.sticks[0], ==, 2159);
}

static void test_profile_roundtrip(void)
{
    InputProfile original, loaded;
    input_profile_defaults(&original, TRUE);
    original.deadzone = .2;
    original.sensitivity = 1.25;
    original.invert[3] = TRUE;
    original.swap_sticks = TRUE;
    original.background = TRUE;
    original.swap_face_buttons = FALSE;
    original.emulated_controller = 1;
    original.keys[0] = 'p';
    g_autofree char *path = g_strdup_printf("%s/input-profile-%u.ini", g_get_tmp_dir(), g_random_int());
    g_autoptr(GError) error = NULL;
    g_assert_true(input_profile_save(&original, path, &error));
    g_assert_no_error(error);
    g_assert_true(input_profile_load(&loaded, path, &error));
    g_assert_no_error(error);
    g_assert_cmpint(loaded.keys[0], ==, 'p');
    g_assert_cmpfloat_with_epsilon(loaded.deadzone, .2, .0001);
    g_assert_cmpfloat_with_epsilon(loaded.sensitivity, 1.25, .0001);
    g_assert_true(loaded.invert[3]);
    g_assert_true(loaded.swap_sticks);
    g_assert_true(loaded.background);
    g_assert_false(loaded.swap_face_buttons);
    g_assert_cmpint(loaded.emulated_controller, ==, 1);
    g_unlink(path);
}

static void test_legacy_profile_defaults_to_pro(void)
{
    InputProfile original, loaded;
    input_profile_defaults(&original, FALSE);
    original.emulated_controller = 1;
    g_autofree char *path = g_strdup_printf("%s/input-profile-legacy-%u.ini",
                                            g_get_tmp_dir(), g_random_int());
    g_autoptr(GError) error = NULL;
    g_assert_true(input_profile_save(&original, path, &error));
    g_assert_no_error(error);
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_assert_true(g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error));
    g_assert_no_error(error);
    g_assert_true(g_key_file_remove_group(key_file, "Controller", &error));
    g_assert_no_error(error);
    g_assert_true(g_key_file_remove_key(key_file, "Input", "swap_face_buttons", &error));
    g_assert_no_error(error);
    g_assert_true(g_key_file_save_to_file(key_file, path, &error));
    g_assert_no_error(error);
    g_assert_true(input_profile_load(&loaded, path, &error));
    g_assert_no_error(error);
    g_assert_cmpint(loaded.emulated_controller, ==, 0);
    g_assert_true(loaded.swap_face_buttons);
    g_unlink(path);
}

static void test_virtual_gamepad(void)
{
    InputProfile profile;
    gboolean pressed[INPUT_ACTIONS];
    double axes[4];
    input_profile_defaults(&profile, FALSE);
    g_assert_cmpint(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER), ==, 0);
    int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           SDL_CONTROLLER_AXIS_MAX,
                                           SDL_CONTROLLER_BUTTON_MAX, 0);
    g_assert_cmpint(device, >=, 0);
    g_assert_true(SDL_IsGameController(device));
    SDL_GameController *controller = SDL_GameControllerOpen(device);
    g_assert_nonnull(controller);
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    g_assert_cmpint(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1), ==, 0);
    g_assert_cmpint(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 32767), ==, 0);
    SDL_GameControllerUpdate();
    input_gamepad(&profile, controller, pressed, axes);
    g_assert_false(pressed[0]);
    g_assert_true(pressed[1]);
    g_assert_cmpfloat(axes[0], >, .99);

    profile.swap_face_buttons = FALSE;
    input_gamepad(&profile, controller, pressed, axes);
    g_assert_true(pressed[0]);
    g_assert_false(pressed[1]);
    SDL_GameControllerClose(controller);
    g_assert_cmpint(SDL_JoystickDetachVirtual(device), ==, 0);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/input/neutral-buttons", test_neutral_and_buttons);
    g_test_add_func("/input/sticks", test_sticks);
    g_test_add_func("/input/profile-roundtrip", test_profile_roundtrip);
    g_test_add_func("/input/profile-legacy", test_legacy_profile_defaults_to_pro);
    g_test_add_func("/input/virtual-gamepad", test_virtual_gamepad);
    return g_test_run();
}
