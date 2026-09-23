#include "control.h"
#include "../src/ipc.h"
#include <glib/gstdio.h>
#include <unistd.h>
#include <string.h>
struct ProControl {
    IpcServer *ipc;
    ProState *state;
    gint64 *release_at;
    gboolean *verbose;
    GMainLoop *loop;
    gboolean mock, initialized;
    gint64 last_request;
    char *peer;
    unsigned tx,rx;
    GQueue logs;
    ProReconnectFunc reconnect;
    gpointer reconnect_data;
};
void pro_control_log(ProControl *c,const char *event,const char *detail) {
    if(!c)return;
    g_queue_push_tail(&c->logs,g_strdup_printf("%s  %s",event,detail));
    while(g_queue_get_length(&c->logs)>60)g_free(g_queue_pop_head(&c->logs));
}
static gboolean stop(gpointer data) {g_main_loop_quit(data);return G_SOURCE_REMOVE;}
static char *serialize(JsonObject *object) {
    JsonNode *n=json_node_new(JSON_NODE_OBJECT);json_node_take_object(n,object);
    char *s=json_to_string(n,FALSE);json_node_free(n);return s;
}
static char *failure(const char *message) {
    JsonObject *o=json_object_new();json_object_set_boolean_member(o,"ok",FALSE);
    json_object_set_string_member(o,"error",message);return serialize(o);
}
static gboolean numbers(JsonObject *o,const char *key,int count,int maximum,int *values) {
    JsonNode *n=json_object_get_member(o,key);
    if(!n || !JSON_NODE_HOLDS_ARRAY(n))return FALSE;
    JsonArray *a=json_node_get_array(n);if(json_array_get_length(a)!=(guint)count)return FALSE;
    for(int i=0;i<count;i++) {
        n=json_array_get_element(a,i);
        if(!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n)!=G_TYPE_INT64)return FALSE;
        gint64 value=json_node_get_int(n);if(value<0 || value>maximum)return FALSE;values[i]=(int)value;
    }
    return TRUE;
}
static char *request(gpointer data,const char *text) {
    ProControl *c=data;
    c->last_request=g_get_monotonic_time();
    g_autoptr(JsonParser) p=json_parser_new();
    if(!json_parser_load_from_data(p,text,-1,NULL))return failure("Invalid JSON");
    JsonNode *root=json_parser_get_root(p);if(!JSON_NODE_HOLDS_OBJECT(root))return failure("Expected object");
    JsonObject *o=json_node_get_object(root);JsonNode *vn=json_object_get_member(o,"version");
    if(!vn || !JSON_NODE_HOLDS_VALUE(vn) || json_node_get_value_type(vn)!=G_TYPE_INT64 || json_node_get_int(vn)!=1)return failure("Unsupported or missing version");
    JsonNode *mn=json_object_get_member(o,"method");
    if(!mn || !JSON_NODE_HOLDS_VALUE(mn) || json_node_get_value_type(mn)!=G_TYPE_STRING)return failure("Expected method string");
    const char *method=json_node_get_string(mn);
    if(!strcmp(method,"input")) {
        int buttons[3],sticks[4];
        if(!numbers(o,"buttons",3,255,buttons) || !numbers(o,"sticks",4,4095,sticks))return failure("Invalid input frame");
        for(int i=0;i<3;i++)c->state->buttons[i]=buttons[i];
        for(int i=0;i<4;i++)c->state->sticks[i]=sticks[i];
        *c->release_at=g_get_monotonic_time()+500000;
    } else if(!strcmp(method,"release")) {
        memset(c->state->buttons,0,3);c->state->sticks[0]=2159;c->state->sticks[1]=1916;
        c->state->sticks[2]=2070;c->state->sticks[3]=2013;*c->release_at=0;
    } else if(!strcmp(method,"stop")) {
        g_timeout_add(100,stop,c->loop);
    } else if(!strcmp(method,"reconnect")) {
        const char *address=json_object_get_string_member_with_default(o,"address","");

        if(!c->reconnect)
            return failure("Reconnect is not available for this controller session");

        if(!g_regex_match_simple("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$",
                                 address,0,0))
            return failure("Reconnect requires a valid Bluetooth address");

        GError *error=NULL;
        if(!c->reconnect(address,c->reconnect_data,&error)) {
            char *response=failure(error?error->message:"Reconnect failed");
            g_clear_error(&error);
            return response;
        }
    } else if(!strcmp(method,"logging")) {
        JsonNode *enabled=json_object_get_member(o,"enabled");
        if(!enabled || !JSON_NODE_HOLDS_VALUE(enabled) || json_node_get_value_type(enabled)!=G_TYPE_BOOLEAN)return failure("Expected enabled boolean");
        *c->verbose=json_node_get_boolean(enabled);
    } else if(strcmp(method,"status"))return failure("Unknown method");
    JsonObject *r=json_object_new(),*result=json_object_new();
    json_object_set_boolean_member(r,"ok",TRUE);json_object_set_int_member(r,"version",1);
    json_object_set_object_member(r,"result",result);
    json_object_set_boolean_member(result,"simulated",c->mock);
    json_object_set_boolean_member(result,"initialized",c->initialized);
    json_object_set_string_member(result,"state",c->peer && *c->peer?"connected":"waiting");
    json_object_set_string_member(result,"peer",c->peer?c->peer:"");
    json_object_set_int_member(result,"tx",c->tx);json_object_set_int_member(result,"rx",c->rx);
    json_object_set_int_member(result,"player_lights",c->state->lights);
    const char *profile=c->state->type==CONTROLLER_JOYCON_L?"Nintendo Joy-Con (L)":c->state->type==CONTROLLER_JOYCON_R?"Nintendo Joy-Con (R)":"Nintendo Switch Pro Controller";
    json_object_set_string_member(result,"profile",profile);
    JsonArray *a=json_array_new();for(int i=0;i<3;i++)json_array_add_int_element(a,c->state->buttons[i]);
    json_object_set_array_member(result,"buttons",a);
    a=json_array_new();for(int i=0;i<4;i++)json_array_add_int_element(a,c->state->sticks[i]);
    json_object_set_array_member(result,"sticks",a);
    a=json_array_new();for(GList *l=c->logs.head;l;l=l->next)json_array_add_string_element(a,l->data);
    json_object_set_array_member(result,"logs",a);
    return serialize(r);
}
ProControl *pro_control_new(ProState *state,gint64 *release_at,gboolean *verbose,uid_t owner,gboolean mock,const char *socket_name,GMainLoop *loop,GError **error) {
    g_autofree char *directory=NULL,*path=NULL;
    if(mock) {
        const char *override=g_getenv("PCBLE2GAMEPAD_PRO_SOCKET");
        path=override?g_strdup(override):g_build_filename(g_get_user_runtime_dir(),"pcble2gamepad",socket_name,NULL);
    } else {
        directory=g_strdup_printf("/run/pcble2gamepad/%u",(unsigned)owner);
        if(g_mkdir_with_parents(directory,0755)<0) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Cannot create runtime directory");return NULL;}
        if(chown(directory,owner,(gid_t)-1)<0 || g_chmod(directory,0700)<0) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Cannot secure runtime directory");return NULL;}
        path=g_build_filename(directory,socket_name,NULL);
    }
    ProControl *c=g_new0(ProControl,1);c->state=state;c->release_at=release_at;c->verbose=verbose;c->mock=mock;c->loop=loop;c->last_request=g_get_monotonic_time();
    c->ipc=ipc_server_new_full(request,c,path,owner,error);
    if(!c->ipc) {g_free(c);return NULL;}
    pro_control_log(c,"ipc_ready",path);return c;
}
void pro_control_set_reconnect(ProControl *c,ProReconnectFunc callback,gpointer user_data) {
    if(!c)return;
    c->reconnect=callback;
    c->reconnect_data=user_data;
}

void pro_control_update(ProControl *c,const char *peer,gboolean initialized,unsigned tx,unsigned rx) {
    if(!c)return;
    g_free(c->peer);c->peer=g_strdup(peer);c->initialized=initialized;c->tx=tx;c->rx=rx;
}
gboolean pro_control_expired(ProControl *c) {
    return c && !c->mock && g_get_monotonic_time()-c->last_request>5000000;
}
void pro_control_free(ProControl *c) {
    if(!c)return;
    ipc_server_free(c->ipc);g_queue_clear_full(&c->logs,g_free);g_free(c->peer);g_free(c);
}
