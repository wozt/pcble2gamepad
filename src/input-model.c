#include "input-model.h"
#include <math.h>
#include <gio/gio.h>
#include <string.h>
const char *input_action_names[INPUT_ACTIONS]={"A","B","X","Y","L","R","ZL","ZR","Minus","Plus","Home","Capture","Left stick click","Right stick click","D-pad up","D-pad down","D-pad left","D-pad right","Left stick up","Left stick down","Left stick left","Left stick right","Right stick up","Right stick down","Right stick left","Right stick right"};
void input_profile_defaults(InputProfile *p,gboolean azerty) {
    memset(p,0,sizeof(*p));
    const guint keys[INPUT_ACTIONS]={'l','k','i','j','u','o','7','9','-','=',0xffbe,0xffbf,'c','v',0xff52,0xff54,0xff51,0xff53,'w','s','a','d','t','g','f','h'};
    memcpy(p->keys,keys,sizeof(keys));if(azerty){p->keys[18]='z';p->keys[20]='q';}
    const int buttons[INPUT_BUTTONS]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_MAX,SDL_CONTROLLER_BUTTON_MAX+1,SDL_CONTROLLER_BUTTON_BACK,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_GUIDE,SDL_CONTROLLER_BUTTON_MISC1,SDL_CONTROLLER_BUTTON_LEFTSTICK,SDL_CONTROLLER_BUTTON_RIGHTSTICK,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    memcpy(p->buttons,buttons,sizeof(buttons));p->deadzone=.12;p->sensitivity=1;
    p->swap_face_buttons=TRUE;
}
gboolean input_profile_save(const InputProfile *p,const char *path,GError **error) {
    g_autoptr(GKeyFile) k=g_key_file_new();
    int keys[INPUT_ACTIONS], buttons[INPUT_BUTTONS];
    gboolean invert[4];
    for(int i=0;i<INPUT_ACTIONS;i++)keys[i]=(int)p->keys[i];
    memcpy(buttons,p->buttons,sizeof(buttons));
    memcpy(invert,p->invert,sizeof(invert));
    g_key_file_set_integer_list(k,"Bindings","keys",keys,INPUT_ACTIONS);
    g_key_file_set_integer_list(k,"Bindings","buttons",buttons,INPUT_BUTTONS);
    g_key_file_set_integer(k,"Controller","profile",p->emulated_controller);
    g_key_file_set_double(k,"Sticks","deadzone",p->deadzone);g_key_file_set_double(k,"Sticks","sensitivity",p->sensitivity);
    g_key_file_set_boolean_list(k,"Sticks","invert",invert,4);
    g_key_file_set_boolean(k,"Sticks","swap",p->swap_sticks);g_key_file_set_boolean(k,"Input","background",p->background);
    g_key_file_set_boolean(k,"Input","swap_face_buttons",p->swap_face_buttons);
    return g_key_file_save_to_file(k,path,error);
}
gboolean input_profile_load(InputProfile *p,const char *path,GError **error) {
    g_autoptr(GKeyFile) k=g_key_file_new();if(!g_key_file_load_from_file(k,path,0,error))return FALSE;
    InputProfile next;input_profile_defaults(&next,FALSE);gsize n=0;
    g_autofree int *keys=NULL,*buttons=NULL;g_autofree gboolean *invert=NULL;
    keys=g_key_file_get_integer_list(k,"Bindings","keys",&n,NULL);
    if(!keys || n!=INPUT_ACTIONS)goto invalid;
    for(int i=0;i<INPUT_ACTIONS;i++){if(keys[i]<0)goto invalid;next.keys[i]=(guint)keys[i];}
    buttons=g_key_file_get_integer_list(k,"Bindings","buttons",&n,NULL);
    if(!buttons || n!=INPUT_BUTTONS)goto invalid;
    for(int i=0;i<INPUT_BUTTONS;i++){if(buttons[i]<-1 || buttons[i]>SDL_CONTROLLER_BUTTON_MAX+1)goto invalid;next.buttons[i]=buttons[i];}
    if(g_key_file_has_key(k,"Controller","profile",NULL))
        next.emulated_controller=g_key_file_get_integer(k,"Controller","profile",NULL);
    if(next.emulated_controller<0 || next.emulated_controller>1)goto invalid;
    next.deadzone=g_key_file_get_double(k,"Sticks","deadzone",NULL);next.sensitivity=g_key_file_get_double(k,"Sticks","sensitivity",NULL);
    if(!isfinite(next.deadzone) || next.deadzone<0 || next.deadzone>.5 || !isfinite(next.sensitivity) || next.sensitivity<.25 || next.sensitivity>2)goto invalid;
    invert=g_key_file_get_boolean_list(k,"Sticks","invert",&n,NULL);
    if(!invert || n!=4)goto invalid;
    memcpy(next.invert,invert,sizeof(next.invert));next.swap_sticks=g_key_file_get_boolean(k,"Sticks","swap",NULL);next.background=g_key_file_get_boolean(k,"Input","background",NULL);
    if(g_key_file_has_key(k,"Input","swap_face_buttons",NULL))
        next.swap_face_buttons=g_key_file_get_boolean(k,"Input","swap_face_buttons",NULL);
    *p=next;return TRUE;
invalid:
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Invalid controller profile");return FALSE;
}
void input_gamepad(const InputProfile *p,SDL_GameController *pad,gboolean pressed[INPUT_ACTIONS],double axes[4]) {
    memset(pressed,0,sizeof(gboolean)*INPUT_ACTIONS);memset(axes,0,sizeof(double)*4);
    if(!pad || !SDL_GameControllerGetAttached(pad))return;
    for(int i=0;i<INPUT_BUTTONS;i++) {
        int b=p->buttons[i];if(b<0)continue;
        pressed[i]=b<SDL_CONTROLLER_BUTTON_MAX?SDL_GameControllerGetButton(pad,(SDL_GameControllerButton)b)!=0:SDL_GameControllerGetAxis(pad,b==SDL_CONTROLLER_BUTTON_MAX?SDL_CONTROLLER_AXIS_TRIGGERLEFT:SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000;
    }
    if(p->swap_face_buttons) {
        gboolean value=pressed[0];pressed[0]=pressed[1];pressed[1]=value;
        value=pressed[2];pressed[2]=pressed[3];pressed[3]=value;
    }
    for(int i=0;i<4;i++)axes[i]=SDL_GameControllerGetAxis(pad,(SDL_GameControllerAxis)i)/32768.0;
    axes[1]=-axes[1];axes[3]=-axes[3];
}
void input_compose(const InputProfile *p,const gboolean pressed[INPUT_ACTIONS],const double raw[4],InputFrame *f) {
    memset(f,0,sizeof(*f));memcpy(f->active,pressed,sizeof(f->active));
    const int byte[]={0,0,0,0,2,0,2,0,1,1,1,1,1,1,2,2,2,2};
    const int bit[]={3,2,1,0,6,6,7,7,0,1,4,5,3,2,1,0,3,2};
    for(int i=0;i<INPUT_BUTTONS;i++)if(pressed[i])f->buttons[byte[i]]|=1<<bit[i];
    double a[4];for(int i=0;i<4;i++)a[i]=raw[p->swap_sticks?(i+2)%4:i];
    a[0]+=pressed[21]-pressed[20];a[1]+=pressed[18]-pressed[19];a[2]+=pressed[25]-pressed[24];a[3]+=pressed[22]-pressed[23];
    for(int j=0;j<4;j+=2) {
        double mag=hypot(a[j],a[j+1]);
        double scale=mag>p->deadzone?MIN(1.,(mag-p->deadzone)/(1.-p->deadzone))*p->sensitivity/mag:0;
        a[j]*=scale;a[j+1]*=scale;
    }
    const int center[]={2159,1916,2070,2013},positive[]={1466,1583,1414,1510},negative[]={1517,1465,1522,1531};
    for(int i=0;i<4;i++) {
        double value=CLAMP(a[i]*(p->invert[i]?-1:1),-1.,1.);f->axes[i]=value;
        f->sticks[i]=(guint16)lrint(center[i]+value*(value>=0?positive[i]:negative[i]));
    }
}
