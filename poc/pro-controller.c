#define _GNU_SOURCE
#include "protocol.h"
#include "control.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROOT "/io/github/wozt/pcble2gamepad/pro_poc"
static GDBusConnection *bus;
static ProControl *desktop;
static gboolean mock_mode;
static GMainLoop *loop;
static char adapter[64], allowed_adapter[64], peer[18], last_peer[18];
static ControllerType controller_type=CONTROLLER_PRO;
static const char *controller_alias="Pro Controller",*control_socket="pro.sock";
static gboolean shared_profile;
static gboolean verbose_traffic;
static gboolean reconnect_mode;
static char reconnect_peer[18];
static bdaddr_t selected_local_address;
static gboolean have_selected_local_address;

/*
 * pcble2gamepad owns a small persistent BR/EDR pairing store because some
 * Nintendo-style pairing flows generate a valid kernel Link Key with
 * store_hint=0. BlueZ then exposes Paired=true for the live session but does
 * not persist the key for the next bluetoothd/host restart.
 */
static uid_t pairing_owner;
static int pairing_mgmt_index=-1;
static int pairing_mgmt_fd=-1;
static guint pairing_mgmt_watch;
static gboolean pairing_mgmt_key_seen;

static int pairing_hci_fd=-1;
static guint pairing_hci_watch;

static guint pairing_reconnect_source;
static unsigned pairing_handoff_attempts;
static gboolean shutting_down;

static char pairing_local_address[18];

#define PC_MGMT_EV_CMD_COMPLETE   0x0001
#define PC_MGMT_EV_CMD_STATUS     0x0002
#define PC_MGMT_EV_NEW_LINK_KEY   0x0009
#define PC_MGMT_OP_LOAD_LINK_KEYS 0x0012
#define PC_MGMT_OP_SET_DEVICE_ID   0x0028

#ifndef HCI_CHANNEL_MONITOR
#define HCI_CHANNEL_MONITOR 2
#endif

#ifndef HCI_CHANNEL_CONTROL
#define HCI_CHANNEL_CONTROL 3
#endif

#define PC_MONITOR_COMMAND_PKT 2
#define PC_MONITOR_EVENT_PKT   3

typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint16_t index;
    uint16_t len;
} PcMgmtHdr;

typedef struct __attribute__((packed)) {
    bdaddr_t bdaddr;
    uint8_t type;
} PcMgmtAddrInfo;

typedef struct __attribute__((packed)) {
    PcMgmtAddrInfo addr;
    uint8_t type;
    uint8_t val[16];
    uint8_t pin_len;
} PcMgmtLinkKey;

typedef struct __attribute__((packed)) {
    uint8_t store_hint;
    PcMgmtLinkKey key;
} PcMgmtNewLinkKey;

typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint8_t status;
} PcMgmtCommandResult;

typedef struct __attribute__((packed)) {
    PcMgmtHdr hdr;
    uint8_t debug_keys;
    uint16_t key_count;
    PcMgmtLinkKey key;
} PcMgmtLoadOneKey;

typedef struct __attribute__((packed)) {
    PcMgmtHdr hdr;
    uint16_t source;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} PcMgmtSetDeviceId;

typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint16_t index;
    uint16_t len;
} PcMonitorHdr;
static gboolean select_controller(const char *type) {
    if(!strcmp(type,"pro")){controller_type=CONTROLLER_PRO;controller_alias="Pro Controller";control_socket="pro.sock";return TRUE;}
    if(!strcmp(type,"joycon-l")){controller_type=CONTROLLER_JOYCON_L;controller_alias="Joy-Con (L)";control_socket="joycon-left.sock";return TRUE;}
    if(!strcmp(type,"joycon-r")){controller_type=CONTROLLER_JOYCON_R;controller_alias="Joy-Con (R)";control_socket="joycon-right.sock";return TRUE;}
    return FALSE;
}
static int channels[2]={-1,-1}, listeners[2]={-1,-1};
static guint watches[2];
static ProState state;

static uint8_t configured_body_color[3]={0x82,0x82,0x82};
static uint8_t configured_button_color[3]={0x0f,0x0f,0x0f};
static uint8_t configured_left_grip_color[3]={0x82,0x82,0x82};
static uint8_t configured_right_grip_color[3]={0x82,0x82,0x82};

static gboolean parse_rgb_color(const char *text,uint8_t color[3]) {
    if(!text || strlen(text)!=6)
        return FALSE;

    unsigned r,g,b;
    char extra;

    if(sscanf(text,"%2x%2x%2x%c",&r,&g,&b,&extra)!=3)
        return FALSE;

    color[0]=(uint8_t)r;
    color[1]=(uint8_t)g;
    color[2]=(uint8_t)b;
    return TRUE;
}

static void apply_controller_colors(void) {
    if(controller_type!=CONTROLLER_PRO)
        return;

    controller_set_colors(
        &state,
        configured_body_color,
        configured_button_color,
        configured_left_grip_color,
        configured_right_grip_color);
}

static unsigned sent, received;
static gboolean initialized;
static gint64 release_at,next_report_at,slow_exit_until;
static gboolean slow_input_frequency,saw_interrupt_output;
typedef struct { char *event, *detail; } PendingLog;
static GQueue pending_logs = G_QUEUE_INIT;
static gboolean desktop_requested;
static void pending_log_free(PendingLog *item) {
    g_free(item->event);g_free(item->detail);g_free(item);
}
static void log_event(const char *event, const char *detail) {
    if(!verbose_traffic && !strcmp(event,"hid_rx"))return;
    if(desktop)pro_control_log(desktop,event,detail);
    else if(desktop_requested) {
        PendingLog *item=g_new0(PendingLog,1);item->event=g_strdup(event);item->detail=g_strdup(detail);
        g_queue_push_tail(&pending_logs,item);
        while(g_queue_get_length(&pending_logs)>60)pending_log_free(g_queue_pop_head(&pending_logs));
    }
    GDateTime *now=g_date_time_new_now_utc();
    char *time=g_date_time_format_iso8601(now);
    g_print("%s %s %s\n",time,event,detail); fflush(stdout);
    g_free(time); g_date_time_unref(now);
}

static int pairing_mgmt_open(gboolean nonblocking) {
    int flags=SOCK_RAW|SOCK_CLOEXEC;
    if(nonblocking)flags|=SOCK_NONBLOCK;

    int fd=socket(PF_BLUETOOTH,flags,BTPROTO_HCI);
    if(fd<0)return -1;

    struct sockaddr_hci address={0};
    address.hci_family=AF_BLUETOOTH;
    address.hci_dev=HCI_DEV_NONE;
    address.hci_channel=HCI_CHANNEL_CONTROL;

    if(bind(fd,(struct sockaddr *)&address,sizeof(address))<0) {
        close(fd);
        return -1;
    }

    return fd;
}

static char *pairing_safe_address(const char *address) {
    char *copy=g_strdup(address);
    for(char *p=copy;*p;p++)
        if(*p==':')*p='_';
    return copy;
}

static char *pairing_store_path(const char *remote,gboolean create) {
    g_autofree char *local_safe=pairing_safe_address(pairing_local_address);
    g_autofree char *remote_safe=pairing_safe_address(remote);
    g_autofree char *base=g_strdup_printf(
        "/var/lib/pcble2gamepad/pairings/%u",
        (unsigned)pairing_owner);

    if(create) {
        if(g_mkdir_with_parents(base,0700)<0)
            return NULL;

        g_chmod("/var/lib/pcble2gamepad",0700);
        g_chmod("/var/lib/pcble2gamepad/pairings",0700);
        g_chmod(base,0700);
    }

    g_autofree char *name=g_strdup_printf(
        "%s__%s.ini",local_safe,remote_safe);
    return g_build_filename(base,name,NULL);
}

static gboolean pairing_write_all(int fd,const char *data,gsize size) {
    gsize offset=0;

    while(offset<size) {
        ssize_t written=write(fd,data+offset,size-offset);
        if(written<0) {
            if(errno==EINTR)continue;
            return FALSE;
        }
        offset+=(gsize)written;
    }

    return TRUE;
}

static gboolean pairing_save_key(const PcMgmtNewLinkKey *event) {
    char remote[18];
    ba2str(&event->key.addr.bdaddr,remote);

    if(event->key.addr.type!=0)
        return TRUE;

    g_autofree char *path=pairing_store_path(remote,TRUE);
    if(!path) {
        log_event("pairing_key_error","Cannot create persistent pairing directory");
        return FALSE;
    }

    GKeyFile *config=g_key_file_new();
    g_key_file_set_integer(config,"Pairing","Version",1);
    g_key_file_set_string(config,"Pairing","Controller","pro");
    g_key_file_set_string(config,"Pairing","AdapterAddress",pairing_local_address);
    g_key_file_set_string(config,"Pairing","SwitchAddress",remote);
    g_key_file_set_integer(config,"Pairing","KeyType",event->key.type);
    g_key_file_set_integer(config,"Pairing","PINLength",event->key.pin_len);
    g_key_file_set_integer(config,"Pairing","StoreHint",event->store_hint);

    g_autofree char *encoded=g_base64_encode(event->key.val,sizeof(event->key.val));
    g_key_file_set_string(config,"Pairing","LinkKeyBase64",encoded);

    GDateTime *now=g_date_time_new_now_utc();
    g_autofree char *timestamp=g_date_time_format_iso8601(now);
    g_date_time_unref(now);
    g_key_file_set_string(config,"Pairing","SavedAt",timestamp);

    gsize length=0;
    g_autofree char *contents=g_key_file_to_data(config,&length,NULL);
    g_key_file_unref(config);

    int fd=open(path,
        O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,
        0600);

    if(fd<0) {
        log_event("pairing_key_error",strerror(errno));
        return FALSE;
    }

    gboolean ok=
        fchmod(fd,0600)==0 &&
        pairing_write_all(fd,contents,length) &&
        fsync(fd)==0;

    close(fd);

    if(!ok) {
        log_event("pairing_key_error","Failed to persist BR/EDR Link Key");
        return FALSE;
    }

    char detail[160];
    snprintf(detail,sizeof(detail),
        "peer=%s key_type=%u store_hint=%u persistent=true",
        remote,event->key.type,event->store_hint);
    log_event("pairing_key_saved",detail);

    return TRUE;
}


static const char *pairing_io_name(uint8_t capability) {
    switch(capability) {
    case 0x00: return "DisplayOnly";
    case 0x01: return "DisplayYesNo";
    case 0x02: return "KeyboardOnly";
    case 0x03: return "NoInputNoOutput";
    default: return "unknown";
    }
}

static const char *pairing_auth_name(uint8_t authentication) {
    switch(authentication) {
    case 0x00: return "no-bonding/no-mitm";
    case 0x01: return "no-bonding/mitm";
    case 0x02: return "dedicated-bonding/no-mitm";
    case 0x03: return "dedicated-bonding/mitm";
    case 0x04: return "general-bonding/no-mitm";
    case 0x05: return "general-bonding/mitm";
    default: return "unknown";
    }
}

static gboolean pairing_hci_event(gint fd,
                                   GIOCondition condition,
                                   gpointer unused) {
    (void)unused;

    if(condition&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)) {
        pairing_hci_watch=0;
        return G_SOURCE_REMOVE;
    }

    uint8_t buffer[4096];
    ssize_t size=recv(fd,buffer,sizeof(buffer),MSG_DONTWAIT);

    if(size<0 && (errno==EAGAIN || errno==EWOULDBLOCK))
        return G_SOURCE_CONTINUE;

    if(size<(ssize_t)sizeof(PcMonitorHdr))
        return G_SOURCE_CONTINUE;

    const PcMonitorHdr *monitor=
        (const PcMonitorHdr *)buffer;

    uint16_t opcode=btohs(monitor->opcode);
    uint16_t index=btohs(monitor->index);
    uint16_t packet_size=btohs(monitor->len);

    if(index!=(uint16_t)pairing_mgmt_index)
        return G_SOURCE_CONTINUE;

    size_t available=
        (size_t)size-sizeof(PcMonitorHdr);

    if(packet_size>available)
        packet_size=(uint16_t)available;

    const uint8_t *packet=
        buffer+sizeof(PcMonitorHdr);

    if(opcode==PC_MONITOR_COMMAND_PKT) {
        if(packet_size<HCI_COMMAND_HDR_SIZE+IO_CAPABILITY_REPLY_CP_SIZE)
            return G_SOURCE_CONTINUE;

        const hci_command_hdr *command=
            (const hci_command_hdr *)packet;

        if(btohs(command->opcode)!=
           cmd_opcode_pack(OGF_LINK_CTL,OCF_IO_CAPABILITY_REPLY))
            return G_SOURCE_CONTINUE;

        const io_capability_reply_cp *reply=
            (const io_capability_reply_cp *)(packet+HCI_COMMAND_HDR_SIZE);

        char remote[18];
        ba2str(&reply->bdaddr,remote);

        char detail[256];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s io=%s(0x%02x) auth=%s(0x%02x) oob=0x%02x",
            remote,
            pairing_io_name(reply->capability),
            reply->capability,
            pairing_auth_name(reply->authentication),
            reply->authentication,
            reply->oob_data);

        log_event("pairing_local_io",detail);
        return G_SOURCE_CONTINUE;
    }

    if(opcode!=PC_MONITOR_EVENT_PKT ||
       packet_size<HCI_EVENT_HDR_SIZE)
        return G_SOURCE_CONTINUE;

    const hci_event_hdr *header=
        (const hci_event_hdr *)packet;

    const uint8_t *payload=
        packet+HCI_EVENT_HDR_SIZE;

    if(header->evt==EVT_IO_CAPABILITY_RESPONSE) {
        if(packet_size<
           HCI_EVENT_HDR_SIZE+EVT_IO_CAPABILITY_RESPONSE_SIZE)
            return G_SOURCE_CONTINUE;

        const evt_io_capability_response *response=
            (const evt_io_capability_response *)payload;

        char remote[18];
        ba2str(&response->bdaddr,remote);

        char detail[256];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s io=%s(0x%02x) auth=%s(0x%02x) oob=0x%02x",
            remote,
            pairing_io_name(response->capability),
            response->capability,
            pairing_auth_name(response->authentication),
            response->authentication,
            response->oob_data);

        log_event("pairing_peer_io",detail);
        return G_SOURCE_CONTINUE;
    }

    if(header->evt==EVT_SIMPLE_PAIRING_COMPLETE) {
        if(packet_size<
           HCI_EVENT_HDR_SIZE+EVT_SIMPLE_PAIRING_COMPLETE_SIZE)
            return G_SOURCE_CONTINUE;

        const evt_simple_pairing_complete *complete=
            (const evt_simple_pairing_complete *)payload;

        char remote[18];
        ba2str(&complete->bdaddr,remote);

        char detail[160];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s status=0x%02x",
            remote,
            complete->status);

        log_event("pairing_ssp_complete",detail);
        return G_SOURCE_CONTINUE;
    }

    if(header->evt!=EVT_LINK_KEY_NOTIFY)
        return G_SOURCE_CONTINUE;

    if(packet_size<
       HCI_EVENT_HDR_SIZE+EVT_LINK_KEY_NOTIFY_SIZE)
        return G_SOURCE_CONTINUE;

    const evt_link_key_notify *notification=
        (const evt_link_key_notify *)payload;

    PcMgmtNewLinkKey event={0};

    event.store_hint=0;
    bacpy(
        &event.key.addr.bdaddr,
        &notification->bdaddr);

    event.key.addr.type=0;
    event.key.type=notification->key_type;

    memcpy(
        event.key.val,
        notification->link_key,
        sizeof(event.key.val));

    event.key.pin_len=0;

    char remote[18];
    ba2str(&notification->bdaddr,remote);

    char detail[160];
    snprintf(
        detail,
        sizeof(detail),
        "peer=%s key_type=%u source=HCI_MONITOR",
        remote,
        notification->key_type);

    log_event("pairing_key_hci",detail);

    if(!pairing_mgmt_key_seen && !pairing_save_key(&event))
        log_event(
            "pairing_key_error",
            "HCI monitor saw Link Key but persistence failed");

    return G_SOURCE_CONTINUE;
}

static gboolean pairing_hci_capture_start(void) {
    pairing_hci_fd=socket(
        PF_BLUETOOTH,
        SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,
        BTPROTO_HCI);

    if(pairing_hci_fd<0) {
        log_event(
            "pairing_key_error",
            "Cannot open HCI monitor socket");
        return FALSE;
    }

    struct sockaddr_hci address={0};
    address.hci_family=AF_BLUETOOTH;

    /*
     * Monitor mode observes all adapters, just like btmon.
     * pairing_hci_event() filters packets by the selected HCI index.
     */
    address.hci_dev=HCI_DEV_NONE;
    address.hci_channel=HCI_CHANNEL_MONITOR;

    if(bind(
            pairing_hci_fd,
            (struct sockaddr *)&address,
            sizeof(address))<0) {
        log_event(
            "pairing_key_error",
            "Cannot bind HCI monitor socket");

        close(pairing_hci_fd);
        pairing_hci_fd=-1;
        return FALSE;
    }

    pairing_hci_watch=g_unix_fd_add(
        pairing_hci_fd,
        G_IO_IN|G_IO_HUP|G_IO_ERR,
        pairing_hci_event,
        NULL);

    log_event(
        "pairing_hci_capture",
        "HCI monitor Link Key capture armed");

    return TRUE;
}

static gboolean pairing_mgmt_event(gint fd,GIOCondition condition,gpointer unused) {
    (void)unused;

    if(condition&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)) {
        pairing_mgmt_watch=0;
        return G_SOURCE_REMOVE;
    }

    uint8_t buffer[1024];
    ssize_t size=recv(fd,buffer,sizeof(buffer),MSG_DONTWAIT);

    if(size<0 && (errno==EAGAIN || errno==EWOULDBLOCK))
        return G_SOURCE_CONTINUE;

    if(size<(ssize_t)sizeof(PcMgmtHdr))
        return G_SOURCE_CONTINUE;

    PcMgmtHdr *header=(PcMgmtHdr *)buffer;
    uint16_t opcode=btohs(header->opcode);
    uint16_t index=btohs(header->index);
    uint16_t payload_size=btohs(header->len);

    if(index!=(uint16_t)pairing_mgmt_index)
        return G_SOURCE_CONTINUE;

    if(opcode!=PC_MGMT_EV_NEW_LINK_KEY ||
       payload_size<sizeof(PcMgmtNewLinkKey) ||
       size<(ssize_t)(sizeof(PcMgmtHdr)+sizeof(PcMgmtNewLinkKey)))
        return G_SOURCE_CONTINUE;

    const PcMgmtNewLinkKey *event=
        (const PcMgmtNewLinkKey *)(buffer+sizeof(PcMgmtHdr));

    pairing_mgmt_key_seen=TRUE;
    pairing_save_key(event);
    return G_SOURCE_CONTINUE;
}

static gboolean pairing_capture_start(void) {
    pairing_mgmt_fd=pairing_mgmt_open(TRUE);

    if(pairing_mgmt_fd<0) {
        log_event("pairing_key_error",
            "Cannot open Bluetooth management channel for Link Key capture");
        return FALSE;
    }

    pairing_mgmt_watch=g_unix_fd_add(
        pairing_mgmt_fd,
        G_IO_IN|G_IO_HUP|G_IO_ERR,
        pairing_mgmt_event,
        NULL);

    if(!pairing_hci_capture_start()) {
        if(pairing_mgmt_watch) {
            g_source_remove(pairing_mgmt_watch);
            pairing_mgmt_watch=0;
        }

        close(pairing_mgmt_fd);
        pairing_mgmt_fd=-1;
        return FALSE;
    }

    log_event("pairing_key_capture",
        "MGMT and HCI monitor BR/EDR Link Key capture armed");

    return TRUE;
}

static gboolean pairing_read_key(const char *remote,PcMgmtLinkKey *key) {
    g_autofree char *path=pairing_store_path(remote,FALSE);

    if(!path || !g_file_test(path,G_FILE_TEST_IS_REGULAR)) {
        log_event("pairing_key_missing",
            "No persistent pairing key; use Pair / Sync new Switch once");
        return FALSE;
    }

    GKeyFile *config=g_key_file_new();
    GError *error=NULL;

    if(!g_key_file_load_from_file(config,path,G_KEY_FILE_NONE,&error)) {
        log_event("pairing_key_error",error->message);
        g_error_free(error);
        g_key_file_unref(config);
        return FALSE;
    }

    g_autofree char *controller=
        g_key_file_get_string(config,"Pairing","Controller",NULL);
    g_autofree char *local=
        g_key_file_get_string(config,"Pairing","AdapterAddress",NULL);
    g_autofree char *stored_remote=
        g_key_file_get_string(config,"Pairing","SwitchAddress",NULL);
    g_autofree char *encoded=
        g_key_file_get_string(config,"Pairing","LinkKeyBase64",NULL);

    gint type=g_key_file_get_integer(config,"Pairing","KeyType",NULL);
    gint pin_length=g_key_file_get_integer(config,"Pairing","PINLength",NULL);

    gboolean metadata_ok=
        controller && !strcmp(controller,"pro") &&
        local && !g_ascii_strcasecmp(local,pairing_local_address) &&
        stored_remote && !g_ascii_strcasecmp(stored_remote,remote) &&
        encoded &&
        type>=0 && type<=8 &&
        pin_length>=0 && pin_length<=16;

    if(!metadata_ok) {
        log_event("pairing_key_error","Persistent pairing metadata does not match this controller");
        g_key_file_unref(config);
        return FALSE;
    }

    gsize decoded_size=0;
    g_autofree guchar *decoded=g_base64_decode(encoded,&decoded_size);

    if(decoded_size!=16) {
        log_event("pairing_key_error","Persistent Bluetooth Link Key has invalid length");
        g_key_file_unref(config);
        return FALSE;
    }

    memset(key,0,sizeof(*key));
    str2ba(remote,&key->addr.bdaddr);
    key->addr.type=0;
    key->type=(uint8_t)type;
    memcpy(key->val,decoded,16);
    key->pin_len=(uint8_t)pin_length;

    g_key_file_unref(config);
    return TRUE;
}

static gboolean pairing_set_controller_device_id(void) {
    if(controller_type!=CONTROLLER_PRO)
        return TRUE;

    int fd=pairing_mgmt_open(FALSE);
    if(fd<0) {
        log_event("device_id_error",
            "Cannot open Bluetooth management channel for controller identity");
        return FALSE;
    }

    PcMgmtSetDeviceId request={0};
    request.hdr.opcode=htobs(PC_MGMT_OP_SET_DEVICE_ID);
    request.hdr.index=htobs((uint16_t)pairing_mgmt_index);
    request.hdr.len=htobs(sizeof(request)-sizeof(request.hdr));
    request.source=htobs(0x0002);
    request.vendor=htobs(0x057e);
    request.product=htobs(0x2009);
    request.version=htobs(0x0001);

    ssize_t written;
    do {
        written=write(fd,&request,sizeof(request));
    } while(written<0 && errno==EINTR);

    if(written<0) {
        char detail[192];
        snprintf(detail,sizeof(detail),
            "Set Device ID write failed: %s (errno=%d)",
            g_strerror(errno),errno);
        log_event("device_id_error",detail);
        close(fd);
        return FALSE;
    }

    gint64 deadline=g_get_monotonic_time()+2000000;
    gboolean success=FALSE;

    while(g_get_monotonic_time()<deadline) {
        gint64 left=deadline-g_get_monotonic_time();
        struct pollfd pollfd={
            .fd=fd,
            .events=POLLIN
        };

        int poll_result=poll(
            &pollfd,
            1,
            (int)MAX(1,left/1000));

        if(poll_result<0) {
            if(errno==EINTR)
                continue;
            break;
        }

        if(poll_result==0 ||
           (pollfd.revents&(POLLERR|POLLHUP|POLLNVAL)))
            break;

        uint8_t buffer[1024];
        ssize_t size;
        do {
            size=read(fd,buffer,sizeof(buffer));
        } while(size<0 && errno==EINTR);

        if(size<(ssize_t)(sizeof(PcMgmtHdr)+
                          sizeof(PcMgmtCommandResult)))
            continue;

        const PcMgmtHdr *header=(const PcMgmtHdr *)buffer;
        uint16_t event=btohs(header->opcode);
        uint16_t index=btohs(header->index);

        if(index!=(uint16_t)pairing_mgmt_index ||
           (event!=PC_MGMT_EV_CMD_COMPLETE &&
            event!=PC_MGMT_EV_CMD_STATUS))
            continue;

        const PcMgmtCommandResult *result=
            (const PcMgmtCommandResult *)(buffer+sizeof(PcMgmtHdr));

        if(btohs(result->opcode)!=PC_MGMT_OP_SET_DEVICE_ID)
            continue;

        if(result->status==0) {
            success=TRUE;
            log_event("device_id_set",
                "EIR identity source=USB vendor=0x057e product=0x2009 version=0x0001");
        } else {
            char detail[96];
            snprintf(detail,sizeof(detail),
                "Set Device ID failed with MGMT status 0x%02x",
                result->status);
            log_event("device_id_error",detail);
        }

        break;
    }

    close(fd);

    if(!success)
        log_event("device_id_error",
            "Kernel did not accept the Pro Controller Device ID");

    return success;
}

static gboolean pairing_load_into_kernel(const char *remote) {
    PcMgmtLinkKey key;

    if(!pairing_read_key(remote,&key))
        return FALSE;

    int fd=pairing_mgmt_open(FALSE);
    if(fd<0) {
        log_event("pairing_key_error",
            "Cannot open Bluetooth management channel for Link Key restore");
        return FALSE;
    }

    PcMgmtLoadOneKey request={0};
    request.hdr.opcode=htobs(PC_MGMT_OP_LOAD_LINK_KEYS);
    request.hdr.index=htobs((uint16_t)pairing_mgmt_index);
    request.hdr.len=htobs(sizeof(request)-sizeof(request.hdr));
    request.debug_keys=0;
    request.key_count=htobs(1);
    memcpy(&request.key,&key,sizeof(key));

    /*
     * HCI_CHANNEL_CONTROL is not a connected socket. BlueZ's own MGMT
     * implementation writes commands with writev()/write(), not send().
     * send() can fail with ENOTCONN before the kernel ever sees the MGMT
     * packet.
     */
    ssize_t written;

    do {
        written=write(fd,&request,sizeof(request));
    } while(written<0 && errno==EINTR);

    /*
     * HCI_CHANNEL_CONTROL does not have normal stream/datagram write
     * semantics. The kernel dispatches the buffer directly to the MGMT
     * command handler, whose successful return value may be zero.
     *
     * BlueZ itself considers only a negative writev() result an error.
     * Do the same here and wait for MGMT_EV_CMD_COMPLETE/STATUS for the
     * actual command result.
     */
    if(written<0) {
        int error_code=errno;
        char detail[256];

        snprintf(
            detail,
            sizeof(detail),
            "Load Link Keys write failed: %s (errno=%d)",
            g_strerror(error_code),
            error_code);

        log_event("pairing_key_error",detail);
        close(fd);
        return FALSE;
    }

    {
        char detail[192];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s index=%d key_type=%u write_return=%zd; "
            "waiting for MGMT completion",
            remote,
            pairing_mgmt_index,
            key.type,
            written);

        log_event("pairing_key_load_request",detail);
    }

    gint64 deadline=g_get_monotonic_time()+2000000;
    gboolean success=FALSE;

    while(g_get_monotonic_time()<deadline) {
        gint64 left=deadline-g_get_monotonic_time();
        int timeout=(int)MAX(1,left/1000);

        struct pollfd pollfd={
            .fd=fd,
            .events=POLLIN
        };

        int poll_result=poll(&pollfd,1,timeout);
        if(poll_result<0) {
            if(errno==EINTR)continue;
            break;
        }

        if(poll_result==0)
            break;

        if(pollfd.revents&(POLLERR|POLLHUP|POLLNVAL)) {
            char detail[128];

            snprintf(
                detail,
                sizeof(detail),
                "MGMT socket poll error revents=0x%x",
                pollfd.revents);

            log_event("pairing_key_error",detail);
            break;
        }

        uint8_t buffer[1024];
        ssize_t size;

        do {
            size=read(fd,buffer,sizeof(buffer));
        } while(size<0 && errno==EINTR);

        if(size<0) {
            char detail[192];

            snprintf(
                detail,
                sizeof(detail),
                "MGMT read failed: %s (errno=%d)",
                g_strerror(errno),
                errno);

            log_event("pairing_key_error",detail);
            break;
        }

        if(size<(ssize_t)(sizeof(PcMgmtHdr)+sizeof(PcMgmtCommandResult)))
            continue;

        PcMgmtHdr *header=(PcMgmtHdr *)buffer;
        uint16_t event=btohs(header->opcode);
        uint16_t index=btohs(header->index);

        if(index!=(uint16_t)pairing_mgmt_index ||
           (event!=PC_MGMT_EV_CMD_COMPLETE &&
            event!=PC_MGMT_EV_CMD_STATUS))
            continue;

        PcMgmtCommandResult *result=
            (PcMgmtCommandResult *)(buffer+sizeof(PcMgmtHdr));

        if(btohs(result->opcode)!=PC_MGMT_OP_LOAD_LINK_KEYS)
            continue;

        if(result->status==0) {
            success=TRUE;

            char detail[160];
            snprintf(
                detail,
                sizeof(detail),
                "peer=%s key_type=%u kernel_index=%d",
                remote,
                key.type,
                pairing_mgmt_index);

            log_event("pairing_key_loaded",detail);
        } else {
            char detail[96];
            snprintf(detail,sizeof(detail),
                "Load Link Keys failed with MGMT status 0x%02x",
                result->status);
            log_event("pairing_key_error",detail);
        }

        break;
    }

    close(fd);

    if(!success)
        log_event("pairing_key_error",
            "Kernel did not accept persistent Link Key");

    return success;
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
    gboolean allowed=!device || g_str_has_prefix(device,prefix) || (allowed_adapter[0] && g_str_has_prefix(device,allowed_adapter)); g_free(prefix);
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
    log_event("bluez_properties",msg);g_free(msg);g_free(v);
}

static gboolean pairing_handoff_reconnect(gpointer unused);

static void reset_link(void) {
    for(int i=0;i<2;i++) {
        if(watches[i]) {g_source_remove(watches[i]);watches[i]=0;}
        if(channels[i]>=0) close(channels[i]);
        channels[i]=-1;
    }
    gboolean should_handoff=
        !shutting_down &&
        !reconnect_mode &&
        last_peer[0];

    peer[0]=0;
    initialized=FALSE;
    next_report_at=0;
    slow_exit_until=0;
    slow_input_frequency=FALSE;
    saw_interrupt_output=FALSE;

    uint8_t addr[6];
    memcpy(addr,state.address,6);
    controller_init(&state,controller_type,addr);
    apply_controller_colors();

    log_event("link_closed",reconnect_mode
        ?"Paired Switch disconnected; start Reconnect again to initiate a new connection"
        :"Initial pairing link closed");

    /*
     * Nintendo completes the temporary Change Grip/Order link and can then
     * drop it. A real controller immediately reconnects using the Link Key it
     * just stored. Do the same transparently.
     */
    if(should_handoff && !pairing_reconnect_source) {
        pairing_handoff_attempts=0;

        log_event(
            "pairing_handoff",
            "Pairing completed; scheduling persistent reconnect");

        pairing_reconnect_source=
            g_timeout_add(
                500,
                pairing_handoff_reconnect,
                NULL);
    }
}
static uint8_t timer_byte(void) {return (uint8_t)(g_get_monotonic_time()/5000);}
static gboolean send_report(const uint8_t out[50],size_t length) {
    ssize_t n=send(channels[1],out,length,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==(ssize_t)length) {sent++; return TRUE;}
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return TRUE;
    log_event("hid_send_error",strerror(errno));return FALSE;
}
static gboolean receive_packet(gint fd,GIOCondition cond,gpointer user) {
    int index=GPOINTER_TO_INT(user);
    if(cond&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)) {watches[index]=0;reset_link();return G_SOURCE_REMOVE;}
    uint8_t in[1024],out[50];ssize_t n=recv(fd,in,sizeof(in),MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return G_SOURCE_CONTINUE;
    if(n<=0) {watches[index]=0;reset_link();return G_SOURCE_REMOVE;}
    if(index && !saw_interrupt_output) {
        saw_interrupt_output=TRUE;
        log_event(reconnect_mode?"reconnect_rx":"pairing_rx",
            reconnect_mode
                ?"First Switch interrupt report received during reconnect"
                :"First Switch interrupt report received; pairing cadence is now 15 Hz");
    }
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
        if(!send_report(out,50)) {watches[index]=0;reset_link();return G_SOURCE_REMOVE;}
        char detail[96];snprintf(detail,sizeof(detail),"subcommand=0x%02x mode=0x%02x lights=0x%02x",in[11],state.mode,state.lights);
        log_event("hid_reply",detail);
        if(state.lights && state.vibration && !initialized) {
            initialized=TRUE;
            if(reconnect_mode) {
                slow_input_frequency=FALSE;
                next_report_at=0;
                log_event("reconnect_initialized",
                    "Player lights and vibration restored; switching to normal report cadence");
            } else {
                log_event("initialization_observed",
                    "Player lights and vibration configured; keeping slow Change Grip/Order cadence until A, B or HOME exits the menu");
            }
        }
    } else if(n>=2 && in[0]==0xa2 && in[1]==0x01) log_event("unsupported_subcommand","No invented ACK sent");
    return G_SOURCE_CONTINUE;
}
/*
 * Switch 2 reconnect timing is stricter than the normal Linux L2CAP
 * security path.
 *
 * A non-blocking L2CAP connect first creates the underlying ACL. As soon as
 * that ACL has a handle, explicitly authenticate and encrypt it with the
 * persisted BR/EDR Link Key before allowing the HID PSM negotiation to
 * finish.
 *
 * This uses the same public BlueZ HCI helpers as hcitool auth/enc.
 */
static gboolean reconnect_use_immediate_auth(void) {
    int hci_fd=hci_open_dev(pairing_mgmt_index);

    if(hci_fd<0) {
        log_event(
            "reconnect_adapter_probe_error",
            strerror(errno));
        return FALSE;
    }

    struct hci_version version={0};

    if(hci_read_local_version(hci_fd,&version,1000)<0) {
        log_event(
            "reconnect_adapter_probe_error",
            strerror(errno));
        hci_close_dev(hci_fd);
        return FALSE;
    }

    hci_close_dev(hci_fd);

    gboolean realtek=version.manufacturer==0x005d;

    char detail[192];
    snprintf(
        detail,
        sizeof(detail),
        "manufacturer=0x%04x hci=%u lmp=%u subver=0x%04x immediate_auth=%s",
        version.manufacturer,
        version.hci_ver,
        version.lmp_ver,
        version.lmp_subver,
        realtek?"true":"false");

    log_event("reconnect_adapter_profile",detail);

    return realtek;
}

/*
 * Realtek reconnect experiment.
 *
 * The Switch accepts the outbound ACL from the RTL8821CU but terminates it
 * with reason 0x13 before the normal Linux security path reaches
 * Authentication Requested. Wait only until Connection Complete has replaced
 * Linux's temporary 0x0fxx handle, then request authentication immediately.
 *
 * This path is enabled only for HCI manufacturer 0x005d (Realtek). Other
 * adapters, including the validated CSR controller, keep the normal kernel
 * L2CAP security path unchanged.
 */
static gboolean secure_reconnect_acl(const bdaddr_t *remote,
                                     const char *address) {
    int hci_fd=hci_open_dev(pairing_mgmt_index);

    if(hci_fd<0) {
        log_event("reconnect_hci_error",strerror(errno));
        return FALSE;
    }

    struct hci_conn_info_req *request=
        g_malloc0(sizeof(*request)+sizeof(struct hci_conn_info));

    bacpy(&request->bdaddr,remote);
    request->type=ACL_LINK;

    gint64 started=g_get_monotonic_time();
    gint64 deadline=started+500000;
    gboolean found=FALSE;
    gboolean saw_pending=FALSE;

    while(g_get_monotonic_time()<deadline) {
        if(ioctl(
                hci_fd,
                HCIGETCONNINFO,
                (unsigned long)request)==0) {
            uint16_t candidate=request->conn_info->handle;

            /*
             * Linux exposes an internal temporary handle in the 0x0fxx
             * range before the physical BR/EDR Connection Complete.
             */
            if(candidate<=0x0eff) {
                found=TRUE;
                break;
            }

            if(!saw_pending) {
                char detail[192];
                snprintf(
                    detail,
                    sizeof(detail),
                    "peer=%s temporary_handle=0x%04x",
                    address,
                    candidate);

                log_event("reconnect_acl_pending",detail);
                saw_pending=TRUE;
            }

            g_usleep(500);
            continue;
        }

        if(errno!=ENOENT &&
           errno!=ENOTCONN &&
           errno!=EAGAIN &&
           errno!=EINVAL) {
            char detail[192];
            snprintf(
                detail,
                sizeof(detail),
                "peer=%s HCIGETCONNINFO failed: %s",
                address,
                strerror(errno));

            log_event("reconnect_acl_error",detail);

            g_free(request);
            hci_close_dev(hci_fd);
            return FALSE;
        }

        g_usleep(500);
    }

    if(!found) {
        char detail[192];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s real ACL handle not visible within %.1f ms",
            address,
            (g_get_monotonic_time()-started)/1000.0);

        log_event("reconnect_acl_timeout",detail);

        g_free(request);
        hci_close_dev(hci_fd);
        return FALSE;
    }

    uint16_t raw_handle=request->conn_info->handle;
    uint16_t handle=htobs(raw_handle);

    {
        char detail[224];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s handle=0x%04x after=%.1fms; sending Authentication Requested immediately",
            address,
            raw_handle,
            (g_get_monotonic_time()-started)/1000.0);

        log_event("reconnect_acl_found",detail);
    }

    if(hci_authenticate_link(hci_fd,handle,1000)<0) {
        char detail[224];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s handle=0x%04x authentication failed: %s",
            address,
            raw_handle,
            strerror(errno));

        log_event("reconnect_auth_failed",detail);

        g_free(request);
        hci_close_dev(hci_fd);
        return FALSE;
    }

    log_event(
        "reconnect_authenticated",
        "Realtek ACL authenticated with persisted BR/EDR Link Key");

    if(hci_encrypt_link(hci_fd,handle,1,1000)<0) {
        char detail[224];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s handle=0x%04x encryption failed: %s",
            address,
            raw_handle,
            strerror(errno));

        log_event("reconnect_encrypt_failed",detail);

        g_free(request);
        hci_close_dev(hci_fd);
        return FALSE;
    }

    log_event(
        "reconnect_encrypted",
        "Realtek ACL encrypted before HID L2CAP completes");

    g_free(request);
    hci_close_dev(hci_fd);
    return TRUE;
}

static gboolean finish_nonblocking_connect(int fd,
                                           const char *address,
                                           int psm) {
    struct pollfd pfd={
        .fd=fd,
        .events=POLLOUT|POLLERR|POLLHUP
    };

    int rc;

    do {
        rc=poll(&pfd,1,2000);
    } while(rc<0 && errno==EINTR);

    if(rc<=0) {
        char detail[192];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s psm=%d completion=%s",
            address,
            psm,
            rc==0?"timeout":strerror(errno));

        log_event("reconnect_failed",detail);
        return FALSE;
    }

    int error=0;
    socklen_t error_len=sizeof(error);

    if(getsockopt(
            fd,
            SOL_SOCKET,
            SO_ERROR,
            &error,
            &error_len)<0) {
        log_event("reconnect_failed",strerror(errno));
        return FALSE;
    }

    if(error) {
        char detail[192];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s psm=%d error=%s",
            address,
            psm,
            strerror(error));

        log_event("reconnect_failed",detail);
        return FALSE;
    }

    return TRUE;
}

static gboolean connect_outbound(const bdaddr_t *local,
                                    const char *address) {
    bdaddr_t remote;

    if(str2ba(address,&remote)<0) {
        log_event(
            "reconnect_error",
            "Invalid Switch Bluetooth address");
        return FALSE;
    }

    const gboolean immediate_auth=
        reconnect_use_immediate_auth();

    for(int i=0;i<2;i++) {
        const int psm=i?19:17;
        const gboolean immediate_first=
            immediate_auth && i==0;

        int fd=socket(
            AF_BLUETOOTH,
            SOCK_SEQPACKET|SOCK_CLOEXEC,
            BTPROTO_L2CAP);

        if(fd<0) {
            log_event(
                "reconnect_socket_error",
                strerror(errno));
            goto failed;
        }

        /*
         * A bonded Pro Controller asks for authenticated/encrypted security
         * only on a reconnect that it initiates. Let the Linux Bluetooth
         * stack sequence remote-feature discovery, authentication and
         * encryption in the normal order instead of issuing raw HCI auth
         * commands while the ACL is still being configured.
         */
        struct bt_security security={0};
        security.level=BT_SECURITY_MEDIUM;

        if(setsockopt(
                fd,
                SOL_BLUETOOTH,
                BT_SECURITY,
                &security,
                sizeof(security))<0) {
            char failure[160];

            snprintf(
                failure,
                sizeof(failure),
                "psm=%d level=BT_SECURITY_MEDIUM error=%s",
                psm,
                strerror(errno));

            log_event(
                "reconnect_security_error",
                failure);

            close(fd);
            goto failed;
        }

        struct sockaddr_l2 source={0};
        source.l2_family=AF_BLUETOOTH;
        bacpy(&source.l2_bdaddr,local);

        if(bind(
                fd,
                (struct sockaddr *)&source,
                sizeof(source))<0) {
            log_event(
                "reconnect_bind_error",
                strerror(errno));

            close(fd);
            goto failed;
        }

        struct sockaddr_l2 destination={0};
        destination.l2_family=AF_BLUETOOTH;
        destination.l2_psm=htobs(psm);
        bacpy(&destination.l2_bdaddr,&remote);

        char detail[128];
        snprintf(
            detail,
            sizeof(detail),
            "peer=%s psm=%d",
            address,
            psm);

        log_event("reconnect_attempt",detail);

        if(immediate_first) {
            int flags=fcntl(fd,F_GETFL,0);

            if(flags<0 ||
               fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) {
                log_event(
                    "reconnect_socket_error",
                    strerror(errno));

                close(fd);
                goto failed;
            }

            log_event(
                "reconnect_realtek_immediate_auth",
                "Starting PSM 17 asynchronously so authentication can be requested immediately after ACL Connection Complete");
        }

        int connect_result=
            connect(
                fd,
                (struct sockaddr *)&destination,
                sizeof(destination));

        if(connect_result<0 &&
           (!immediate_first ||
            (errno!=EINPROGRESS && errno!=EAGAIN))) {
            char failure[160];

            snprintf(
                failure,
                sizeof(failure),
                "peer=%s psm=%d error=%s",
                address,
                psm,
                strerror(errno));

            log_event("reconnect_failed",failure);

            close(fd);
            goto failed;
        }

        if(immediate_first) {
            if(!secure_reconnect_acl(&remote,address)) {
                close(fd);
                goto failed;
            }

            if(connect_result<0 &&
               !finish_nonblocking_connect(
                    fd,
                    address,
                    psm)) {
                close(fd);
                goto failed;
            }
        }

        int flags=fcntl(fd,F_GETFL,0);
        if(flags>=0)
            fcntl(fd,F_SETFL,flags|O_NONBLOCK);

        channels[i]=fd;

        watches[i]=g_unix_fd_add(
            fd,
            G_IO_IN|G_IO_HUP|G_IO_ERR,
            receive_packet,
            GINT_TO_POINTER(i));

        snprintf(
            detail,
            sizeof(detail),
            "peer=%s psm=%d",
            address,
            psm);

        log_event("l2cap_connected",detail);
    }

    g_strlcpy(peer,address,sizeof(peer));
    g_strlcpy(last_peer,address,sizeof(last_peer));

    slow_input_frequency=TRUE;
    saw_interrupt_output=FALSE;
    next_report_at=0;
    slow_exit_until=0;

    uint8_t out[50];
    size_t length=pro_stream_input(&state,timer_byte(),out);

    if(!send_report(out,length))
        goto failed;

    log_event(
        "reconnect_connected",
        "Authenticated/encrypted kernel L2CAP reconnect established; waiting for Switch HID initialization");

    return TRUE;

failed:
    for(int i=0;i<2;i++) {
        if(watches[i]) {
            g_source_remove(watches[i]);
            watches[i]=0;
        }

        if(channels[i]>=0) {
            close(channels[i]);
            channels[i]=-1;
        }
    }

    peer[0]=0;
    return FALSE;
}

static gboolean pairing_handoff_reconnect(gpointer unused) {
    (void)unused;

    pairing_reconnect_source=0;

    if(shutting_down ||
       channels[0]>=0 ||
       channels[1]>=0 ||
       !last_peer[0])
        return G_SOURCE_REMOVE;

    /*
     * The key must already have been persisted by HCI monitor several
     * seconds before the Switch closes the temporary pairing ACL.
     */
    if(!pairing_load_into_kernel(last_peer)) {
        log_event(
            "pairing_handoff_error",
            "Persistent Link Key was not available after pairing");
        return G_SOURCE_REMOVE;
    }

    set_property(
        "Pairable",
        g_variant_new_boolean(FALSE));

    set_property(
        "Discoverable",
        g_variant_new_boolean(FALSE));

    reconnect_mode=TRUE;
    g_strlcpy(
        reconnect_peer,
        last_peer,
        sizeof(reconnect_peer));

    pairing_handoff_attempts++;

    char detail[128];
    snprintf(
        detail,
        sizeof(detail),
        "peer=%s attempt=%u",
        last_peer,
        pairing_handoff_attempts);

    log_event("pairing_handoff_reconnect",detail);

    if(connect_outbound(
            &selected_local_address,
            last_peer)) {
        pairing_handoff_attempts=0;

        log_event(
            "pairing_handoff_connected",
            "One-time pairing transitioned to persistent controller connection");

        return G_SOURCE_REMOVE;
    }

    reconnect_mode=FALSE;

    /*
     * Give the Switch a little time to finish leaving Change Grip/Order.
     * Retry a few times automatically instead of exposing this transition
     * to the user.
     */
    if(pairing_handoff_attempts<5 && !shutting_down) {
        pairing_reconnect_source=
            g_timeout_add(
                1000,
                pairing_handoff_reconnect,
                NULL);

        log_event(
            "pairing_handoff_retry",
            "Switch not ready yet; retrying persistent reconnect");
    } else {
        /*
         * Switch 2 advertises No Bonding during Change Grip/Order and rejects
         * the locally retained key. Keep the already-open listeners usable and
         * return to inbound SSP instead of leaving the adapter hidden. The
         * console can then establish a fresh temporary session without asking
         * the user to restart Controller Studio.
         */
        gboolean inbound_ready=TRUE;

        if(!set_property(
                "PairableTimeout",
                g_variant_new_uint32(180)))
            inbound_ready=FALSE;

        if(!set_property(
                "DiscoverableTimeout",
                g_variant_new_uint32(180)))
            inbound_ready=FALSE;

        if(!set_property(
                "Pairable",
                g_variant_new_boolean(TRUE)))
            inbound_ready=FALSE;

        if(!set_property(
                "Discoverable",
                g_variant_new_boolean(TRUE)))
            inbound_ready=FALSE;

        log_event(
            inbound_ready
                ?"pairing_handoff_fallback"
                :"pairing_handoff_error",
            inbound_ready
                ?"Stored key rejected; waiting for a fresh inbound Switch pairing"
                :"Stored key rejected and inbound pairing could not be restored");
    }

    return G_SOURCE_REMOVE;
}

static gboolean reconnect_from_control(const char *address,
                                       gpointer user_data,
                                       GError **error) {
    (void)user_data;

    if(controller_type!=CONTROLLER_PRO) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,
                            "Reconnect is currently available for Pro Controller only");
        return FALSE;
    }

    if(!have_selected_local_address) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,
                            "Bluetooth adapter address is unavailable");
        return FALSE;
    }

    if(channels[0]>=0 || channels[1]>=0) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_BUSY,
                            "Controller is already connected");
        return FALSE;
    }

    reconnect_mode=TRUE;
    g_strlcpy(reconnect_peer,address,sizeof(reconnect_peer));

    log_event("reconnect_in_session",
        "Reusing the active pairing backend and BlueZ controller state");

    if(!connect_outbound(&selected_local_address,address)) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,
                            "Switch rejected the outbound HID connection");
        return FALSE;
    }

    return TRUE;
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
    g_strlcpy(peer,address,sizeof(peer));
    g_strlcpy(last_peer,address,sizeof(last_peer));
    channels[index]=client;
    watches[index]=g_unix_fd_add(client,G_IO_IN|G_IO_HUP|G_IO_ERR,receive_packet,GINT_TO_POINTER(index));
    char msg[100];snprintf(msg,sizeof(msg),"peer=%s psm=%d",address,index?19:17);log_event("l2cap_connected",msg);
    if(channels[0]>=0 && channels[1]>=0) {
        slow_input_frequency=TRUE;
        saw_interrupt_output=FALSE;
        next_report_at=0;
        slow_exit_until=0;

        log_event(
            "pairing_rate",
            "Using reduced report cadence during Change Grip/Order");

        /*
         * BT_SECURITY_MEDIUM already caused the kernel/console SSP exchange.
         * The resulting HCI Link Key Notification is captured independently
         * through HCI_CHANNEL_MONITOR.
         */
    }
    return G_SOURCE_CONTINUE;
}
static gboolean tick(gpointer unused) {
    (void)unused;
    if(pro_control_expired(desktop)) {
        log_event("desktop_timeout","No UI request for five seconds; stopping session");
        g_main_loop_quit(loop);return G_SOURCE_REMOVE;
    }

    gint64 now=g_get_monotonic_time();

    if(release_at && now>=release_at) {
        memset(state.buttons,0,3);
        state.sticks[0]=0x86f;state.sticks[1]=0x77c;
        state.sticks[2]=0x816;state.sticks[3]=0x7dd;
        release_at=0;
        log_event("input_released","neutral");
    }

    if(channels[1]>=0 && channels[0]>=0) {
        /*
         * Historical NXBT behaviour: the Switch processes controller traffic
         * much more slowly in Change Grip/Order. Stay in the reduced-rate
         * phase until A, B or HOME is used to leave that menu.
         *
         * Right button byte: B=0x04, A=0x08
         * Shared button byte: HOME=0x10
         */
        gboolean grip_exit_pressed =
            (state.buttons[0] & 0x0c) ||
            (state.buttons[1] & 0x10);

        if(slow_input_frequency && initialized &&
           grip_exit_pressed && !slow_exit_until) {
            slow_exit_until=now+1000000;
            log_event("grip_exit_input",
                "A/B/HOME observed; keeping 15 Hz for one second while Change Grip/Order exits");
        }

        if(slow_input_frequency && slow_exit_until &&
           now>=slow_exit_until) {
            slow_input_frequency=FALSE;
            slow_exit_until=0;
            next_report_at=0;
            log_event("input_rate",
                "Change Grip/Order transition complete; switching to normal report cadence");
        }

        if(!next_report_at || now>=next_report_at) {
            uint8_t out[50];
            size_t length=pro_stream_input(&state,timer_byte(),out);

            if(!send_report(out,length)) {
                reset_link();
            } else {
                gint64 interval;

                if(slow_input_frequency)
                    interval=saw_interrupt_output ? 66667 :
                        controller_type==CONTROLLER_PRO ? 100000 : 1000000;
                else
                    interval=15000;

                next_report_at=now+interval;
            }
        }
    }

    pro_control_update(desktop,mock_mode?"Simulation":peer,
                       mock_mode || initialized,sent,received);
    return G_SOURCE_CONTINUE;
}
static void status_event(void) {
    const char *phase=!peer[0]?(reconnect_mode?"reconnect":"waiting"):
        !saw_interrupt_output?(reconnect_mode?"reconnect-handshake":"pairing-10hz"):
        slow_input_frequency?(reconnect_mode?"reconnect-15hz":"grip-order-15hz"):"normal";
    char msg[180];
    snprintf(msg,sizeof(msg),
        "peer=%s tx=%u rx=%u initialized=%s phase=%s",
        peer[0]?peer:"none",sent,received,
        initialized?"true":"false",phase);
    log_event("status",msg);
}
static gboolean stats(gpointer unused) {
    (void)unused;if(verbose_traffic)status_event();return G_SOURCE_CONTINUE;
}
static gboolean quit(gpointer unused) {(void)unused;g_main_loop_quit(loop);return G_SOURCE_CONTINUE;}
static gboolean input(GIOChannel *io,GIOCondition cond,gpointer unused) {
    (void)unused;if(cond&G_IO_HUP) return G_SOURCE_REMOVE;
    char *line=NULL;gsize len;
    if(g_io_channel_read_line(io,&line,&len,NULL,NULL)!=G_IO_STATUS_NORMAL) return G_SOURCE_CONTINUE;
    g_strstrip(line);
    if(!strcmp(line,"quit")) quit(NULL);
    else if(!strcmp(line,"release")) release_at=1;
    else if(!strcmp(line,"status"))status_event();
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
    mock_mode=argc>=2 && !strcmp(argv[1],"--mock");
    if(mock_mode) {
        if(argc>3 || (argc==3 && !select_controller(argv[2]))) {fprintf(stderr,"Usage: %s --mock [pro|joycon-l|joycon-r]\n",argv[0]);return 2;}
        uint8_t addr[6]={0};controller_init(&state,controller_type,addr);
        loop=g_main_loop_new(NULL,FALSE);GError *error=NULL;
        desktop=pro_control_new(&state,&release_at,&verbose_traffic,getuid(),TRUE,control_socket,loop,&error);
        if(!desktop) {g_printerr("%s\n",error->message);g_error_free(error);return 1;}
        g_unix_signal_add(SIGINT,quit,NULL);g_unix_signal_add(SIGTERM,quit,NULL);
        g_timeout_add(15,tick,NULL);log_event("mock_ready","No Bluetooth activity");
        g_main_loop_run(loop);pro_control_free(desktop);g_main_loop_unref(loop);return 0;
    }
    gboolean desktop_mode=FALSE;uid_t desktop_owner=0;
    for(int i=3;i<argc;i++) {
        if(!strcmp(argv[i],"--desktop") && i+1<argc) {
            char *end=NULL;guint64 owner=g_ascii_strtoull(argv[++i],&end,10);
            if(!end || *end || owner>G_MAXUINT32) {fprintf(stderr,"Invalid desktop owner\n");return 2;}
            desktop_mode=TRUE;desktop_owner=(uid_t)owner;
        } else if(!strcmp(argv[i],"--type") && i+1<argc) {
            if(!select_controller(argv[++i])) {fprintf(stderr,"Unknown controller type\n");return 2;}
        } else if(!strcmp(argv[i],"--allow-adapter") && i+1<argc && g_regex_match_simple("^hci[0-9]+$",argv[i+1],0,0)) {
            snprintf(allowed_adapter,sizeof(allowed_adapter),"/org/bluez/%s/dev_",argv[++i]);
        } else if(!strcmp(argv[i],"--shared-profile"))shared_profile=TRUE;
        else if(!strcmp(argv[i],"--verbose"))verbose_traffic=TRUE;
        else if(!strcmp(argv[i],"--body-color") && i+1<argc) {
            if(!parse_rgb_color(argv[++i],configured_body_color)) {
                fprintf(stderr,"Invalid body color; expected RRGGBB\n");
                return 2;
            }
        } else if(!strcmp(argv[i],"--button-color") && i+1<argc) {
            if(!parse_rgb_color(argv[++i],configured_button_color)) {
                fprintf(stderr,"Invalid button color; expected RRGGBB\n");
                return 2;
            }
        } else if(!strcmp(argv[i],"--left-grip-color") && i+1<argc) {
            if(!parse_rgb_color(argv[++i],configured_left_grip_color)) {
                fprintf(stderr,"Invalid left grip color; expected RRGGBB\n");
                return 2;
            }
        } else if(!strcmp(argv[i],"--right-grip-color") && i+1<argc) {
            if(!parse_rgb_color(argv[++i],configured_right_grip_color)) {
                fprintf(stderr,"Invalid right grip color; expected RRGGBB\n");
                return 2;
            }
        }
        else if(!strcmp(argv[i],"--reconnect") && i+1<argc &&
                g_regex_match_simple("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$",argv[i+1],0,0)) {
            g_strlcpy(reconnect_peer,argv[++i],sizeof(reconnect_peer));
            reconnect_mode=TRUE;
        }
        else {fprintf(stderr,"Unknown or incomplete option: %s\n",argv[i]);return 2;}
    }
    desktop_requested=desktop_mode;
    if(argc<3 || !g_regex_match_simple("^hci[0-9]+$",argv[1],0,0) || (desktop_mode && getuid()!=0)) {
        fprintf(stderr,"Usage: %s hciN SDP_XML [--desktop UID] [--type pro|joycon-l|joycon-r] [--allow-adapter hciN] [--shared-profile] [--verbose] [--body-color RRGGBB] [--button-color RRGGBB] [--left-grip-color RRGGBB] [--right-grip-color RRGGBB] [--reconnect MAC]\n",argv[0]);return 2;
    }
    int result=1,dd=-1;uint8_t old_class[3]={0};gboolean have_class=FALSE;
    GVariant *saved[6]={0};const char *keys[]={"Powered","Alias","Pairable","Discoverable","PairableTimeout","DiscoverableTimeout"};
    guint agent_id=0,profile_id=0;gboolean registered_agent=FALSE,registered_profile=FALSE;
    snprintf(adapter,sizeof(adapter),"/org/bluez/%s",argv[1]);
    GError *err=NULL;bus=g_bus_get_sync(G_BUS_TYPE_SYSTEM,NULL,&err);
    if(!bus) {fprintf(stderr,"%s\n",err->message);g_error_free(err);return 1;}
    GDBusNodeInfo *node=g_dbus_node_info_new_for_xml(xml,NULL);
    GVariant *v=property("Address");if(!v) goto cleanup;
    const char *address=g_variant_get_string(v,NULL);bdaddr_t local;
    str2ba(address,&local);
    bacpy(&selected_local_address,&local);
    have_selected_local_address=TRUE;

    pairing_owner=desktop_mode?desktop_owner:0;

    /*
     * argv[1] is already validated as hciN above. Do not use hci_devid()
     * here: hci_devid("hciN") internally calls hci_devba(), which fails
     * while the controller is still DOWN. That previously left the MGMT
     * controller index at -1 (0xffff / MGMT_INDEX_NONE).
     */
    char *index_end=NULL;
    guint64 parsed_index=
        g_ascii_strtoull(argv[1]+3,&index_end,10);

    if(!index_end || *index_end || parsed_index>G_MAXUINT16) {
        log_event("adapter_index_error",
            "Invalid HCI controller index");
        goto cleanup;
    }

    pairing_mgmt_index=(int)parsed_index;

    char index_detail[96];
    snprintf(index_detail,sizeof(index_detail),
        "adapter=%s mgmt_index=%d",
        argv[1],pairing_mgmt_index);
    log_event("adapter_index",index_detail);

    g_strlcpy(pairing_local_address,address,sizeof(pairing_local_address));

    uint8_t mac[6];for(int i=0;i<6;i++) mac[i]=local.b[5-i];
    controller_init(&state,controller_type,mac);
    apply_controller_colors();

    if(controller_type==CONTROLLER_PRO) {
        char colors[192];
        snprintf(
            colors,sizeof(colors),
            "body=%02X%02X%02X buttons=%02X%02X%02X left_grip=%02X%02X%02X right_grip=%02X%02X%02X",
            configured_body_color[0],configured_body_color[1],configured_body_color[2],
            configured_button_color[0],configured_button_color[1],configured_button_color[2],
            configured_left_grip_color[0],configured_left_grip_color[1],configured_left_grip_color[2],
            configured_right_grip_color[0],configured_right_grip_color[1],configured_right_grip_color[2]);
        log_event("controller_colors",colors);
    }

    log_event("adapter_selected",address);g_variant_unref(v);
    for(int i=0;i<6;i++) {saved[i]=property(keys[i]);if(!saved[i]) goto cleanup;}
    if(!g_variant_get_boolean(saved[0])) {
        if(!set_property("Powered",g_variant_new_boolean(TRUE)))goto cleanup;
        log_event("adapter_powered","Powered on temporarily for the Classic HID session");
    }

    /*
     * BlueZ normally advertises the Linux host Device ID in EIR. A Switch Pro
     * Controller advertises Nintendo's USB VID/PID instead. Set this volatile
     * kernel identity before inquiry becomes visible; restarting normal BlueZ
     * in the runner restores the host identity after the session.
     */
    if(!pairing_set_controller_device_id())
        goto cleanup;

    dd=hci_open_dev(pairing_mgmt_index);
    if(dd<0 || hci_read_class_of_dev(dd,old_class,2000)<0) {
        log_event("hci_error",strerror(errno));
        goto cleanup;
    }
    have_class=TRUE;

    if(!reconnect_mode && controller_type==CONTROLLER_PRO &&
       !pairing_capture_start())
        goto cleanup;

    static const GDBusInterfaceVTable av={.method_call=agent},pv={.method_call=profile};

    if(!reconnect_mode) {
        agent_id=g_dbus_connection_register_object(bus,ROOT "/agent",node->interfaces[0],&av,NULL,NULL,&err);
        profile_id=g_dbus_connection_register_object(bus,ROOT "/profile",node->interfaces[1],&pv,NULL,NULL,&err);
        if(!agent_id || !profile_id) goto cleanup;

        if(!invoke("/org/bluez","org.bluez.AgentManager1","RegisterAgent",
                   g_variant_new("(os)",ROOT "/agent","NoInputNoOutput")))
            goto cleanup;
        registered_agent=TRUE;

        if(!invoke("/org/bluez","org.bluez.AgentManager1","RequestDefaultAgent",
                   g_variant_new("(o)",ROOT "/agent")))
            goto cleanup;

        log_event("agent_registered",
            "NoInputNoOutput; Just Works SSP like a real Pro Controller");

        char *record=NULL;
        if(!g_file_get_contents(argv[2],&record,NULL,&err))
            goto cleanup;

        GVariantBuilder opts;
        g_variant_builder_init(&opts,G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&opts,"{sv}","ServiceRecord",
                              g_variant_new_string(record));
        g_free(record);

        g_variant_builder_add(&opts,"{sv}","Role",
                              g_variant_new_string("server"));
        /*
         * A real Switch Pro Controller does not require authentication at
         * the HID service itself during a fresh Change Grip/Order pairing.
         * The Switch drives SSP/authentication/encryption. Requiring it here
         * changes the security negotiation fingerprint.
         */
        g_variant_builder_add(&opts,"{sv}","RequireAuthentication",
                              g_variant_new_boolean(FALSE));
        g_variant_builder_add(&opts,"{sv}","RequireAuthorization",
                              g_variant_new_boolean(FALSE));
        g_variant_builder_add(&opts,"{sv}","AutoConnect",
                              g_variant_new_boolean(TRUE));

        if(!shared_profile) {
            if(!invoke("/org/bluez","org.bluez.ProfileManager1","RegisterProfile",
                       g_variant_new("(osa{sv})",
                           ROOT "/profile",
                           "00001000-0000-1000-8000-00805f9b34fb",
                           &opts)))
                goto cleanup;

            registered_profile=TRUE;
            log_event("sdp_registered",
                "Nintendo Switch controller HID record");
        } else {
            log_event("sdp_shared",
                "Using the companion controller's HID profile");
        }
    } else {
        log_event("reconnect_persistent",
            "Restoring persisted controller identity and Bluetooth Link Key");
    }
    if(reconnect_mode) {
        /*
         * Durable reconnect path. Rehydrate the BR/EDR Link Key captured
         * during the one-time Pair / Sync operation before opening HID.
         */
        if(controller_type!=CONTROLLER_PRO) {
            log_event("pairing_key_error",
                "Persistent reconnect is currently implemented for Pro Controller only");
            goto cleanup;
        }

        if(!pairing_load_into_kernel(reconnect_peer))
            goto cleanup;

        if(!set_property("Alias",g_variant_new_string(controller_alias)) ||
           !set_property("Pairable",g_variant_new_boolean(FALSE)) ||
           !set_property("Discoverable",g_variant_new_boolean(FALSE)))
            goto cleanup;

        if(hci_write_class_of_dev(dd,0x002508,2000)<0) {
            log_event("class_error",strerror(errno));
            goto cleanup;
        }

        char ready[180];
        snprintf(ready,sizeof(ready),
            "Persistent reconnect to %s; Link Key loaded, initiating PSM 17/19",
            reconnect_peer);
        log_event("ready",ready);

        if(!connect_outbound(&local,reconnect_peer))
            goto cleanup;
    } else {
        /*
         * First pairing / new console: the Switch initiates the HID
         * connection while Change Grip/Order is open.
         */
        for(int i=0;i<2;i++) {
            listeners[i]=socket(
                AF_BLUETOOTH,
                SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,
                BTPROTO_L2CAP);

            if(listeners[i]<0) {
                log_event("l2cap_listen_error",strerror(errno));
                goto cleanup;
            }

            /*
             * Fresh pairing intentionally mirrors a real Pro Controller:
             * the HID PSMs themselves impose no security requirement.
             *
             * Switch consoles perform SSP/authentication/encryption from
             * their side. Reconnect is different: when we initiate it with
             * a stored bond, connect_outbound() requests MEDIUM security.
             */
            struct bt_security security={0};
            security.level=BT_SECURITY_LOW;

            if(setsockopt(
                    listeners[i],
                    SOL_BLUETOOTH,
                    BT_SECURITY,
                    &security,
                    sizeof(security))<0) {
                char detail[160];

                snprintf(
                    detail,
                    sizeof(detail),
                    "psm=%d level=BT_SECURITY_LOW error=%s",
                    i?19:17,
                    strerror(errno));

                log_event("pairing_security_error",detail);
                goto cleanup;
            }

            char security_detail[128];
            snprintf(
                security_detail,
                sizeof(security_detail),
                "psm=%d level=BT_SECURITY_LOW host-driven-ssp=true",
                i?19:17);

            log_event("pairing_security",security_detail);

            struct sockaddr_l2 bind_addr={
                .l2_family=AF_BLUETOOTH,
                .l2_psm=htobs(i?19:17)
            };
            bacpy(&bind_addr.l2_bdaddr,&local);

            if(bind(
                    listeners[i],
                    (struct sockaddr *)&bind_addr,
                    sizeof(bind_addr))<0 ||
               listen(listeners[i],1)<0) {
                log_event("l2cap_listen_error",strerror(errno));
                goto cleanup;
            }

            g_unix_fd_add(
                listeners[i],
                G_IO_IN,
                accept_peer,
                GINT_TO_POINTER(i));
        }

        if(!set_property("Alias",g_variant_new_string(controller_alias)) ||
           !set_property("PairableTimeout",g_variant_new_uint32(180)) ||
           !set_property("DiscoverableTimeout",g_variant_new_uint32(180)) ||
           !set_property("Pairable",g_variant_new_boolean(TRUE)) ||
           !set_property("Discoverable",g_variant_new_boolean(TRUE))) goto cleanup;

        if(hci_write_class_of_dev(dd,0x002508,2000)<0) {
            log_event("class_error",strerror(errno));goto cleanup;
        }

        char ready[180];
        snprintf(ready,sizeof(ready),
            "Name=%s class=0x002508 discoverable=true pairable=true PSM=17/19; open Change Grip/Order",
            controller_alias);
        log_event("ready",ready);
    }
    g_dbus_connection_signal_subscribe(bus,"org.bluez","org.freedesktop.DBus.Properties","PropertiesChanged",NULL,NULL,0,changed,NULL,NULL);
    loop=g_main_loop_new(NULL,FALSE);g_unix_signal_add(SIGINT,quit,NULL);g_unix_signal_add(SIGTERM,quit,NULL);
    if(desktop_mode) {
        desktop=pro_control_new(&state,&release_at,&verbose_traffic,desktop_owner,FALSE,control_socket,loop,&err);
        if(!desktop)goto cleanup;

        pro_control_set_reconnect(desktop,reconnect_from_control,NULL);

        while(!g_queue_is_empty(&pending_logs)) {
            PendingLog *item=g_queue_pop_head(&pending_logs);
            pro_control_log(desktop,item->event,item->detail);pending_log_free(item);
        }
    }
    g_timeout_add(15,tick,NULL);g_timeout_add_seconds(5,stats,NULL);
    if(!desktop_mode)g_timeout_add_seconds(600,quit,NULL);
    GIOChannel *io=g_io_channel_unix_new(STDIN_FILENO);
    g_io_channel_set_flags(io,g_io_channel_get_flags(io)|G_IO_FLAG_NONBLOCK,NULL);
    g_io_add_watch(io,G_IO_IN|G_IO_HUP,input,NULL);
    g_main_loop_run(loop);g_io_channel_unref(io);g_main_loop_unref(loop);result=0;
cleanup:
    shutting_down=TRUE;

    if(pairing_reconnect_source) {
        g_source_remove(pairing_reconnect_source);
        pairing_reconnect_source=0;
    }

    pro_control_free(desktop);desktop=NULL;
    if(err) {log_event("error",err->message);g_error_free(err);}
    reset_link();for(int i=0;i<2;i++) if(listeners[i]>=0) close(listeners[i]);
    if(registered_profile) invoke("/org/bluez","org.bluez.ProfileManager1","UnregisterProfile",g_variant_new("(o)",ROOT "/profile"));
    if(registered_agent) invoke("/org/bluez","org.bluez.AgentManager1","UnregisterAgent",g_variant_new("(o)",ROOT "/agent"));
    gboolean restored=TRUE;
    for(int i=5;i>=1;i--) if(saved[i]) {
        if(!set_property(keys[i],saved[i])) restored=FALSE;
        g_variant_unref(saved[i]);
    }
    if(have_class && hci_write_class_of_dev(dd,old_class[0]|((uint32_t)old_class[1]<<8)|((uint32_t)old_class[2]<<16),2000)<0) restored=FALSE;
    if(saved[0]) {
        if(!set_property(keys[0],saved[0]))restored=FALSE;
        g_variant_unref(saved[0]);
    }
    if(pairing_hci_watch) {
        g_source_remove(pairing_hci_watch);
        pairing_hci_watch=0;
    }
    if(pairing_hci_fd>=0) {
        close(pairing_hci_fd);
        pairing_hci_fd=-1;
    }

    if(pairing_mgmt_watch) {
        g_source_remove(pairing_mgmt_watch);
        pairing_mgmt_watch=0;
    }
    if(pairing_mgmt_fd>=0) {
        close(pairing_mgmt_fd);
        pairing_mgmt_fd=-1;
    }

    if(dd>=0) close(dd);
    if(profile_id) g_dbus_connection_unregister_object(bus,profile_id);
    if(agent_id) g_dbus_connection_unregister_object(bus,agent_id);
    g_dbus_node_info_unref(node);g_object_unref(bus);desktop_requested=FALSE;
    g_queue_clear_full(&pending_logs,(GDestroyNotify)pending_log_free);
    log_event("stopped",restored?"Adapter properties restored; pairing keys retained by BlueZ":"Adapter restoration incomplete; inspect earlier errors");return restored?result:1;
}
