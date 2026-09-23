#define _GNU_SOURCE
#include "protocol.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROOT "/io/github/wozt/pcble2gamepad/pro_poc"
static GDBusConnection *bus;
static GMainLoop *loop;
static char adapter[64], peer[18];
static int channels[2]={-1,-1}, listeners[2]={-1,-1};
static guint watches[2];
static ProState state;
static unsigned sent, received;
static gboolean initialized;
static gint64 release_at;
static void log_event(const char *event, const char *detail) {
    GDateTime *now=g_date_time_new_now_utc();
    char *time=g_date_time_format_iso8601(now);
    g_print("%s %s %s\n",time,event,detail); fflush(stdout);
    g_free(time); g_date_time_unref(now);
}
static GVariant *call(const char *path,const char *iface,const char *method,GVariant *args) {
    GError *err=NULL;
    GVariant *r=g_dbus_connection_call_sync(bus,"org.bluez",path,iface,method,args,NULL,
        G_DBUS_CALL_FLAGS_NONE,5000,NULL,&err);
    if (!r) { log_event(method,err->message); g_error_free(err); }
    return r;
}
static gboolean invoke(const char *path,const char *iface,const char *method,GVariant *args) {
    GVariant *r=call(path,iface,method,args);
    if (!r) return FALSE;
    g_variant_unref(r); return TRUE;
}
static GVariant *property(const char *key) {
    GVariant *r=call(adapter,"org.freedesktop.DBus.Properties","Get",
        g_variant_new("(ss)","org.bluez.Adapter1",key));
    if (!r) return NULL;
    GVariant *v; g_variant_get(r,"(v)",&v); g_variant_unref(r); return v;
}
static gboolean set_property(const char *key,GVariant *value) {
    return invoke(adapter,"org.freedesktop.DBus.Properties","Set",
        g_variant_new("(ssv)","org.bluez.Adapter1",key,value));
}
static void agent(GDBusConnection *c,const char *sender,const char *path,const char *iface,
    const char *method,GVariant *args,GDBusMethodInvocation *inv,gpointer data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)data;
    const char *device=NULL;
    if (g_variant_n_children(args)) g_variant_get_child(args,0,"&o",&device);
    char *detail=g_strdup_printf("method=%s device=%s",method,device?device:"none");
    log_event("pairing_agent",detail); g_free(detail);
    char *prefix=g_strconcat(adapter,"/dev_",NULL);
    gboolean allowed=!device || g_str_has_prefix(device,prefix); g_free(prefix);
    if (!allowed) {
        g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Rejected","Outside selected adapter");return;
    }
    if (!strcmp(method,"RequestPinCode")) g_dbus_method_invocation_return_value(inv,g_variant_new("(s)","0000"));
    else if (!strcmp(method,"RequestPasskey")) g_dbus_method_invocation_return_value(inv,g_variant_new("(u)",0u));
    else g_dbus_method_invocation_return_value(inv,NULL);
}
static void profile(GDBusConnection *c,const char *sender,const char *path,const char *iface,
    const char *method,GVariant *args,GDBusMethodInvocation *inv,gpointer data) {
    (void)c;(void)sender;(void)path;(void)iface;(void)args;(void)data;
    log_event("profile",method);
    if (!strcmp(method,"NewConnection"))
        g_dbus_method_invocation_return_dbus_error(inv,"org.bluez.Error.Rejected","POC owns explicit HID listeners");
    else g_dbus_method_invocation_return_value(inv,NULL);
}
static void changed(GDBusConnection *c,const char *sender,const char *path,const char *iface,
    const char *signal,GVariant *args,gpointer data) {
    (void)c;(void)sender;(void)iface;(void)signal;(void)data;
    if (!g_str_has_prefix(path,adapter)) return;
    char *v=g_variant_print(args,TRUE), *msg=g_strdup_printf("path=%s %s",path,v);
    log_event("bluez_properties",msg); g_free(msg);g_free(v);
}
static void reset_link(void) {
    for(int i=0;i<2;i++) {
        if(watches[i]) {g_source_remove(watches[i]);watches[i]=0;}
        if(channels[i]>=0) close(channels[i]);
        channels[i]=-1;
    }
    peer[0]=0; initialized=FALSE;
    uint8_t addr[6];memcpy(addr,state.address,6);pro_init(&state,addr);
    log_event("link_closed","Waiting for a new control/interrupt connection; see btmon for HCI reason");
}
static uint8_t timer_byte(void) {return (uint8_t)(g_get_monotonic_time()/5000);}
static gboolean send_report(const uint8_t out[50]) {
    ssize_t n=send(channels[1],out,50,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==50) {sent++; return TRUE;}
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return TRUE;
    log_event("hid_send_error",strerror(errno));return FALSE;
}
static gboolean receive_packet(gint fd,GIOCondition cond,gpointer user) {
    int index=GPOINTER_TO_INT(user);
    if(cond&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)) {watches[index]=0;reset_link();return G_SOURCE_REMOVE;}
    uint8_t in[1024],out[50];ssize_t n=recv(fd,in,sizeof(in),MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return G_SOURCE_CONTINUE;
    if(n<=0) {watches[index]=0;reset_link();return G_SOURCE_REMOVE;}
    received++;
    char *hex=g_malloc((size_t)n*2+1);
    for(ssize_t i=0;i<n;i++) sprintf(hex+2*i,"%02x",in[i]);
    char *msg=g_strdup_printf("psm=%d len=%zd data=%s",index?19:17,n,hex);
    log_event("hid_rx",msg);g_free(msg);g_free(hex);
    if(!index) {
        /* SET_PROTOCOL / SET_IDLE acknowledge on the HID control channel. */
        if((in[0]&0xf0)==0x70 || (in[0]&0xf0)==0x90) {
            uint8_t ok=0;ssize_t ignored=send(fd,&ok,1,MSG_NOSIGNAL);(void)ignored;
        }
        return G_SOURCE_CONTINUE;
    }
    if(pro_reply(&state,in,(size_t)n,timer_byte(),out)) {
        if(!send_report(out)) {watches[index]=0;reset_link();return G_SOURCE_REMOVE;}
        char detail[96];snprintf(detail,sizeof(detail),"subcommand=0x%02x mode=0x%02x lights=0x%02x",in[11],state.mode,state.lights);
        log_event("hid_reply",detail);
        if(state.lights && state.vibration && !initialized) {
            initialized=TRUE;log_event("initialization_observed","Player lights and vibration configured; console visibility/input still require user verification");
        }
    } else if(n>=2 && in[0]==0xa2 && in[1]==0x01) log_event("unsupported_subcommand","No invented ACK sent");
    return G_SOURCE_CONTINUE;
}
static gboolean accept_peer(gint fd,GIOCondition cond,gpointer user) {
    (void)cond;int index=GPOINTER_TO_INT(user);
    struct sockaddr_l2 addr={0};socklen_t size=sizeof(addr);
    int client=accept4(fd,(struct sockaddr *)&addr,&size,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if(client<0) return G_SOURCE_CONTINUE;
    char address[18];ba2str(&addr.l2_bdaddr,address);
    if(channels[index]>=0 || (peer[0] && strcmp(peer,address))) {
        log_event("peer_rejected",address);close(client);return G_SOURCE_CONTINUE;
    }
    g_strlcpy(peer,address,sizeof(peer));channels[index]=client;
    watches[index]=g_unix_fd_add(client,G_IO_IN|G_IO_HUP|G_IO_ERR,receive_packet,GINT_TO_POINTER(index));
    char msg[100];snprintf(msg,sizeof(msg),"peer=%s psm=%d",address,index?19:17);log_event("l2cap_connected",msg);
    return G_SOURCE_CONTINUE;
}
static gboolean tick(gpointer unused) {
    (void)unused;
    if(release_at && g_get_monotonic_time()>=release_at) {
        memset(state.buttons,0,3);state.sticks[0]=0x86f;state.sticks[1]=0x77c;state.sticks[2]=0x816;state.sticks[3]=0x7dd;
        release_at=0;log_event("input_released","neutral");
    }
    if(channels[1]>=0 && channels[0]>=0) {
        uint8_t out[50];pro_input(&state,timer_byte(),out);
        if(!send_report(out)) reset_link();
    }
    return G_SOURCE_CONTINUE;
}
static gboolean stats(gpointer unused) {
    (void)unused;char msg[120];snprintf(msg,sizeof(msg),"peer=%s tx=%u rx=%u initialized=%s",peer[0]?peer:"none",sent,received,initialized?"true":"false");
    log_event("status",msg);return G_SOURCE_CONTINUE;
}
static gboolean quit(gpointer unused) {(void)unused;g_main_loop_quit(loop);return G_SOURCE_CONTINUE;}
static gboolean input(GIOChannel *io,GIOCondition cond,gpointer unused) {
    (void)unused;if(cond&G_IO_HUP) return G_SOURCE_REMOVE;
    char *line=NULL;gsize len;
    if(g_io_channel_read_line(io,&line,&len,NULL,NULL)!=G_IO_STATUS_NORMAL) return G_SOURCE_CONTINUE;
    g_strstrip(line);
    if(!strcmp(line,"quit")) quit(NULL);
    else if(!strcmp(line,"release")) release_at=1;
    else if(!strcmp(line,"status")) stats(NULL);
    else if(g_str_has_prefix(line,"buttons ")) {
        unsigned a,b,c;if(sscanf(line+8,"%x %x %x",&a,&b,&c)==3 && a<256 && b<256 && c<256) {
            state.buttons[0]=a;state.buttons[1]=b;state.buttons[2]=c;
            release_at=g_get_monotonic_time()+500000;log_event("input_buttons",line+8);
        } else log_event("input_error","buttons expects three hexadecimal bytes");
    } else if(g_str_has_prefix(line,"sticks ")) {
        unsigned a,b,c,d;if(sscanf(line+7,"%u %u %u %u",&a,&b,&c,&d)==4 && a<4096 && b<4096 && c<4096 && d<4096) {
            state.sticks[0]=a;state.sticks[1]=b;state.sticks[2]=c;state.sticks[3]=d;
            release_at=g_get_monotonic_time()+500000;log_event("input_sticks",line+7);
        } else log_event("input_error","sticks expects four 12-bit decimal values");
    } else log_event("input_help","buttons HEX HEX HEX | sticks LX LY RX RY | release | status | quit; inputs release after 500 ms");
    g_free(line);return G_SOURCE_CONTINUE;
}
static const char xml[]=
"<node><interface name='org.bluez.Agent1'>"
"<method name='Release'/><method name='Cancel'/>"
"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
"</interface><interface name='org.bluez.Profile1'><method name='Release'/>"
"<method name='NewConnection'><arg type='o' direction='in'/><arg type='h' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='RequestDisconnection'><arg type='o' direction='in'/></method></interface></node>";
int main(int argc,char **argv) {
    if(argc!=3 || !g_regex_match_simple("^hci[0-9]+$",argv[1],0,0)) {
        fprintf(stderr,"Usage: %s hciN SDP_XML\n",argv[0]);return 2;
    }
    int result=1,dd=-1;uint8_t old_class[3]={0};gboolean have_class=FALSE;
    GVariant *saved[5]={0};const char *keys[]={"Alias","Pairable","Discoverable","PairableTimeout","DiscoverableTimeout"};
    guint agent_id=0,profile_id=0;gboolean registered_agent=FALSE,registered_profile=FALSE;
    snprintf(adapter,sizeof(adapter),"/org/bluez/%s",argv[1]);
    GError *err=NULL;bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,&err);
    if(!bus) {fprintf(stderr,"%s\n",err->message);g_error_free(err);return 1;}
    GDBusNodeInfo *node=g_dbus_node_info_new_for_xml(xml,NULL);
    GVariant *v=property("Address");if(!v) goto cleanup;
    const char *address=g_variant_get_string(v,NULL);bdaddr_t local;
    str2ba(address,&local);uint8_t mac[6];for(int i=0;i<6;i++) mac[i]=local.b[5-i];
    pro_init(&state,mac);log_event("adapter_selected",address);g_variant_unref(v);
    v=property("Powered");if(!v) goto cleanup;
    gboolean powered=g_variant_get_boolean(v);g_variant_unref(v);
    if(!powered) {log_event("error","Adapter must already be powered");goto cleanup;}
    for(int i=0;i<5;i++) {saved[i]=property(keys[i]);if(!saved[i]) goto cleanup;}
    dd=hci_open_dev(hci_devid(argv[1]));
    if(dd<0 || hci_read_class_of_dev(dd,old_class,2000)<0) {log_event("hci_error",strerror(errno));goto cleanup;}
    have_class=TRUE;
    static const GDBusInterfaceVTable av={.method_call=agent},pv={.method_call=profile};
    agent_id=g_dbus_connection_register_object(bus,ROOT "/agent",node->interfaces[0],&av,NULL,NULL,&err);
    profile_id=g_dbus_connection_register_object(bus,ROOT "/profile",node->interfaces[1],&pv,NULL,NULL,&err);
    if(!agent_id || !profile_id) goto cleanup;
    if(!invoke("/org/bluez","org.bluez.AgentManager1","RegisterAgent",g_variant_new("(os)",ROOT "/agent","DisplayYesNo"))) goto cleanup;
    registered_agent=TRUE;
    if(!invoke("/org/bluez","org.bluez.AgentManager1","RequestDefaultAgent",g_variant_new("(o)",ROOT "/agent"))) goto cleanup;
    log_event("agent_registered","DisplayYesNo; accepts requests only on selected adapter");
    char *record=NULL;if(!g_file_get_contents(argv[2],&record,NULL,&err)) goto cleanup;
    GVariantBuilder opts;g_variant_builder_init(&opts,G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts,"{sv}","ServiceRecord",g_variant_new_string(record));g_free(record);
    g_variant_builder_add(&opts,"{sv}","Role",g_variant_new_string("server"));
    g_variant_builder_add(&opts,"{sv}","RequireAuthentication",g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts,"{sv}","RequireAuthorization",g_variant_new_boolean(FALSE));
    if(!invoke("/org/bluez","org.bluez.ProfileManager1","RegisterProfile",g_variant_new("(osa{sv})",ROOT "/profile","00001000-0000-1000-8000-00805f9b34fb",&opts))) goto cleanup;
    registered_profile=TRUE;log_event("sdp_registered","Switch 1 Pro Controller HID record");
    for(int i=0;i<2;i++) {
        listeners[i]=socket(AF_BLUETOOTH,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,BTPROTO_L2CAP);
        struct sockaddr_l2 bind_addr={.l2_family=AF_BLUETOOTH,.l2_psm=htobs(i?19:17)};
        bacpy(&bind_addr.l2_bdaddr,&local);
        if(listeners[i]<0 || bind(listeners[i],(struct sockaddr *)&bind_addr,sizeof(bind_addr))<0 || listen(listeners[i],1)<0) {
            log_event("l2cap_listen_error",strerror(errno));goto cleanup;
        }
        g_unix_fd_add(listeners[i],G_IO_IN,accept_peer,GINT_TO_POINTER(i));
    }
    if(!set_property("Alias",g_variant_new_string("Pro Controller")) ||
       !set_property("PairableTimeout",g_variant_new_uint32(180)) ||
       !set_property("DiscoverableTimeout",g_variant_new_uint32(180)) ||
       !set_property("Pairable",g_variant_new_boolean(TRUE)) ||
       !set_property("Discoverable",g_variant_new_boolean(TRUE))) goto cleanup;
    if(hci_write_class_of_dev(dd,0x002508,2000)<0) {log_event("class_error",strerror(errno));goto cleanup;}
    log_event("ready","Name=Pro Controller class=0x002508 discoverable=true pairable=true PSM=17/19; open Change Grip/Order");
    g_dbus_connection_signal_subscribe(bus,"org.bluez","org.freedesktop.DBus.Properties","PropertiesChanged",NULL,NULL,0,changed,NULL,NULL);
    loop=g_main_loop_new(NULL,FALSE);g_unix_signal_add(SIGINT,quit,NULL);g_unix_signal_add(SIGTERM,quit,NULL);
    g_timeout_add(15,tick,NULL);g_timeout_add_seconds(5,stats,NULL);g_timeout_add_seconds(600,quit,NULL);
    GIOChannel *io=g_io_channel_unix_new(STDIN_FILENO);
    g_io_channel_set_flags(io,g_io_channel_get_flags(io)|G_IO_FLAG_NONBLOCK,NULL);
    g_io_add_watch(io,G_IO_IN|G_IO_HUP,input,NULL);
    g_main_loop_run(loop);g_io_channel_unref(io);g_main_loop_unref(loop);result=0;
cleanup:
    if(err) {log_event("error",err->message);g_error_free(err);}
    reset_link();for(int i=0;i<2;i++) if(listeners[i]>=0) close(listeners[i]);
    if(registered_profile) invoke("/org/bluez","org.bluez.ProfileManager1","UnregisterProfile",g_variant_new("(o)",ROOT "/profile"));
    if(registered_agent) invoke("/org/bluez","org.bluez.AgentManager1","UnregisterAgent",g_variant_new("(o)",ROOT "/agent"));
    gboolean restored=TRUE;
    for(int i=4;i>=0;i--) if(saved[i]) {
        if(!set_property(keys[i],saved[i])) restored=FALSE;
        g_variant_unref(saved[i]);
    }
    if(have_class && hci_write_class_of_dev(dd,old_class[0]|((uint32_t)old_class[1]<<8)|((uint32_t)old_class[2]<<16),2000)<0) restored=FALSE;
    if(dd>=0) close(dd);
    if(profile_id) g_dbus_connection_unregister_object(bus,profile_id);
    if(agent_id) g_dbus_connection_unregister_object(bus,agent_id);
    g_dbus_node_info_unref(node);g_object_unref(bus);log_event("stopped",restored?"Adapter properties restored; pairing keys retained by BlueZ":"Adapter restoration incomplete; inspect earlier errors");return restored?result:1;
}
