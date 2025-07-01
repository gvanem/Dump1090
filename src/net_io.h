/**\file    net_io.h
 * \ingroup Main
 *
 * Network functions for Dump1090.
 */
#pragma once

#include "misc.h"

/**
 * \def INDEX_HTML
 * Our default main server page relative to `Modes.where_am_I`.
 */
#define INDEX_HTML   "web_root/index.html"

/**
 * Timeout for an active connect.
 */
#define MODES_CONNECT_TIMEOUT      5000

/**
 * Various HTTP content headers values.
 */
#define MODES_CONTENT_TYPE_ICON   "image/x-icon"
#define MODES_CONTENT_TYPE_JSON   "application/json"
#define MODES_CONTENT_TYPE_PNG    "image/png"

/**
 * The `readsb` program will send 5 heart-beats like this
 * in RAW mode.
 */
#define MODES_RAW_HEART_BEAT      "*0000;\n*0000;\n*0000;\n*0000;\n*0000;\n"

/**
 * The max length of an IPv4/6 address or ACL spec.
 */
#define MAX_ADDRESS 50

typedef char ip_address [MAX_ADDRESS];

/**
 * Default network port numbers:
 */
#define MODES_NET_PORT_RAW_IN   30001
#define MODES_NET_PORT_RAW_OUT  30002
#define MODES_NET_PORT_SBS      30003
#define MODES_NET_PORT_HTTP      8080
#define MODES_NET_PORT_RTL_TCP   1234
#define MODES_NET_PORT_DNS_UDP     53

extern net_service modeS_net_services [MODES_NET_SERVICES_NUM];

/**
 * \typedef net_msg_handler
 * The function-type for handling "RAW TCP Input", "SBS TCP Input"
 * and "DNS input" messages.
 */
typedef bool (*net_msg_handler) (mg_iobuf *msg, int loop_cnt);

bool      net_init (void);
bool      net_exit (void);
void      net_poll (void);
uint16_t  net_handler_port (intptr_t service);
char     *net_handler_host (intptr_t service);
char     *net_handler_protocol (intptr_t service);
char     *net_handler_url (intptr_t service);
char     *net_handler_descr (intptr_t service);
char     *net_handler_error (intptr_t service);
bool      net_handler_sending (intptr_t service);
void      net_handler_send (intptr_t service, const void *msg, size_t len);
bool      net_set_host_port (const char *host_port, net_service *serv, uint16_t def_port);
void      net_show_stats (void);
bool      net_deny4 (const char *val);
bool      net_deny6 (const char *val);

/**
 * Timeout for reception of RTL_TCP data.
 */
#define MODES_DATA_TIMEOUT 2000

bool rtl_tcp_set_gain      (mg_connection *c, int16_t gain);
bool rtl_tcp_set_gain_mode (mg_connection *c, bool autogain);
