/* Wire constants and calibration data adapted from NUXBT (MIT), see NUXBT-LICENSE. */
#include "protocol.h"
#include <string.h>
static void pack(uint8_t *p, uint16_t x, uint16_t y) {
    p[0] = x; p[1] = (x >> 8) | (y << 4); p[2] = y >> 4;
}
void controller_init(ProState *s, ControllerType type, const uint8_t address[6]) {
    memset(s, 0, sizeof(*s)); memcpy(s->address, address, 6);
    s->type = type;
    s->sticks[0] = 0x86f; s->sticks[1] = 0x77c;
    s->sticks[2] = 0x816; s->sticks[3] = 0x7dd;
}
void pro_init(ProState *s, const uint8_t address[6]) { controller_init(s, CONTROLLER_PRO, address); }
void pro_input(const ProState *s, uint8_t timer, uint8_t out[50]) {
    memset(out, 0, 50); out[0] = 0xa1; out[1] = 0x30;
    out[2] = timer; out[3] = s->type == CONTROLLER_PRO ? 0x90 : 0x9e;
    if(s->type != CONTROLLER_JOYCON_L)out[4]=s->buttons[0];
    out[5]=s->buttons[1] & (s->type==CONTROLLER_JOYCON_L?0x29:s->type==CONTROLLER_JOYCON_R?0x16:0xff);
    if(s->type != CONTROLLER_JOYCON_R)out[6]=s->buttons[2];
    if(s->type != CONTROLLER_JOYCON_R)pack(out + 7, s->sticks[0], s->sticks[1]);
    if(s->type != CONTROLLER_JOYCON_L)pack(out + 10, s->sticks[2], s->sticks[3]);
    out[13] = 0xa0;
    if (s->imu) for (int i = 0; i < 3; i++) out[19 + 12*i] = 0x10;
}
static uint8_t spi(const ProState *s,uint32_t a) {
    static const uint8_t sticks[] = {0xba,0xf5,0x62,0x6f,0xc8,0x77,0xed,0x95,0x5b,
        0x16,0xd8,0x7d,0xf2,0xb5,0x5f,0x86,0x65,0x5e};
    static const uint8_t sensor[] = {0xd3,0xff,0xd5,0xff,0x55,0x01,0x00,0x40,0x00,0x40,0x00,0x40,
        0x19,0x00,0xdd,0xff,0xdc,0xff,0x3b,0x34,0x3b,0x34,0x3b,0x34};
    static const uint8_t params[] = {0x0f,0x30,0x61,0x96,0x30,0xf3,0xd4,0x14,0x54,
        0x41,0x15,0x54,0xc7,0x79,0x9c,0x33,0x36,0x63};
    static const uint8_t motion[] = {0x50,0xfd,0,0,0xc6,0x0f};
    static const uint8_t left_body[]={0x0a,0xb9,0xe6},right_body[]={0xff,0x3c,0x28};
    static const uint8_t left_buttons[]={0x00,0x1e,0x1e},right_buttons[]={0x1e,0x0a,0x0a};
    if (a >= 0x603d && a < 0x6046 && s->type==CONTROLLER_JOYCON_R)return 0xff;
    if (a >= 0x6046 && a < 0x604f && s->type==CONTROLLER_JOYCON_L)return 0xff;
    if (a >= 0x603d && a < 0x604f) return sticks[a - 0x603d];
    if (a >= 0x6020 && a < 0x6038) return sensor[a - 0x6020];
    if (a >= 0x6080 && a < 0x6086) {
        if(s->type==CONTROLLER_PRO)return motion[a-0x6080];
        const uint8_t left[]={0x5e,0x01,0,0,0xf1,0x0f},right[]={0x5e,0x01,0,0,0x0f,0xf0};
        return (s->type==CONTROLLER_JOYCON_L?left:right)[a-0x6080];
    }
    if (a >= 0x6086 && a < 0x6098) return a==0x6089 && s->type!=CONTROLLER_PRO?0xae:params[a - 0x6086];
    if (a >= 0x6098 && a < 0x60aa) return a==0x609b && s->type!=CONTROLLER_PRO?0xae:params[a - 0x6098];
    if (a >= 0x6050 && a < 0x6053) return s->type==CONTROLLER_JOYCON_L?left_body[a-0x6050]:s->type==CONTROLLER_JOYCON_R?right_body[a-0x6050]:0x82;
    if (a >= 0x6053 && a < 0x6056) return s->type==CONTROLLER_JOYCON_L?left_buttons[a-0x6053]:s->type==CONTROLLER_JOYCON_R?right_buttons[a-0x6053]:0x0f;
    return 0xff;
}
bool pro_reply(ProState *s, const uint8_t *in, size_t len, uint8_t timer, uint8_t out[50]) {
    if (len < 12 || in[0] != 0xa2 || in[1] != 0x01) return false;
    uint8_t cmd = in[11];
    if ((cmd == 3 || cmd == 0x30 || cmd == 0x40 || cmd == 0x48) && len < 13) return false;
    if (cmd == 0x10 && (len < 17 || in[16] > 29)) return false;
    pro_input(s, timer, out); out[1] = 0x21;
    memset(out + 14, 0, 36); out[14] = 0x80; out[15] = cmd;
    switch (cmd) {
    case 2:
        out[14]=0x82; out[16]=3; out[17]=0x8b; out[18]=s->type; out[19]=2;
        memcpy(out+20,s->address,6); out[26]=1; out[27]=1; break;
    case 3: s->mode=in[12]; break;
    case 4: out[14]=0x83; break;
    case 8: break;
    case 0x10: {
        uint32_t addr = (uint32_t)in[12] | ((uint32_t)in[13]<<8) |
            ((uint32_t)in[14]<<16) | ((uint32_t)in[15]<<24);
        out[14]=0x90; memcpy(out+16,in+12,5);
        for (unsigned i=0;i<in[16];i++) out[21+i]=spi(s,addr+i);
        break;
    }
    case 0x21: {
        const uint8_t params[]={1,0,0xff,0,8,0,0x1b,1};
        out[14]=0xa0;memcpy(out+16,params,8);out[49]=0xc8;break;
    }
    case 0x22: break;
    case 0x30: s->lights=in[12]; break;
    case 0x40: s->imu=in[12]!=0; break;
    case 0x48: out[14]=0x82; s->vibration=in[12]!=0; break;
    default: return false;
    }
    return true;
}
