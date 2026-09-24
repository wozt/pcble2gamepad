/*
 * Optional Switch Pro Controller bonding experiment using BTstack's Linux
 * HCI_CHANNEL_USER transport. BTstack is a separate, non-commercial
 * dependency; see BTSTACK-LICENSE and build-btstack.sh.
 */
#define _POSIX_C_SOURCE 200809L
#include "btstack_config.h"
#include "protocol.h"

#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_posix.h"
#include "btstack_signal.h"
#include "btstack_tlv.h"
#include "btstack_tlv_posix.h"
#include "btstack_util.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/device_id_server.h"
#include "classic/hid_device.h"
#include "classic/sdp_server.h"
#include "gap.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_posix_fs.h"
#include "hci_transport_linux.h"
#include "l2cap.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPLY_QUEUE_LENGTH 8
#define PRO_REPORT_LENGTH 50
#define STREAM_TICK_MS 15
#define PAIRING_INITIAL_MS 1000
#define PAIRING_ACTIVE_MS 67
#define RECONNECT_DELAY_MS 1500
#define RECONNECT_MAX_ATTEMPTS 4

static const uint8_t report_descriptor[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,
    0x06,0x00,0xff,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x30,
    0x85,0x30,0x09,0x30,0x81,0x02,
    0x85,0x21,0x09,0x21,0x81,0x02,
    0x85,0x3f,0x09,0x3f,0x95,0x0b,0x81,0x02,
    0x85,0x01,0x09,0x01,0x95,0x30,0x91,0x02,
    0x85,0x10,0x09,0x10,0x91,0x02,
    0xc0
};

static hci_transport_config_linux_t transport_config = {
    .type = HCI_TRANSPORT_CONFIG_LINUX,
    .device_id = 0
};
static btstack_tlv_posix_t tlv_context;
static const btstack_tlv_t *tlv_impl;
static btstack_packet_callback_registration_t event_registration;
static btstack_timer_source_t stream_timer;
static btstack_timer_source_t reconnect_timer;
static ProState controller;
static bd_addr_t local_address;
static bd_addr_t peer_address;
static bool have_peer;
static bool have_bond;
static bool shutting_down;
static bool reconnect_enabled = true;
static bool saw_output;
static bool initialized;
static bool can_send_requested;
static bool regular_report_due;
static uint16_t hid_cid;
static uint32_t next_report_ms;
static unsigned sent_reports;
static unsigned received_reports;
static unsigned reconnect_attempts;
static uint8_t reply_queue[REPLY_QUEUE_LENGTH][PRO_REPORT_LENGTH];
static unsigned reply_head;
static unsigned reply_count;

static void log_line(const char *event, const char *detail) {
    struct timespec now;
    struct tm utc;
    char timestamp[40];
    clock_gettime(CLOCK_REALTIME, &now);
    gmtime_r(&now.tv_sec, &utc);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
    printf("%s.%06ldZ %s %s\n", timestamp, now.tv_nsec / 1000, event, detail);
    fflush(stdout);
}

static void format_address(const bd_addr_t address, char output[18]) {
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[0], address[1], address[2], address[3], address[4], address[5]);
}

static uint8_t report_timer(void) {
    return (uint8_t)(btstack_run_loop_get_time_ms() / 5u);
}

static void request_send(void) {
    if (!hid_cid || can_send_requested)
        return;
    can_send_requested = true;
    hid_device_request_can_send_now_event(hid_cid);
}

static void queue_reply(const uint8_t report[PRO_REPORT_LENGTH]) {
    if (reply_count == REPLY_QUEUE_LENGTH) {
        log_line("hid_reply_error", "Reply queue full");
        return;
    }
    unsigned tail = (reply_head + reply_count) % REPLY_QUEUE_LENGTH;
    memcpy(reply_queue[tail], report, PRO_REPORT_LENGTH);
    reply_count++;
    request_send();
}

static void handle_can_send_now(void) {
    can_send_requested = false;
    if (!hid_cid)
        return;

    if (reply_count) {
        hid_device_send_interrupt_message(hid_cid, reply_queue[reply_head], PRO_REPORT_LENGTH);
        reply_head = (reply_head + 1u) % REPLY_QUEUE_LENGTH;
        reply_count--;
        sent_reports++;
    } else if (regular_report_due) {
        uint8_t report[PRO_REPORT_LENGTH];
        regular_report_due = false;
        pro_input(&controller, report_timer(), report);
        hid_device_send_interrupt_message(hid_cid, report, sizeof(report));
        sent_reports++;
    }

    if (reply_count || regular_report_due)
        request_send();
}

static void stream_timer_handler(btstack_timer_source_t *timer) {
    if (!hid_cid)
        return;

    uint32_t now = btstack_run_loop_get_time_ms();
    if (btstack_time_delta(now, next_report_ms) >= 0) {
        regular_report_due = true;
        request_send();
        next_report_ms = now + (saw_output ? PAIRING_ACTIVE_MS : PAIRING_INITIAL_MS);
    }

    btstack_run_loop_set_timer(timer, STREAM_TICK_MS);
    btstack_run_loop_add_timer(timer);
}

static void start_stream(void) {
    saw_output = false;
    initialized = false;
    regular_report_due = false;
    can_send_requested = false;
    reply_head = 0;
    reply_count = 0;
    next_report_ms = btstack_run_loop_get_time_ms();
    btstack_run_loop_remove_timer(&stream_timer);
    btstack_run_loop_set_timer_handler(&stream_timer, stream_timer_handler);
    btstack_run_loop_set_timer(&stream_timer, STREAM_TICK_MS);
    btstack_run_loop_add_timer(&stream_timer);
}

static void stop_stream(void) {
    btstack_run_loop_remove_timer(&stream_timer);
    can_send_requested = false;
    regular_report_due = false;
    reply_head = 0;
    reply_count = 0;
}

static void process_output(const uint8_t *packet, size_t size) {
    uint8_t reply[PRO_REPORT_LENGTH];
    received_reports++;
    saw_output = true;

    if (!pro_reply(&controller, packet, size, report_timer(), reply)) {
        if (size >= 2 && packet[0] == 0xa2 && packet[1] == 0x01)
            log_line("unsupported_subcommand", "No invented ACK sent");
        return;
    }

    char detail[128];
    snprintf(detail, sizeof(detail),
             "subcommand=0x%02x mode=0x%02x lights=0x%02x",
             packet[11], controller.mode, controller.lights);
    log_line("hid_reply", detail);
    queue_reply(reply);

    if (!initialized && controller.lights && controller.vibration) {
        initialized = true;
        log_line("initialization_observed",
                 "Player lights and vibration configured by the console");
    }
}

static void report_data(uint16_t cid, hid_report_type_t type, uint16_t report_id,
                        int report_size, uint8_t *report) {
    (void)cid;
    if (type != HID_REPORT_TYPE_OUTPUT || report_size < 0 || report_size > 1022)
        return;

    uint8_t packet[1024];
    packet[0] = 0xa2;
    packet[1] = (uint8_t)report_id;
    memcpy(packet + 2, report, (size_t)report_size);
    process_output(packet, (size_t)report_size + 2u);
}

static void set_report(uint16_t cid, hid_report_type_t type, int report_size,
                       uint8_t *report) {
    (void)cid;
    if (type != HID_REPORT_TYPE_OUTPUT || report_size < 1 || report_size > 1023)
        return;

    uint8_t packet[1024];
    packet[0] = 0xa2;
    memcpy(packet + 1, report, (size_t)report_size);
    process_output(packet, (size_t)report_size + 1u);
}

static void enter_pairable(void) {
    gap_set_security_level(LEVEL_0);
    gap_connectable_control(1);
    gap_discoverable_control(1);
}

static void reconnect_timer_handler(btstack_timer_source_t *timer) {
    (void)timer;
    if (!reconnect_enabled || !have_bond || !have_peer || hid_cid || shutting_down)
        return;
    if (reconnect_attempts >= RECONNECT_MAX_ATTEMPTS) {
        log_line("reconnect_exhausted",
                 "Four controller-initiated attempts failed; remaining passive");
        return;
    }

    reconnect_attempts++;
    gap_set_security_level(LEVEL_2);
    uint16_t pending_cid = 0;
    uint8_t status = hid_device_connect(peer_address, &pending_cid);
    char address[18];
    char detail[128];
    format_address(peer_address, address);
    snprintf(detail, sizeof(detail), "peer=%s attempt=%u/%u status=0x%02x",
             address, reconnect_attempts, RECONNECT_MAX_ATTEMPTS, status);
    log_line("reconnect_attempt", detail);
}

static void schedule_reconnect(void) {
    if (!reconnect_enabled || !have_bond || !have_peer || shutting_down)
        return;
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer_handler(&reconnect_timer, reconnect_timer_handler);
    btstack_run_loop_set_timer(&reconnect_timer, RECONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&reconnect_timer);
}

static void load_first_bond(void) {
    btstack_link_key_iterator_t iterator;
    if (!gap_link_key_iterator_init(&iterator)) {
        log_line("bond_store_error", "Link Key iterator unavailable");
        return;
    }

    link_key_t key;
    link_key_type_t type;
    unsigned count = 0;
    bd_addr_t address;
    while (gap_link_key_iterator_get_next(&iterator, address, key, &type)) {
        if (!count) {
            memcpy(peer_address, address, sizeof(peer_address));
            have_peer = true;
            have_bond = true;
        }
        count++;
    }
    gap_link_key_iterator_done(&iterator);

    char detail[128];
    if (have_peer) {
        char address_text[18];
        format_address(peer_address, address_text);
        snprintf(detail, sizeof(detail), "count=%u peer=%s", count, address_text);
    } else {
        snprintf(detail, sizeof(detail), "count=%u", count);
    }
    log_line("bond_store", detail);
}

static void hid_event(const uint8_t *packet) {
    uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
    if (subevent == HID_SUBEVENT_CONNECTION_OPENED) {
        uint8_t status = hid_subevent_connection_opened_get_status(packet);
        if (status) {
            char detail[64];
            snprintf(detail, sizeof(detail), "status=0x%02x", status);
            log_line("hid_connect_error", detail);
            enter_pairable();
            schedule_reconnect();
            return;
        }

        hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
        reconnect_attempts = 0;
        hid_subevent_connection_opened_get_bd_addr(packet, peer_address);
        have_peer = true;
        gap_discoverable_control(0);
        gap_connectable_control(0);
        uint8_t mac[6];
        memcpy(mac, local_address, sizeof(mac));
        controller_init(&controller, CONTROLLER_PRO, mac);
        start_stream();

        char address[18];
        char detail[160];
        format_address(peer_address, address);
        snprintf(detail, sizeof(detail), "peer=%s direction=%s cid=%u",
                 address,
                 hid_subevent_connection_opened_get_incoming(packet) ? "switch" : "controller",
                 hid_cid);
        log_line("hid_connected", detail);
        return;
    }

    if (subevent == HID_SUBEVENT_CONNECTION_CLOSED) {
        log_line("hid_disconnected", "HID control and interrupt channels closed");
        hid_cid = 0;
        stop_stream();
        enter_pairable();
        schedule_reconnect();
        return;
    }

    if (subevent == HID_SUBEVENT_CAN_SEND_NOW)
        handle_can_send_now();
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event = hci_event_packet_get_type(packet);
    if (event == BTSTACK_EVENT_STATE) {
        uint8_t state = btstack_event_state_get_state(packet);
        if (state == HCI_STATE_WORKING) {
            gap_local_bd_addr(local_address);
            uint8_t mac[6];
            memcpy(mac, local_address, sizeof(mac));
            controller_init(&controller, CONTROLLER_PRO, mac);
            char address[18];
            format_address(local_address, address);
            log_line("hci_ready", address);
            load_first_bond();
            enter_pairable();
            schedule_reconnect();
        } else if (state == HCI_STATE_OFF && shutting_down) {
            btstack_run_loop_trigger_exit();
        }
        return;
    }

    if (event == HCI_EVENT_HID_META) {
        hid_event(packet);
        return;
    }

    if (event == HCI_EVENT_CONNECTION_REQUEST) {
        bd_addr_t address;
        hci_event_connection_request_get_bd_addr(packet, address);
        char address_text[18];
        char detail[128];
        format_address(address, address_text);
        snprintf(detail, sizeof(detail), "peer=%s cod=0x%06lx", address_text,
                 (unsigned long)hci_event_connection_request_get_class_of_device(packet));
        log_line("connection_request", detail);
        return;
    }

    if (event == HCI_EVENT_IO_CAPABILITY_RESPONSE) {
        bd_addr_t address;
        hci_event_io_capability_response_get_bd_addr(packet, address);
        char address_text[18];
        char detail[160];
        format_address(address, address_text);
        snprintf(detail, sizeof(detail), "peer=%s io=0x%02x auth=0x%02x oob=0x%02x",
                 address_text,
                 hci_event_io_capability_response_get_io_capability(packet),
                 hci_event_io_capability_response_get_authentication_requirements(packet),
                 hci_event_io_capability_response_get_oob_data_present(packet));
        log_line("pairing_peer_io", detail);
        return;
    }

    if (event == HCI_EVENT_IO_CAPABILITY_REQUEST) {
        log_line("pairing_local_io",
                 "io=NoInputNoOutput(0x03) auth=dedicated-bonding/no-mitm(0x02)");
        return;
    }

    if (event == HCI_EVENT_USER_CONFIRMATION_REQUEST) {
        log_line("pairing_confirmation", "Just Works auto-accepted");
        return;
    }

    if (event == HCI_EVENT_SIMPLE_PAIRING_COMPLETE) {
        uint8_t status = hci_event_simple_pairing_complete_get_status(packet);
        char detail[64];
        snprintf(detail, sizeof(detail), "status=0x%02x", status);
        log_line("pairing_ssp_complete", detail);
        return;
    }

    if (event == HCI_EVENT_LINK_KEY_NOTIFICATION) {
        if (size >= 25) {
            bd_addr_t address;
            reverse_bd_addr(packet + 2, address);
            memcpy(peer_address, address, sizeof(peer_address));
            have_peer = true;
            have_bond = true;
            char address_text[18];
            char detail[128];
            format_address(address, address_text);
            snprintf(detail, sizeof(detail), "peer=%s key_type=0x%02x persistent=tlv",
                     address_text, packet[24]);
            log_line("pairing_key_saved", detail);
        }
        return;
    }

    if (event == HCI_EVENT_AUTHENTICATION_COMPLETE) {
        char detail[64];
        snprintf(detail, sizeof(detail), "status=0x%02x",
                 hci_event_authentication_complete_get_status(packet));
        log_line("authentication_complete", detail);
        return;
    }

    if (event == HCI_EVENT_ENCRYPTION_CHANGE) {
        char detail[80];
        snprintf(detail, sizeof(detail), "status=0x%02x enabled=%u",
                 hci_event_encryption_change_get_status(packet),
                 hci_event_encryption_change_get_encryption_enabled(packet));
        log_line("encryption_change", detail);
        return;
    }

    if (event == HCI_EVENT_DISCONNECTION_COMPLETE) {
        char detail[64];
        snprintf(detail, sizeof(detail), "reason=0x%02x",
                 hci_event_disconnection_complete_get_reason(packet));
        log_line("acl_disconnected", detail);
    }
}

static void shutdown_handler(void) {
    if (shutting_down)
        return;
    shutting_down = true;
    log_line("shutdown", "Powering off userspace HCI stack");
    btstack_run_loop_remove_timer(&reconnect_timer);
    stop_stream();
    hci_power_control(HCI_POWER_OFF);
}

void hal_led_toggle(void) {
}

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --device-id N --tlv PATH --logfile PATH [--reset-bond] [--passive]\n",
            program);
}

int main(int argc, char **argv) {
    const char *tlv_path = NULL;
    const char *log_path = NULL;
    bool reset_bond = false;
    static const struct option options[] = {
        {"device-id", required_argument, NULL, 'u'},
        {"tlv", required_argument, NULL, 't'},
        {"logfile", required_argument, NULL, 'l'},
        {"reset-bond", no_argument, NULL, 'r'},
        {"passive", no_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    for (;;) {
        int option = getopt_long(argc, argv, "u:t:l:rph", options, NULL);
        if (option < 0)
            break;
        switch (option) {
        case 'u': {
            char *end = NULL;
            unsigned long id = strtoul(optarg, &end, 10);
            if (!end || *end || id > UINT16_MAX) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            transport_config.device_id = (int)id;
            break;
        }
        case 't': tlv_path = optarg; break;
        case 'l': log_path = optarg; break;
        case 'r': reset_bond = true; break;
        case 'p': reconnect_enabled = false; break;
        default:
            usage(argv[0]);
            return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (!tlv_path || !log_path || optind != argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (reset_bond && unlink(tlv_path) < 0 && errno != ENOENT) {
        perror("unlink TLV store");
        return EXIT_FAILURE;
    }

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    hci_dump_posix_fs_open(log_path, HCI_DUMP_PACKETLOGGER);
    hci_dump_init(hci_dump_posix_fs_get_instance());

    tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, tlv_path);
    btstack_tlv_set_instance(tlv_impl, &tlv_context);

    hci_init(hci_transport_linux_instance(), &transport_config);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));

    gap_set_class_of_device(0x002508);
    gap_set_local_name("Pro Controller");
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_set_page_timeout(0x2000);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    gap_secure_connections_enable(true);

    l2cap_init();
    sdp_init();

    static uint8_t hid_record[500];
    hid_sdp_record_t hid_parameters = {
        .hid_device_subclass = 0x2508,
        .hid_country_code = 33,
        .hid_virtual_cable = 1,
        .hid_remote_wake = 1,
        .hid_reconnect_initiate = 1,
        .hid_normally_connectable = false,
        .hid_boot_device = false,
        .hid_ssr_host_max_latency = 0xffff,
        .hid_ssr_host_min_timeout = 0xffff,
        .hid_supervision_timeout = 3200,
        .hid_descriptor = report_descriptor,
        .hid_descriptor_size = sizeof(report_descriptor),
        .device_name = "Pro Controller"
    };
    hid_create_sdp_record(hid_record, sdp_create_service_record_handle(), &hid_parameters);
    sdp_register_service(hid_record);

    static uint8_t device_id_record[100];
    device_id_create_sdp_record(device_id_record, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_USB, 0x057e, 0x2009, 0x0001);
    sdp_register_service(device_id_record);

    gap_set_security_level(LEVEL_0);
    hid_device_init(false, sizeof(report_descriptor), report_descriptor);
    hid_device_accept_truncated_hid_reports(true);
    hid_device_register_packet_handler(packet_handler);
    hid_device_register_report_data_callback(report_data);
    hid_device_register_set_report_callback(set_report);

    event_registration.callback = packet_handler;
    hci_add_event_handler(&event_registration);
    /* BTstack's POSIX helper waits for SIGINT only. The runner restores
     * BlueZ even when an external SIGTERM ends this process directly. */
    btstack_signal_register_callback(SIGINT, shutdown_handler);

    log_line("starting", "BTstack HCI user-channel Pro Controller experiment");
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();

    btstack_tlv_posix_deinit(&tlv_context);
    log_line("stopped", "Userspace HCI stack stopped");
    return EXIT_SUCCESS;
}
