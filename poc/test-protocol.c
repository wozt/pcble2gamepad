#include "protocol.h"
#include <glib.h>
#include <string.h>
static void reports(void) {
    ProState s;uint8_t mac[]={0xe0,0xad,0x47,0x40,0x70,0xd9},out[50],in[50]={0xa2,1};
    pro_init(&s,mac);pro_input(&s,5,out);
    const uint8_t neutral[]={0x6f,0xc8,0x77,0x16,0xd8,0x7d};
    g_assert_cmpmem(out+7,6,neutral,6);g_assert_cmpint(out[2],==,5);
    in[11]=2;g_assert_true(pro_reply(&s,in,12,6,out));
    g_assert_cmpint(out[1],==,0x21);g_assert_cmpint(out[18],==,3);g_assert_cmpmem(out+20,6,mac,6);
    in[11]=0x10;in[12]=0x3d;in[13]=0x60;in[16]=18;
    g_assert_true(pro_reply(&s,in,17,7,out));g_assert_cmpint(out[14],==,0x90);
    g_assert_cmpmem(out+24,3,neutral,3);g_assert_cmpmem(out+30,3,neutral+3,3);
    in[12]=0x10;in[13]=0x80;in[16]=24;
    g_assert_true(pro_reply(&s,in,17,7,out));for(int i=21;i<45;i++)g_assert_cmpint(out[i],==,0xff);
    in[16]=30;g_assert_false(pro_reply(&s,in,17,7,out));
    for(size_t len=0;len<17;len++)g_assert_false(pro_reply(&s,in,len,7,out));
    in[11]=0x30;in[12]=1;g_assert_true(pro_reply(&s,in,13,8,out));g_assert_cmpint(s.lights,==,1);
    in[11]=0x48;in[12]=1;g_assert_true(pro_reply(&s,in,13,8,out));g_assert_true(s.vibration);
    in[11]=0x21;g_assert_true(pro_reply(&s,in,13,8,out));
    g_assert_cmpint(out[14],==,0xa0);g_assert_cmpint(out[49],==,0xc8);
    in[11]=0x22;g_assert_true(pro_reply(&s,in,13,8,out));
    in[11]=0xfe;g_assert_false(pro_reply(&s,in,13,8,out));
    s.buttons[0]=8;s.sticks[0]=4095;pro_input(&s,9,out);
    g_assert_cmpint(out[4],==,8);g_assert_cmpint(out[7],==,255);g_assert_cmpint(out[8]&15,==,15);
}
int main(int argc,char **argv) {g_test_init(&argc,&argv,NULL);g_test_add_func("/pro/wire",reports);return g_test_run();}
