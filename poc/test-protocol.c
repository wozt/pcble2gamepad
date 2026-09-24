#include "protocol.h"
#include <glib.h>
#include <string.h>
static void reports(void) {
    ProState s;uint8_t mac[]={0xe0,0xad,0x47,0x40,0x70,0xd9},out[50],in[50]={0xa2,1};
    pro_init(&s,mac);pro_input(&s,5,out);
    const uint8_t neutral[]={0x6f,0xc8,0x77,0x16,0xd8,0x7d};
    g_assert_cmpmem(out+7,6,neutral,6);g_assert_cmpint(out[2],==,5);
    g_assert_cmpint(out[3],==,0x80);g_assert_cmpint(out[13],==,0x80);
    g_assert_cmpuint(pro_stream_input(&s,5,out),==,13);
    g_assert_cmpint(out[1],==,0x3f);g_assert_cmpint(out[4],==,8);
    for(int i=0;i<4;i++)g_assert_cmpint(out[5+i*2]|(out[6+i*2]<<8),==,0x8000);
    in[11]=2;g_assert_true(pro_reply(&s,in,12,6,out));
    g_assert_cmpint(out[1],==,0x21);g_assert_cmpint(out[17],==,0x48);
    g_assert_cmpint(out[18],==,3);g_assert_cmpmem(out+20,6,mac,6);
    g_assert_cmpint(out[26],==,1);
    g_assert_cmpint(out[27],==,2);

    /*
     * Factory configuration says that controller colors are present.
     */
    in[11]=0x10;
    in[12]=0x1b;
    in[13]=0x60;
    in[14]=0;
    in[15]=0;
    in[16]=1;

    g_assert_true(
        pro_reply(&s,in,17,7,out));

    g_assert_cmpint(
        out[21],
        ==,
        2);

    const uint8_t body[]={0x12,0x34,0x56};
    const uint8_t buttons[]={0x65,0x43,0x21};
    const uint8_t left_grip[]={0xaa,0xbb,0xcc};
    const uint8_t right_grip[]={0x11,0x22,0x33};
    controller_set_colors(&s,body,buttons,left_grip,right_grip);

    in[11]=0x10;in[12]=0x50;in[13]=0x60;in[14]=0;in[15]=0;in[16]=13;
    g_assert_true(pro_reply(&s,in,17,7,out));
    g_assert_cmpmem(out+21,3,body,3);
    g_assert_cmpmem(out+24,3,buttons,3);
    g_assert_cmpmem(out+27,3,left_grip,3);
    g_assert_cmpmem(out+30,3,right_grip,3);

    /* 0x605C: standard design variation. */
    g_assert_cmpint(out[33],==,0);

    in[11]=0x10;
    in[12]=0x3d;
    in[13]=0x60;
    in[14]=0;
    in[15]=0;
    in[16]=25;

    g_assert_true(pro_reply(&s,in,17,7,out));
    g_assert_cmpint(out[14],==,0x90);

    /*
     * The SPI calibration must describe the same coordinate space used
     * by src/input-model.c when it builds InputFrame.sticks.
     *
     * Lock the whole 0x603D..0x604F block down: changing only one center
     * or excursion would otherwise bring back asymmetric stick travel.
     */
    const uint8_t pro_calibration[]={
        0xba,0xf5,0x62,
        0x6f,0xc8,0x77,
        0xed,0x95,0x5b,
        0x16,0xd8,0x7d,
        0xf2,0xb5,0x5f,
        0x86,0x65,0x5e,
        0x0f
    };

    g_assert_cmpmem(
        out+21,
        sizeof(pro_calibration),
        pro_calibration,
        sizeof(pro_calibration));

    /*
     * 0x603D + 0x13 == 0x6050.
     * The tail of this same Switch read must expose the configured
     * body/buttons, not stale stick-table bytes.
     */
    g_assert_cmpmem(out+40,3,body,3);
    g_assert_cmpmem(out+43,3,buttons,3);
    in[12]=0x10;in[13]=0x80;in[16]=24;
    g_assert_true(pro_reply(&s,in,17,7,out));for(int i=21;i<45;i++)g_assert_cmpint(out[i],==,0xff);
    in[16]=30;g_assert_false(pro_reply(&s,in,17,7,out));
    for(size_t len=0;len<17;len++)g_assert_false(pro_reply(&s,in,len,7,out));
    in[11]=3;in[12]=0x30;g_assert_true(pro_reply(&s,in,13,8,out));
    g_assert_cmpuint(pro_stream_input(&s,8,out),==,50);g_assert_cmpint(out[1],==,0x30);
    in[11]=0x30;in[12]=1;g_assert_true(pro_reply(&s,in,13,8,out));g_assert_cmpint(s.lights,==,1);
    in[11]=0x48;in[12]=1;g_assert_true(pro_reply(&s,in,13,8,out));g_assert_true(s.vibration);
    g_assert_cmpint(out[14],==,0x80);
    in[11]=0x21;g_assert_true(pro_reply(&s,in,13,8,out));
    g_assert_cmpint(out[14],==,0x80);g_assert_cmpint(out[49],==,0);
    in[11]=0x22;g_assert_true(pro_reply(&s,in,13,8,out));
    in[11]=0xfe;g_assert_true(pro_reply(&s,in,13,8,out));
    s.buttons[0]=8;s.sticks[0]=4095;pro_input(&s,9,out);
    g_assert_cmpint(out[4],==,8);g_assert_cmpint(out[7],==,255);g_assert_cmpint(out[8]&15,==,15);
}
static void joycon_reports(void) {
    const uint8_t mac[]={1,2,3,4,5,6};uint8_t out[50],in[50]={0xa2,1};ProState s;
    controller_init(&s,CONTROLLER_JOYCON_L,mac);s.buttons[0]=0xff;s.buttons[1]=0xff;s.buttons[2]=0xff;
    pro_input(&s,1,out);g_assert_cmpint(out[3],==,0x9e);g_assert_cmpint(out[4],==,0);
    g_assert_cmpint(out[5],==,0x29);g_assert_cmpint(out[6],==,0xff);
    for(int i=10;i<13;i++)g_assert_cmpint(out[i],==,0);
    in[11]=2;g_assert_true(pro_reply(&s,in,12,2,out));g_assert_cmpint(out[18],==,CONTROLLER_JOYCON_L);
    in[11]=0x10;in[12]=0x3d;in[13]=0x60;in[16]=18;
    g_assert_true(pro_reply(&s,in,17,3,out));for(int i=30;i<39;i++)g_assert_cmpint(out[i],==,0xff);
    controller_init(&s,CONTROLLER_JOYCON_R,mac);s.buttons[0]=0xff;s.buttons[1]=0xff;s.buttons[2]=0xff;
    pro_input(&s,1,out);g_assert_cmpint(out[4],==,0xff);g_assert_cmpint(out[5],==,0x16);
    g_assert_cmpint(out[6],==,0);for(int i=7;i<10;i++)g_assert_cmpint(out[i],==,0);
    in[11]=2;g_assert_true(pro_reply(&s,in,12,2,out));g_assert_cmpint(out[18],==,CONTROLLER_JOYCON_R);
}
int main(int argc,char **argv) {g_test_init(&argc,&argv,NULL);g_test_add_func("/pro/wire",reports);g_test_add_func("/joycon/wire",joycon_reports);return g_test_run();}
