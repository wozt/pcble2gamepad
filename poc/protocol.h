#ifndef PRO_POC_PROTOCOL_H
#define PRO_POC_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef enum { CONTROLLER_JOYCON_L=1, CONTROLLER_JOYCON_R=2, CONTROLLER_PRO=3 } ControllerType;
typedef struct {
    uint8_t address[6], buttons[3], lights, mode;
    uint8_t body_color[3];
    uint8_t button_color[3];
    uint8_t left_grip_color[3];
    uint8_t right_grip_color[3];
    uint16_t sticks[4];
    ControllerType type;
    bool imu, vibration;
} ProState;
void controller_init(ProState *s, ControllerType type, const uint8_t address[6]);
void controller_set_colors(ProState *s,
                           const uint8_t body[3],
                           const uint8_t buttons[3],
                           const uint8_t left_grip[3],
                           const uint8_t right_grip[3]);
void pro_init(ProState *s, const uint8_t address[6]);
void pro_input(const ProState *s, uint8_t timer, uint8_t out[50]);
size_t pro_stream_input(const ProState *s, uint8_t timer, uint8_t out[50]);
/* Returns false for malformed/unsupported output reports; never reads past len. */
bool pro_reply(ProState *s, const uint8_t *in, size_t len, uint8_t timer, uint8_t out[50]);
#endif
