/**\file    net_io.h
 * \ingroup Main
 *
 * Most network functions for Dump1090.
 */
#ifndef _NETWORK_H
#define _NETWORK_H

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
 * Default network port numbers:
 */
#define MODES_NET_PORT_RAW_IN   30001
#define MODES_NET_PORT_RAW_OUT  30002
#define MODES_NET_PORT_SBS      30003
#define MODES_NET_PORT_HTTP      8080

extern net_service modeS_net_services [MODES_NET_SERVICES_NUM];

typedef bool (*net_msg_handler) (mg_iobuf *msg, int loop_cnt);

/**
 * A function-pointer for either `mg_listen()` or `mg_http_listen()`.
 */
typedef struct mg_connection *(*mg_listen_func) (struct mg_mgr     *mgr,
                                                 const char        *url,
                                                 mg_event_handler_t fn,
                                                 void              *fn_data);

bool     net_init (void);
bool     net_exit (void);
void     net_poll (void);
void     net_show_stats (void);

uint16_t    net_handler_port (intptr_t service);
const char *net_handler_protocol (intptr_t service);
bool        net_handler_sending (intptr_t service);
void        net_connection_recv (connection *conn, net_msg_handler handler, bool is_server);
void        net_connection_send (intptr_t service, const void *msg, size_t len);
bool        net_set_host_port (const char *host_port, net_service *serv, uint16_t def_port);

#endif  /* _NETWORK_H */

