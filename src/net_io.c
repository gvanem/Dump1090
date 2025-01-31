/**\file    net_io.c
 * \ingroup Main
 * \brief   Most network functions and handling of network services.
 */
#include <stdint.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>

#include "misc.h"
#include "aircraft.h"
#include "net_io.h"
#include "rtl-tcp.h"
#include "speech.h"
#include "server-cert-key.h"
#include "client-cert-key.h"

/**
 * Handlers for the network services.
 *
 * We use Mongoose for handling all the server and low-level network I/O. <br>
 * We register event-handlers that gets called on important network events.
 *
 * Keep the data for all our 6 network services in this structure.
 */
net_service modeS_net_services [MODES_NET_SERVICES_NUM] = {
          { &Modes.raw_out,    "Raw TCP output", "tcp", MODES_NET_PORT_RAW_OUT },  // MODES_NET_SERVICE_RAW_OUT
          { &Modes.raw_in,     "Raw TCP input",  "tcp", MODES_NET_PORT_RAW_IN  },  // MODES_NET_SERVICE_RAW_IN
          { &Modes.sbs_out,    "SBS TCP output", "tcp", MODES_NET_PORT_SBS     },  // MODES_NET_SERVICE_SBS_OUT
          { &Modes.sbs_in,     "SBS TCP input",  "tcp", MODES_NET_PORT_SBS     },  // MODES_NET_SERVICE_SBS_IN
          { &Modes.http_out,   "HTTP server",    "tcp", MODES_NET_PORT_HTTP    },  // MODES_NET_SERVICE_HTTP
          { &Modes.rtl_tcp_in, "RTL-TCP input",  "tcp", MODES_NET_PORT_RTL_TCP }   // MODES_NET_SERVICE_RTL_TCP
        };

/**
 * Handling a "Packed Web FileSystem".
 */
static bool use_packed_dll = false;
static bool use_bsearch    = false;

static file_packed *lookup_table = NULL;
static size_t       lookup_table_sz;
static uint32_t     num_lookups, num_misses;

/**
 * Define the func-ptr to the `mg_unpack()` + `mg_unlist()` functions loaded
 * dynamically from the `web-page = some.dll;N` config-file setting.
 */
DEF_C_FUNC (const char *, mg_unpack, (const char *name, size_t *size, time_t *mtime));
DEF_C_FUNC (const char *, mg_unlist, (size_t i));

/**
 * For handling denial of clients in `client_handler (.., MG_EV_ACCEPT)` .
 */
typedef struct deny_element {
        ip_address           acl;
        bool                 is_ip6;
        struct deny_element *next;
      } deny_element;

static deny_element *g_deny_list = NULL;

/**
 * For handling a list of unique network clients.
 * A list of address, service and time first seen.
 */
typedef struct unique_IP {
        mg_addr           addr;      /**< The IPv4/6 address */
        intptr_t          service;   /**< unique in service */
        FILETIME          seen;      /**< time when this address was created */
        uint32_t          accepted;  /**< number of times for `accept()` */
        uint32_t          denied;    /**< number of times denied */
        struct unique_IP *next;
      } unique_IP;

static unique_IP *g_unique_ips = NULL;

/**
 * For handling timers in each network service.
 */
typedef struct timeout_data {
        mg_timer *timer;
        intptr_t  service;
        int32_t   id;
      } timeout_data;

static timeout_data service_timers [MODES_NET_SERVICES_NUM];

static void        net_handler (mg_connection *c, int ev, void *ev_data);
static void        net_timer_add (intptr_t service, int timeout_ms, int flag);
static void        net_timer_del (intptr_t service);
static void        net_conn_free (connection *conn, intptr_t service);
static char       *net_store_error (intptr_t service, const char *err);
static char       *net_error_details (mg_connection *c, const char *in_out, const void *ev_data);
static char       *net_str_addr (const mg_addr *a, char *buf, size_t len);
static uint16_t   *net_num_connections (intptr_t service);
static const char *net_service_descr (intptr_t service);
static char       *net_service_error (intptr_t service);
static char       *net_service_url (intptr_t service);
static bool        client_is_extern (const mg_addr *addr);
static bool        client_handler (mg_connection *c, intptr_t service, int ev);
const char        *mg_unpack (const char *path, size_t *size, time_t *mtime);

/**
 * Remote RTL_TCP server functions.
 */
static bool rtl_tcp_connect (mg_connection *c);
static bool rtl_tcp_decode (mg_iobuf *msg, int loop_cnt);

/**
 * \def ASSERT_SERVICE(s)
 * Assert the service `s` is in legal range.
 */
#define ASSERT_SERVICE(s)  assert (s >= MODES_NET_SERVICE_FIRST); \
                           assert (s <= MODES_NET_SERVICE_LAST)

/**
 * \def HEX_DUMP(data, len)
 * Do a hex-dump of network data if option `--debug M` was used.
 */
#define HEX_DUMP(data, len)                  \
        do {                                 \
          if (Modes.debug & DEBUG_MONGOOSE2) \
             mg_hexdump (data, len);         \
        } while (0)

/**
 * For 'deny_lists_test()' and 'unique_ip_tests()'
 */
#define HOSTILE_IP_1  "45.128.232.127"
#define HOSTILE_IP_2  "80.94.95.226"

/**
 * Mongoose event names.
 */
static const char *event_name (int ev)
{
  static char buf [20];

  if (ev >= MG_EV_USER)
  {
    snprintf (buf, sizeof(buf), "MG_EV_USER%d", ev - MG_EV_USER);
    return (buf);
  }

  return (ev == MG_EV_OPEN       ? "MG_EV_OPEN" :    /* Event on 'connect()', 'listen()' and 'accept()' */
          ev == MG_EV_POLL       ? "MG_EV_POLL" :
          ev == MG_EV_RESOLVE    ? "MG_EV_RESOLVE" :
          ev == MG_EV_CONNECT    ? "MG_EV_CONNECT" :
          ev == MG_EV_ACCEPT     ? "MG_EV_ACCEPT" :
          ev == MG_EV_READ       ? "MG_EV_READ" :
          ev == MG_EV_WRITE      ? "MG_EV_WRITE" :
          ev == MG_EV_CLOSE      ? "MG_EV_CLOSE" :
          ev == MG_EV_ERROR      ? "MG_EV_ERROR" :
          ev == MG_EV_HTTP_MSG   ? "MG_EV_HTTP_MSG" :
          ev == MG_EV_HTTP_HDRS  ? "MG_EV_HTTP_HDRS" :
          ev == MG_EV_WS_OPEN    ? "MG_EV_WS_OPEN" :
          ev == MG_EV_WS_MSG     ? "MG_EV_WS_MSG" :
          ev == MG_EV_WS_CTL     ? "MG_EV_WS_CTL" :
          ev == MG_EV_TLS_HS     ? "MG_EV_TLS_HS" :
          ev == MG_EV_MQTT_CMD   ? "MG_EV_MQTT_CMD" :   /* Can never occur here */
          ev == MG_EV_MQTT_MSG   ? "MG_EV_MQTT_MSG" :   /* Can never occur here */
          ev == MG_EV_MQTT_OPEN  ? "MG_EV_MQTT_OPEN" :  /* Can never occur here */
          ev == MG_EV_SNTP_TIME  ? "MG_EV_SNTP_TIME" :  /* Can never occur here */
          ev == MG_EV_WAKEUP     ? "MG_EV_WAKEUP"       /* Can never occur here */
                                 : "?");                /* Ref: https://mongoose.ws/documentation/tutorials/core/multi-threaded/ */
}

/**
 * Setup a connection for a service.
 * Active or passive (`listen == true`).
 * If it's active, we could use UDP.
 */
static mg_connection *connection_setup (intptr_t service, bool listen, bool sending)
{
  mg_connection *c = NULL;
  bool           allow_udp = (service == MODES_NET_SERVICE_RAW_IN ||
                              service == MODES_NET_SERVICE_RTL_TCP);
  bool           use_udp = (modeS_net_services [service].is_udp && !modeS_net_services [service].is_ip6);
  char          *url;

  /* Temporary enable important errors to go to `stderr` only.
   * For both an active and listen (passive) coonection we handle
   * "early" errors (like out of memory) by returning NULL.
   * A failed active connection will fail later. See comment below.
   */
  mg_log_set_fn (modeS_logc, stderr);
  mg_log_set (MG_LL_ERROR);
  modeS_err_set (true);

  if (use_udp && !allow_udp)
  {
    LOG_STDERR ("'udp://%s:%u' is not allowed for service %s (only TCP).\n",
                modeS_net_services [service].host,
                modeS_net_services [service].port,
                net_service_descr(service));
    goto quit;
  }

  modeS_net_services [service].active_send = sending;

  if (listen)
  {
    url = mg_mprintf ("%s://0.0.0.0:%u",
                      modeS_net_services [service].protocol,
                      modeS_net_services [service].port);
    modeS_net_services [service].url = url;

    if (service == MODES_NET_SERVICE_HTTP)
         c = mg_http_listen (&Modes.mgr, url, net_handler, (void*)service);
    else c = mg_listen (&Modes.mgr, url, net_handler, (void*)service);
  }
  else
  {
    /* For an active connect(), we'll get one of these event in net_handler():
     *  - MG_EV_ERROR    -- the `--host-xx` argument was not resolved or the connection failed or timed out.
     *  - MG_EV_RESOLVE  -- the `--host-xx` argument was successfully resolved to an IP-address.
     *  - MG_EV_CONNECT  -- successfully connected.
     */
    int timeout = MODES_CONNECT_TIMEOUT;  /* 5 sec */

    if (modeS_net_services [service].is_udp)
       timeout = -1;      /* Should UDP expire? */

    url = mg_mprintf ("%s://%s:%u",
                      modeS_net_services [service].protocol,
                      modeS_net_services [service].host,
                      modeS_net_services [service].port);
    modeS_net_services [service].url = url;

    DEBUG (DEBUG_NET, "Connecting to '%s' (service \"%s\").\n",
           url, net_service_descr(service));

    net_timer_add (service, timeout, MG_TIMER_ONCE);
    c = mg_connect (&Modes.mgr, url, net_handler, (void*)service);
  }

  if (Modes.https_enable && service == MODES_NET_SERVICE_HTTP)
     strcpy (modeS_net_services [MODES_NET_SERVICE_HTTP].descr, "HTTP(S) Server");

  if (c && (Modes.debug & DEBUG_MONGOOSE2))
     c->is_hexdumping = 1;

quit:
  modeS_err_set (false);
  modeS_log_set();         /* restore previous log-settings */
  return (c);
}

/**
 * This function reads client/server data for services:
 *  \li `MODES_NET_SERVICE_RAW_IN` or
 *  \li `MODES_NET_SERVICE_SBS_IN`
 *
 * when the event `MG_EV_READ` is received in `net_handler()`.
 *
 * The message is supposed to be separated by the next message by a
 * separator `sep` checked for in the `handler` function.
 *
 * The `handler` function is also responsible for freeing `msg` as it consumes
 * each record in the `msg`. This `msg` can consist of several records or incomplete
 * records since Mongoose uses non-blocking sockets.
 *
 * The `tools/SBS_client.py` script is sending this in "RAW-OUT" test-mode:
 * ```
 *  *8d4b969699155600e87406f5b69f;\n
 * ```
 *
 * This message shows up as ICAO "4B9696" and Reg-num "TC-ETV" in `--interactive` mode.
 */
static void net_connection_recv (connection *conn, net_msg_handler handler, bool is_server)
{
  mg_iobuf *msg;
  int       loops;

  if (!conn)
     return;

  msg = &conn->c->recv;
  if (msg->len == 0)
  {
    DEBUG (DEBUG_NET2, "No msg for %s.\n", is_server ? "server" : "client");
    return;
  }

  for (loops = 0; msg->len > 0; loops++)
     (*handler) (msg, loops);
}

/**
 * Iterate over all the listening connections and send a `msg` to
 * all clients in the specified `service`.
 *
 * There can only be 1 service that matches this. But this
 * service can have many clients.
 *
 * \note
 *  \li This function is not used for sending HTTP data.
 *  \li This function is not called when `--net-active` is used.
 */
void net_connection_send (intptr_t service, const void *msg, size_t len)
{
  connection *conn;
  int         found = 0;

  for (conn = Modes.connections [service]; conn; conn = conn->next)
  {
    if (conn->service != service)
       continue;

    mg_send (conn->c, msg, len);   /* if write fails, the client gets freed in net_handler() */
    found++;
  }
  if (found > 0)
     DEBUG (DEBUG_NET2, "Sent %zd bytes to %d clients in service \"%s\".\n",
            len, found, net_service_descr(service));
}

/**
 * Returns a `connection *` based on the remote `addr` and `service`.
 * This can be either client or server.
 */
connection *connection_get (mg_connection *c, intptr_t service, bool is_server)
{
  connection *conn;

  ASSERT_SERVICE (service);

  for (conn = Modes.connections [service]; conn; conn = conn->next)
  {
    if (conn->service == service && !memcmp(&conn->rem, &c->rem, sizeof(mg_addr)))
       return (conn);
  }

  is_server ? Modes.stat.srv_unknown [service]++ :   /* Should never happen */
              Modes.stat.cli_unknown [service]++;
  return (NULL);
}

static const char *set_headers (const connection *cli, const char *content_type)
{
  static char headers [200];
  char       *p = headers;

  *p = '\0';
  if (content_type)
  {
    strcpy (headers, "Content-Type: ");
    p += strlen ("Content-Type: ");
    strcpy (headers, content_type);
    p += strlen (content_type);
    strcpy (p, "\r\n");
    p += 2;
  }

  if (Modes.keep_alive && cli->keep_alive)
  {
    strcpy (p, "Connection: keep-alive\r\n");
    Modes.stat.HTTP_keep_alive_sent++;
  }
  return (headers);
}

/*
 * Generated arrays from:
 *   xxd -i favicon.png
 *   xxd -i favicon.ico
 */
#include "favicon.c"

static int send_file_favicon (mg_connection *c, connection *cli, bool send_png)
{
  const char    *file;
  const uint8_t *data;
  size_t         data_len;
  const char    *content_type;

  if (send_png)
  {
    file = "favicon.png";
    data = favicon_png;
    data_len = favicon_png_len;
    content_type = MODES_CONTENT_TYPE_PNG;
  }
  else
  {
    file = "favicon.ico";
    data = favicon_ico;
    data_len = favicon_ico_len;
    content_type = MODES_CONTENT_TYPE_ICON;
  }

  DEBUG (DEBUG_NET2, "Sending %s (%s, %zu bytes, conn-id: %lu).\n",
         file, content_type, data_len, c->id);

  mg_printf (c, "HTTP/1.1 200 OK\r\n"
                "Content-Length: %lu\r\n"
                "%s\r\n", data_len, set_headers(cli, content_type));
  mg_send (c, data, data_len);
  c->is_resp = 0;
  return (200);
}

static int send_file (mg_connection *c, connection *cli, mg_http_message *hm,
                      const char *uri, const char *content_type)
{
  mg_http_serve_opts opts;
  mg_file_path       file;
  bool               found;
  const char        *packed = "";
  int                rc = 200;    /* Assume status 200 OK */

  memset (&opts, '\0', sizeof(opts));
  opts.extra_headers = set_headers (cli, content_type);

#if defined(USE_PACKED_DLL)
  if (use_packed_dll)
  {
    snprintf (file, sizeof(file), "%s", uri+1);
    opts.fs = &mg_fs_packed;
    packed  = "packed ";
    found = (mg_unpack(file, NULL, NULL) != NULL);
  }
  else  /* config-option 'web-page = web_rootX/index.html' used even if 'web-page.dll' was built */
#endif
  {
    snprintf (file, sizeof(file), "%s/%s", Modes.web_root, uri+1);
    found = (access(file, 0) == 0);
  }

  DEBUG (DEBUG_NET, "Serving %sfile: '%s', found: %d.\n", packed, slashify(file), found);
  DEBUG (DEBUG_NET2, "extra-headers: '%s'.\n", opts.extra_headers);

  mg_http_serve_file (c, hm, file, &opts);

  if (!found)
  {
    Modes.stat.HTTP_404_responses++;
    rc = 404;
  }
  return (rc);
}

/**
 * Return a description of the receiver in JSON.
 *  { "version" : "0.3", "refresh" : 1000, "history" : 3 }
 */
static char *receiver_to_json (void)
{
  int history_size = DIM(Modes.json_aircraft_history)-1;

  /* work out number of valid history entries
   */
  if (!Modes.json_aircraft_history [history_size].buf)
     history_size = Modes.json_aircraft_history_next;

  return mg_mprintf ("{\"version\": \"%s\", "
                      "\"refresh\": %llu, "
                      "\"history\": %d, "
                      "\"lat\": %.8g, "       /* if 'Modes.home_pos_ok == false', this is 0. */
                      "\"lon\": %.8g}",       /* ditto */
                      PROG_VERSION,
                      Modes.json_interval,
                      history_size,
                      Modes.home_pos.lat,
                      Modes.home_pos.lon);
}

/**
 * The event handler for all HTTP traffic.
 */
static int net_handler_http (mg_connection *c, mg_http_message *hm, mg_http_uri request_uri)
{
  mg_str      *header;
  connection  *cli;
  bool         is_dump1090, is_extended, is_HEAD, is_GET;
  const char  *content_type = NULL;
  const char  *uri, *dot, *first_nl;
  mg_host_name addr_buf;
  size_t       len;

  /* Make a copy of the URI for the caller
   */
  len = min (sizeof(mg_http_uri) - 1, hm->uri.len);
  uri = strncpy (request_uri, hm->uri.buf, len);
  request_uri [len] = '\0';

  first_nl = strchr (hm->head.buf, '\r');
  len = hm->head.len;

  if (first_nl > hm->head.buf - 1)
     len = first_nl - hm->head.buf;

  DEBUG (DEBUG_NET2, "\n"
         "  MG_EV_HTTP_MSG: (conn-id: %lu)\n"
         "    head:    '%.*s' ...\n"     /* 1st line in request */
         "    uri:     '%s'\n"
         "    method:  '%.*s'\n",
         c->id, (int)len, hm->head.buf, uri, (int)hm->method.len, hm->method.buf);

  is_GET  = (strnicmp(hm->method.buf, "GET", hm->method.len) == 0);
  is_HEAD = (strnicmp(hm->method.buf, "HEAD", hm->method.len) == 0);

  if (!is_GET && !is_HEAD)
  {
    DEBUG (DEBUG_NET, "Bad Request: '%.*s %s' from %s (conn-id: %lu)\n",
           (int)hm->method.len, hm->method.buf, uri,
           net_str_addr(&c->rem, addr_buf, sizeof(addr_buf)), c->id);

    Modes.stat.HTTP_400_responses++;
    return (400);
  }

  cli = connection_get (c, MODES_NET_SERVICE_HTTP, false);
  if (!cli)
     return (505);

  Modes.stat.HTTP_get_requests++;

  header = mg_http_get_header (hm, "Connection");
  if (header && !strnicmp(header->buf, "keep-alive", header->len))
  {
    DEBUG (DEBUG_NET2, "Connection: '%.*s'\n", (int)header->len, header->buf);
    Modes.stat.HTTP_keep_alive_recv++;
    cli->keep_alive = true;
  }

  header = mg_http_get_header (hm, "Accept-Encoding");
  if (header && !strnicmp(header->buf, "gzip", header->len))
  {
    DEBUG (DEBUG_NET2, "Accept-Encoding: '%.*s'\n", (int)header->len, header->buf);
    cli->encoding_gzip = true;  /**\todo Add gzip compression */
  }

  /* Redirect a 'GET /' to a 'GET /' + 'web_page'
   */
  if (!strcmp(uri, "/"))
  {
    mg_printf (c, "HTTP/1.1 301 Moved\r\n"
                  "Location: %s\r\n"
                  "Content-Length: 0\r\n\r\n", Modes.web_page);

    DEBUG (DEBUG_NET2, "301 redirect to: '%s/%s'\n", Modes.web_root, Modes.web_page);
    return (301);
  }

  /**
   * \todo Check header for a "Upgrade: websocket" and call mg_ws_upgrade()?
   */
  if (!stricmp(uri, "/echo"))
  {
    DEBUG (DEBUG_NET, "Got WebSocket echo:\n'%.*s'.\n", (int)hm->head.len, hm->head.buf);
    mg_ws_upgrade (c, hm, "WS test");
    return (200);
  }

  if (!stricmp(uri, "/data/receiver.json"))
  {
    char *data = receiver_to_json();

    DEBUG (DEBUG_NET2, "Feeding conn-id %lu with receiver-data:\n%.100s\n", c->id, data);

    mg_http_reply (c, 200, MODES_CONTENT_TYPE_JSON "\r\n", data);
    free (data);
    return (200);
  }

  /* What we normally expect with the default 'web_root/index.html'
   */
  is_dump1090 = stricmp (uri, "/data.json") == 0;

  /* Or From an OpenLayers3/Tar1090/FlightAware web-client
   */
  is_extended = (stricmp (uri, "/data/aircraft.json") == 0) ||
                (stricmp (uri, "/chunks/chunks.json") == 0);

  if (is_dump1090 || is_extended)
  {
    char *data = aircraft_make_json (is_extended);

    /* "Cross Origin Resource Sharing":
     * https://www.freecodecamp.org/news/access-control-allow-origin-header-explained/
     */
    #define CORS_HEADER "Access-Control-Allow-Origin: *\r\n"

    if (!data)
    {
      c->is_closing = 1;
      Modes.stat.HTTP_500_responses++;   /* malloc() failed -> "Internal Server Error" */
      return (500);
    }

    /* This is rather inefficient way to pump data over to the client.
     * Better use a WebSocket instead.
     */
    if (is_extended)
         mg_http_reply (c, 200, CORS_HEADER, data);
    else mg_http_reply (c, 200, CORS_HEADER MODES_CONTENT_TYPE_JSON "\r\n", data);
    free (data);
    return (200);
  }

  dot = strrchr (uri, '.');
  if (dot)
  {
    if (!stricmp(uri, "/favicon.png"))
       return send_file_favicon (c, cli, true);

    if (!stricmp(uri, "/favicon.ico"))   /* Some browsers may want a 'favicon.ico' file */
       return send_file_favicon (c, cli, false);

    return send_file (c, cli, hm, uri, content_type);
  }

  mg_http_reply (c, 404, set_headers(cli, NULL), "Not found\n");
  DEBUG (DEBUG_NET, "Unhandled URI '%.20s' (conn-id: %lu).\n", uri, c->id);
  return (404);
}

/**
 * \todo
 * The event handler for WebSocket control messages.
 */
static int net_handler_websocket (mg_connection *c, const mg_ws_message *ws, int ev)
{
  mg_host_name addr_buf;
  const char  *remote = net_str_addr (&c->rem, addr_buf, sizeof(addr_buf));

  DEBUG (DEBUG_NET, "%s from %s has %zd bytes for us. is_websocket: %d.\n",
         event_name(ev), remote, c->recv.len, c->is_websocket);

  if (!c->is_websocket)
     return (0);

  if (ev == MG_EV_WS_OPEN)
  {
    DEBUG (DEBUG_MONGOOSE2, "WebSock open from conn-id: %lu:\n", c->id);
    HEX_DUMP (ws->data.buf, ws->data.len);
  }
  else if (ev == MG_EV_WS_MSG)
  {
    DEBUG (DEBUG_MONGOOSE2, "WebSock message from conn-id: %lu:\n", c->id);
    HEX_DUMP (ws->data.buf, ws->data.len);
  }
  else if (ev == MG_EV_WS_CTL)
  {
    DEBUG (DEBUG_MONGOOSE2, "WebSock control from conn-id: %lu:\n", c->id);
    HEX_DUMP (ws->data.buf, ws->data.len);
    Modes.stat.HTTP_websockets++;
  }
  return (1);
}

/**
 * The timer callback for an active `connect()`.
 * Or for data-timeout in `MODES_NET_SERVICE_RTL_TCP` service.
 */
static void net_timeout (const timeout_data *td)
{
  char        err [200];
  const char *what = "connection";

  if (td->service == MODES_NET_SERVICE_RTL_TCP)
     what = "data";

  snprintf (err, sizeof(err), "Timeout in %s to host %s (service: \"%s\", tid: %u)",
            what, net_service_url(td->service), net_service_descr(td->service), td->id);
  net_store_error (td->service, err);

  modeS_signal_handler (0);  /* break out of main_data_loop()  */
}

static void net_timer_add (intptr_t service, int timeout_ms, int flag)
{
  ASSERT_SERVICE (service);
  assert (flag == MG_TIMER_ONCE || flag == MG_TIMER_REPEAT);

  if (timeout_ms > 0)
  {
    mg_timer *t = mg_timer_add (&Modes.mgr, timeout_ms, flag,
                                (void (*)(void*)) net_timeout,
                                &service_timers [service]);

    service_timers [service].service = service;
    service_timers [service].id      = (int32_t) t->id;
    service_timers [service].timer   = t;
    DEBUG (DEBUG_NET, "Added timer-id: %ld, %d msec, %s.\n",
           t->id, timeout_ms, flag == MG_TIMER_ONCE ? "MG_TIMER_ONCE" : "MG_TIMER_REPEAT");
  }
  else
  {
    service_timers [service].id    = -1;
    service_timers [service].timer = NULL;
  }
}

static void net_timer_del (intptr_t service)
{
  mg_timer *t;

  ASSERT_SERVICE (service);
  t = service_timers [service].timer;
  if (t)
  {
    timeout_data *td = service_timers + service;

    DEBUG (DEBUG_NET, "Stopping timer-id: %d\n", td->id);
    mg_timer_free (&Modes.mgr.timers, t);
    FREE (service_timers [service].timer);
  }
}

static void net_timer_del_all (void)
{
  intptr_t service;

  for (service = MODES_NET_SERVICE_FIRST; service <= MODES_NET_SERVICE_LAST; service++)
      net_timer_del (service);
}

static bool net_setsockopt (mg_connection *c, int opt, int len)
{
  SOCKET sock;

  if (!c)
     return (false);
  sock = (SOCKET) ((size_t) c->fd);
  if (sock == INVALID_SOCKET)
     return (false);
  return (setsockopt (sock, SOL_SOCKET, opt, (const char*)&len, len) == 0);
}

static char *net_error_details (mg_connection *c, const char *in_out, const void *ev_data)
{
  const char *err = (const char*) ev_data;
  char        orig_err [60] = "";
  const char *wsa_err_str   = "?";
  int         wsa_err_num   = -1;
  SOCKET      sock       = INVALID_SOCKET;
  bool        sock_error = (strnicmp (err, "socket error", 12) == 0);
  bool        http_error = (strnicmp (err, "HTTP parse", 10) == 0);
  bool        get_WSAE   = false;

  static char err_buf [200];
  int         len;
  size_t      left = sizeof(err_buf);
  char       *ptr  = err_buf;

  strncpy (orig_err, err, sizeof(orig_err)-1);

  if (!c)      /* We used modeS_err_get() as ev_data */
  {
    char *end, *bind = strstr (orig_err, "bind: ");
    int   val;

    if (bind)
    {
      val = strtod (bind+6, &end);  /* point to the WSAEx error-number */
      if (end > bind+6)
      {
        wsa_err_num = val;
        *end = '\0';
      }
      orig_err [0] = '\0';
      get_WSAE = true;
    }
  }
  else   /* For a simple "socket error", try to get the true `WSAEx` value on the socket */
  {
    int sz = sizeof (wsa_err_num);

    sock = (SOCKET) ((size_t) c->fd);
    if (sock != INVALID_SOCKET && sock_error && getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&wsa_err_num, &sz) == 0)
       get_WSAE = true;
  }

  if (get_WSAE)
  {
    #define WSA_CASE(e) case e: wsa_err_str = #e; break

    switch (wsa_err_num)
    {
      WSA_CASE (WSAECONNREFUSED);
      WSA_CASE (WSAETIMEDOUT);
      WSA_CASE (WSAECONNRESET);
      WSA_CASE (WSAEADDRINUSE);
      WSA_CASE (WSAENETDOWN);
      WSA_CASE (WSAENETUNREACH);
      WSA_CASE (WSAENETRESET);
      WSA_CASE (WSAECONNABORTED);
      WSA_CASE (WSAEHOSTDOWN);
      WSA_CASE (WSAEHOSTUNREACH);
      WSA_CASE (WSAESTALE);
      WSA_CASE (WSAEREMOTE);
      WSA_CASE (WSAEDISCON);
      WSA_CASE (WSASYSNOTREADY);
      WSA_CASE (WSAHOST_NOT_FOUND);
      WSA_CASE (WSATRY_AGAIN);
      WSA_CASE (WSANO_RECOVERY);
      WSA_CASE (WSANO_DATA);
      WSA_CASE (WSAENOMORE);
      WSA_CASE (WSASYSCALLFAILURE);
      WSA_CASE (WSASERVICE_NOT_FOUND);
      WSA_CASE (WSAEREFUSED);
      case 0:
           wsa_err_str = "0!?";
           break;

      /* plus some more */
    }
    #undef WSA_CASE
  }

  len = snprintf (ptr, left, "%s(sock %d", in_out, (int)sock);
  left -= len;
  ptr  += len;

  if (!http_error && wsa_err_num != 0)
  {
    len = snprintf (ptr, left, ", wsa_err: %d/%s", wsa_err_num, wsa_err_str);
    left -= len;
    ptr  += len;
  }

  if (*orig_err)
  {
    len = snprintf (ptr, left, ", orig_err: '%.30s'", orig_err);
    left -= len;
    ptr  += len;
  }
  *ptr++ = ')';
  *ptr = '\0';
  return (err_buf);
}

/**
 * The function for an active `connect()` failure.
 */
static void connection_failed_active (mg_connection *c, intptr_t service, const void *ev_data)
{
  const char *err = net_error_details (c, "Connection out ", ev_data);

  net_store_error (service, err);
}

/**
 * Handle failure for an `accept()`-ed connection.
 */
static void connection_failed_accepted (mg_connection *c, intptr_t service, const void *ev_data)
{
  connection *conn = connection_get (c, service, true);
  const char *err;

  err = net_error_details (c, "Connection in ", ev_data);
  net_store_error (service, err);
  net_conn_free (conn, service);
}

static void tls_handler (mg_connection *c, const char *host)
{
  struct mg_tls_opts opts;

  memset (&opts, '\0', sizeof(opts));
  if (host)
  {
    opts.ca   = mg_str (s_ca);
    opts.name = mg_url_host (host);
  }
  else
  {
    opts.cert = mg_str (s_ssl_cert);
    opts.key  = mg_str (s_ssl_key);
    opts.skip_verification = true;
  }
  mg_tls_init (c, &opts);
  LOG_FILEONLY ("%s (\"%s\"): conn-id: %lu.\n",
                __FUNCTION__, host ? host : "NULL", c->id);
}

static void tls_error (intptr_t service, const char *err)
{
  net_store_error (service, err);
}

/**
 * The event handler for ALL network I/O.
 */
static void net_handler (mg_connection *c, int ev, void *ev_data)
{
  connection  *conn;
  char        *remote;
  const char  *is_tls = "";
  mg_host_name remote_buf;
  long         bytes;                           /* bytes read or written */
  INT_PTR      service = (INT_PTR) c->fn_data;  /* 'fn_data' is arbitrary user data */

  if (Modes.exit)
     return;

  if (ev == MG_EV_POLL || ev == MG_EV_OPEN)     /* Ignore these events */
     return;

  if (ev == MG_EV_TLS_HS)
  {
    Modes.stat.HTTP_tls_handshakes++;
    return;
  }

  if (ev == MG_EV_ERROR)
  {
    remote = modeS_net_services [service].host;

    if (c->tls)
       net_store_error (service, ev_data);

    if (service >= MODES_NET_SERVICE_FIRST && service <= MODES_NET_SERVICE_LAST)
    {
      if (c->is_accepted)
      {
        connection_failed_accepted (c, service, ev_data);
        /* not fatal that a client goes away */
      }
      else if (remote)
      {
        connection_failed_active (c, service, ev_data);
        net_timer_del (service);
        modeS_signal_handler (0);   /* break out of main_data_loop()  */
      }
    }
    return;
  }

  remote = net_str_addr (&c->rem, remote_buf, sizeof(remote_buf));

  if (ev == MG_EV_RESOLVE)
  {
    DEBUG (DEBUG_NET, "MG_EV_RESOLVE: address %s (service: \"%s\")\n",
           remote, net_service_url(service));
    return;
  }

  if (ev == MG_EV_CONNECT)
  {
    conn = calloc (sizeof(*conn), 1);
    if (!conn)
    {
      c->is_closing = 1;
      return;
    }

    conn->c       = c;      /* Keep a copy of the active connection */
    conn->id      = c->id;
    conn->rem     = c->rem;
    conn->service = service;
    strcpy (conn->rem_buf, remote_buf);

    if (Modes.https_enable && service == MODES_NET_SERVICE_HTTP)
    {
      tls_handler (c, remote);
      is_tls = ", TLS";
    }

    DEBUG (DEBUG_NET, "Connected to host %s (service \"%s\"%s).\n",
           remote, net_service_descr(service), is_tls);
    net_timer_del (service);

    if (service == MODES_NET_SERVICE_RTL_TCP && !rtl_tcp_connect(c))
       return;

    LIST_ADD_TAIL (connection, &Modes.connections [service], conn);

    ++ (*net_num_connections (service));  /* should never go above 1 */
    Modes.stat.srv_connected [service]++;
    return;
  }

  if (ev == MG_EV_ACCEPT)
  {
    if (!client_handler(c, service, MG_EV_ACCEPT))    /* Drop this remote? */
    {
      shutdown ((SOCKET) ((size_t)c->fd), SD_BOTH);
      c->is_closing = 1;
      return;
    }

    conn = calloc (sizeof(*conn), 1);
    if (!conn)
    {
      c->is_closing = 1;
      return;
    }

    conn->c       = c;         /* Keep a copy of the passive (listen) connection */
    conn->id      = c->id;
    conn->rem     = c->rem;
    conn->service = service;
    strcpy (conn->rem_buf, remote_buf);

    LIST_ADD_TAIL (connection, &Modes.connections [service], conn);

    ++ (*net_num_connections (service));
    Modes.stat.cli_accepted [service]++;
    return;
  }

  if (ev == MG_EV_READ)
  {
    bytes = *(const long*) ev_data;
    Modes.stat.bytes_recv [service] += bytes;

    DEBUG (DEBUG_NET2, "%s: %lu bytes from %s (service \"%s\")\n",
           event_name(ev), bytes, remote, net_service_descr(service));

    if (service == MODES_NET_SERVICE_RAW_IN)
    {
      conn = connection_get (c, service, false);
      net_connection_recv (conn, decode_RAW_message, false);

      conn = connection_get (c, service, true);
      net_connection_recv (conn, decode_RAW_message, true);
    }
    else if (service == MODES_NET_SERVICE_SBS_IN)
    {
      conn = connection_get (c, service, true);
      net_connection_recv (conn, decode_SBS_message, true);
    }
    else if (service == MODES_NET_SERVICE_RTL_TCP)
    {
      conn = connection_get (c, service, false);
      net_connection_recv (conn, rtl_tcp_decode, false);
    }
    return;
  }

  if (ev == MG_EV_WRITE)         /* Increment our own send() bytes */
  {
    bytes = *(const long*) ev_data;
    Modes.stat.bytes_sent [service] += bytes;
    DEBUG (DEBUG_NET2, "%s: %ld bytes to %s (\"%s\").\n",
           event_name(ev), bytes, remote, net_service_descr(service));
    return;
  }

  if (ev == MG_EV_CLOSE)
  {
    client_handler (c, service, MG_EV_CLOSE);

    conn = connection_get (c, service, false);
    net_conn_free (conn, service);

    conn = connection_get (c, service, true);
    net_conn_free (conn, service);

    -- (*net_num_connections (service));
    return;
  }

  if (service == MODES_NET_SERVICE_HTTP)
  {
    mg_http_message *hm = ev_data;
    mg_ws_message   *ws = ev_data;
    mg_http_uri      request_uri;
    int              status;

    if (ev == MG_EV_WS_OPEN || ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL)
    {
      status = net_handler_websocket (c, ws, ev);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
      status = net_handler_http (c, hm, request_uri);
      DEBUG (DEBUG_NET2, "HTTP %d for '%.*s' (conn-id: %lu)\n",
             status, (int)hm->uri.len, hm->uri.buf, c->id);
    }
    else
      DEBUG (DEBUG_NET2, "Ignoring HTTP event '%s' (conn-id: %lu)\n",
             event_name(ev), c->id);
  }
}

/**
 * Setup an active connection for a service.
 */
static bool connection_setup_active (intptr_t service, mg_connection **c)
{
  *c = connection_setup (service, false, false);
  if (!*c)
  {
    char *err = net_error_details (NULL, "", modeS_err_get());
#if 0
    net_store_error (service, err);
#else
    LOG_STDERR ("Active socket for %s failed; %s.\n", net_service_descr(service), err);
#endif
    return (false);
  }
  return (true);
}

/**
 * Setup a listen connection for a service.
 */
static bool connection_setup_listen (intptr_t service, mg_connection **c, bool sending)
{
  *c = connection_setup (service, true, sending);
  if (!*c)
  {
    char *err = net_error_details (NULL, "", modeS_err_get());
#if 0
    net_store_error (service, err);
#else
    LOG_STDERR ("Listen socket for \"%s\" failed; %s.\n", net_service_descr(service), err);
#endif
    return (false);
  }
  return (true);
}

/**
 * Free a specific connection, client or server.
 */
static void net_conn_free (connection *this_conn, intptr_t service)
{
  connection  *conn;
  int          is_server = -1;
  ULONG        id = 0;
  mg_host_name addr;

  if (!this_conn)
     return;

  for (conn = Modes.connections[service]; conn; conn = conn->next)
  {
    if (conn != this_conn)
       continue;

    LIST_DELETE (connection, &Modes.connections [service], conn);
    if (this_conn->c->is_accepted)
    {
      Modes.stat.cli_removed [service]++;
      is_server = 0;
    }
    else
    {
      Modes.stat.srv_removed [service]++;
      is_server = 1;
    }
    id = conn->id;
    strcpy (addr, conn->rem_buf);
    free (conn);
    break;
  }

  DEBUG (DEBUG_NET, "Freeing %s at %s (conn-id: %lu, url: %s, service: \"%s\").\n",
         is_server == 1 ? "server" :
         is_server == 0 ? "client" : "?", addr, id,
         net_service_url(service), net_service_descr(service));
}

/**
 * Free all connections in all services.
 */
static uint32_t net_conn_free_all (void)
{
  intptr_t service;
  uint32_t num = 0;

  for (service = MODES_NET_SERVICE_FIRST; service <= MODES_NET_SERVICE_LAST; service++)
  {
    connection *conn, *conn_next;

    for (conn = Modes.connections [service]; conn; conn = conn_next)
    {
      conn_next = conn->next;
      net_conn_free (conn, service);
      num++;
    }
    free (net_service_url(service));
  }
  return (num);
}

static char *net_store_error (intptr_t service, const char *err)
{
  ASSERT_SERVICE (service);

  FREE (modeS_net_services [service].last_err);
  if (err)
     modeS_net_services [service].last_err = strdup (err);

  DEBUG (DEBUG_NET, "%s\n", err);
  return (modeS_net_services [service].last_err);
}

static uint16_t *net_num_connections (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (&modeS_net_services [service].num_connections);
}

static const char *net_service_descr (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (modeS_net_services [service].descr);
}

uint16_t net_handler_port (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (modeS_net_services [service].port);
}

const char *net_handler_protocol (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (modeS_net_services [service].protocol);
}

static char *net_service_url (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (modeS_net_services [service].url);
}

static char *net_service_error (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (modeS_net_services [service].last_err);
}

bool net_handler_sending (intptr_t service)
{
  ASSERT_SERVICE (service);
  return (modeS_net_services [service].active_send);
}

static void net_flush_all (void)
{
  mg_connection *c;
  mg_timer      *t = Modes.mgr.timers;
  uint32_t       num_active  = 0;
  uint32_t       num_passive = 0;
  uint32_t       num_unknown = 0;
  uint32_t       num_timers  = 0;
  size_t         total_rx = 0;
  size_t         total_tx = 0;

  while (t)
  {
    num_timers++;
    t = t->next;
  }

  for (c = Modes.mgr.conns; c; c = c->next)
  {
    total_rx += c->recv.len;
    total_tx += c->send.len;

    mg_iobuf_free (&c->recv);
    mg_iobuf_free (&c->send);

    if (c->is_accepted || c->is_listening)
         num_passive++;
    else if (c->is_client)
         num_active++;
    else num_unknown++;
  }
  DEBUG (DEBUG_NET,
         "Flushed %u active connections, %u passive, %u unknown. Remaining bytes: %zu Rx, %zu Tx. %u timers.\n",
         num_active, num_passive, num_unknown, total_rx, total_tx, num_timers);
}

/**
 * Check if the client `*addr` is unique.
 */
static bool _client_is_unique (const mg_addr *addr, intptr_t service, unique_IP **ipp)
{
  unique_IP *ip;

  *ipp = NULL;

  if (addr->is_ip6 ||               /* check only IPv4 addresses */
      *(uint32_t*) &addr->ip == 0)  /* Ignore 0.0.0.0 */
     return (false);

  for (ip = g_unique_ips; ip; ip = ip->next)
  {
    if (ip->service == service && *(uint32_t*)&ip->addr.ip == *(uint32_t*)&addr->ip)
    {
      ip->accepted++;  /* accept() counter */
      *ipp = ip;
      return (false);
    }
  }

  ip = calloc (sizeof(*ip), 1);  /* assign a new element for this `*addr` */
  if (!ip)
     return (false);             /* cannot tell */

  ip->addr     = *addr;
  ip->service  = service;
  ip->accepted = 1;
  get_FILETIME_now (&ip->seen);
  LIST_ADD_TAIL (unique_IP, &g_unique_ips, ip);
  *ipp = ip;
  return (true);
}

static bool client_is_unique (const mg_addr *addr, intptr_t service, unique_IP **ipp)
{
  bool unique = _client_is_unique (addr, service, ipp);

  if (test_contains(Modes.tests, "net"))
  {
    ip_address ip_addr;

    mg_snprintf (ip_addr, sizeof(ip_addr), "%M", mg_print_ip, addr);

    if (*ipp && (*ipp)->denied > 0)
         printf ("  unique: %d, ip: %-15s denied: %u\n", unique, ip_addr, (*ipp)->denied);
    else printf ("  unique: %d, ip: %s\n", unique, ip_addr);
  }
  return (unique);
}

static int compare_on_ip (const void *_a, const void *_b)
{
  const unique_IP *a = (const unique_IP*) _a;
  const unique_IP *b = (const unique_IP*) _b;

  return memcmp (&a->addr.ip, &b->addr.ip, sizeof(a->addr.ip));
}

/*
 * Print number of unique clients and their addresses sorted.
 *
 * If an IP was denied N times, print as e.g. "a.b.c.d (N)"
 */
static void unique_ips_print (intptr_t service)
{
  const unique_IP *ip;
  size_t           i, num;
  size_t           indent = 12;
  char            *buf = NULL;
  FILE            *save = Modes.log;

  LOG_STDOUT ("    %8llu unique client(s):\n", Modes.stat.unique_clients [service]);

  if (test_contains(Modes.tests, "net"))
       Modes.log = stdout;
  else indent += strlen ("HH:MM:SS.MMM:");

  if (!Modes.log)
     return;

  for (num = 0, ip = g_unique_ips; ip; ip = ip->next)
      if (ip->service == service)
         num++;

  if (num > 0)
  {
    unique_IP *_ip, *start = calloc (sizeof(*start), num);

    if (!start)
       return;

    for (ip = g_unique_ips, _ip = start; ip; ip = ip->next, _ip++)
        if (ip->service == service)
           memcpy (_ip, ip, sizeof(*_ip));

    qsort (start, num, sizeof(*start), compare_on_ip);

    for (i = 0, _ip = start; i < num; _ip++, i++)
    {
      ip_address ip_addr;

      mg_snprintf (ip_addr, sizeof(ip_addr), "%M", mg_print_ip, _ip->addr);
      modeS_asprintf (&buf, "%s", ip_addr);

      if (_ip->denied > 0)
           modeS_asprintf (&buf, " (%u), ", _ip->denied);
      else modeS_asprintf (&buf, ", ");
    }
    free (start);
  }

  if (num == 0)
       modeS_asprintf (&buf, "None!?");
  else *(strrchr (buf, '\0') - 2) = '\0';      /* remove last "," */

  fprintf (Modes.log, "%*s", (int)indent, " ");
  fputs_long_line (Modes.log, buf, indent);
  if (Modes.debug & DEBUG_NET)
  {
    fprintf (stdout, "%*s", (int)indent, " ");
    fputs_long_line (stdout, buf, indent);
  }
  free (buf);
  Modes.log = save;
}

static void unique_ips_free (void)
{
  unique_IP *ip, *ip_next;

  for (ip = g_unique_ips; ip; ip = ip_next)
  {
    ip_next = ip->next;
    free (ip);
  }
  g_unique_ips = NULL;
}

static bool add_deny (const char *val, bool is_ip6)
{
  deny_element *deny = calloc (sizeof(*deny), 1);

  if (deny)
  {
    strncpy (deny->acl, val, sizeof(deny->acl)-1);
    deny->is_ip6 = is_ip6;
    LIST_ADD_TAIL (deny_element, &g_deny_list, deny);
  }
  return (true);
}

/**
 * Callbacks from cfg_file.c
 */
bool net_deny4 (const char *val)
{
  return add_deny (val, false);
}

bool net_deny6 (const char *val)
{
  return add_deny (val, true);
}

/**
 * Loop over `g_deny_list` to check if client `*addr` should be denied.
 * `mg_check_ip_acl()` accepts netmasks.
 */
static bool client_deny (const mg_addr *addr, int *rc)
{
  const deny_element *d;
  int   dummy_rc;

  if (!rc)
     rc = &dummy_rc;

  *rc = -3;      /* unknown */

  for (d = g_deny_list; d; d = d->next)
  {
    if (d->is_ip6)    /* Mongoose does not support IPv6 here yet */
       continue;

    *rc = mg_check_ip_acl (mg_str(d->acl), (struct mg_addr*)addr);
    if (*rc == 1)
       return (true);
  }
  return (false);
}

static size_t deny_list_dump (bool is_ip6)
{
  const deny_element *d;
  size_t              num = 0;

  for (d = g_deny_list; d; d = d->next)
  {
    if (d->is_ip6 == is_ip6)
    {
      printf ("  Added deny ACL: %s.\n", d->acl);
      num++;
    }
  }
  return (num);
}

static void deny_lists_dump (void)
{
  size_t num4, num6;

  printf ("\n%s():\n", __FUNCTION__);
  num4 = deny_list_dump (false);
  num6 = deny_list_dump (true);
  printf ("  num4 ACL: %zu, num6 ACL: %zu.\n", num4, num6);
}

static void deny_lists_test (void)
{
  mg_addr *a, addr [7];
  mg_str   str;
  size_t   i;

  printf ("\n%s():\n", __FUNCTION__);

#define DENY_LIST_ADD(dest, value) \
        str = mg_str (value);      \
        mg_aton (str, dest);       \
        net_deny4 (value)

  memset (&addr, '\0', sizeof(addr));

  DENY_LIST_ADD (addr+0, "127.0.0.1");
  DENY_LIST_ADD (addr+1, "127.0.0.2");
  DENY_LIST_ADD (addr+2, "127.0.1.0");
  DENY_LIST_ADD (addr+3, "127.0.1.1");
  DENY_LIST_ADD (addr+4, "127.0.1.2");
  DENY_LIST_ADD (addr+5, HOSTILE_IP_1);
  DENY_LIST_ADD (addr+6, HOSTILE_IP_2);

  a = addr + 0;
  for (i = 0; i < DIM(addr); i++, a++)
  {
    char abuf [30];
    int  rc;
    bool deny = client_deny (a, &rc);

    mg_snprintf (abuf, sizeof(abuf), "%M", mg_print_ip, a);
    printf ("  rc: %d, addr[%zu]: %s -> %s\n",
            rc, i, abuf, deny ? "denied" : "accepted");
  }
}

static void deny_lists_tests (void)
{
  deny_lists_dump();
  deny_lists_test();
}

static void deny_list_free (void)
{
  deny_element *d, *d_next;

  for (d = g_deny_list; d; d = d_next)
  {
    d_next = d->next;
    free (d);
  }
  g_deny_list = NULL;
}

static size_t deny_list_numX (bool is_ip6)
{
  const deny_element *d;
  size_t              num = 0;

  for (d = g_deny_list; d; d = d->next)
      if (d->is_ip6 == is_ip6)
         num++;
  return (num);
}

static size_t deny_list_num4 (void)
{
  return deny_list_numX (false);
}

static size_t deny_list_num6 (void)
{
  return deny_list_numX (true);
}

static bool client_is_extern (const mg_addr *addr)
{
  const struct in_addr *ia;

  if (addr->is_ip6)
     return (IN6_IS_ADDR_LOOPBACK ((const IN6_ADDR*)&addr->ip) == false);

  ia = (const struct in_addr*) &addr->ip;
  return (ia->s_net != 0 && ia->s_net != 127);   /* not 0.0.0.0 and not 127.x.y.z */
}

static bool client_handler (mg_connection *c, intptr_t service, int ev)
{
  const mg_addr *addr = &c->rem;
  const char    *is_tls = "";
  mg_host_name   addr_buf;
  unique_IP     *unique;

  assert (ev == MG_EV_ACCEPT || ev == MG_EV_CLOSE);

  if (Modes.debug & DEBUG_MONGOOSE2)
     c->is_hexdumping = 1;

  if (ev == MG_EV_ACCEPT)
  {
    if (client_is_unique(addr, service, &unique))  /* Have we seen this IPv4-address before? */
       Modes.stat.unique_clients [service]++;

    if (Modes.https_enable && service == MODES_NET_SERVICE_HTTP)
    {
      tls_handler (c, NULL);
      is_tls = ", TLS";
    }

    if (client_is_extern(addr))           /* Not from 127.0.0.1 */
    {
      bool deny = client_deny (addr, NULL);

      if (deny && unique)  /* increment deny-counter for this `addr` */
         unique->denied++;

      net_str_addr (addr, addr_buf, sizeof(addr_buf));

      if (Modes.speech_enable && Modes.speech_clients)
      {
        speak_string ("Client for %s %s.",
                      net_service_descr(service),
                      deny ? "denied" : "accepted");
      }
      else if (Modes.debug & DEBUG_NET)
      {
        Beep (deny ? 1200 : 800, 20);
      }

      LOG_FILEONLY ("%s connection: %s (conn-id: %lu, service: \"%s\"%s).\n",
                    deny ? "Denied" : "Accepted",
                    addr_buf, c->id, net_service_descr(service), is_tls);
      return (!deny);
    }
  }

  if (client_is_extern(addr))
  {
    LOG_FILEONLY ("Closing connection: %s (conn-id: %lu, service: \"%s\").\n",
                  net_str_addr(addr, addr_buf, sizeof(addr_buf)), c->id, net_service_descr(service));
  }
  return (true);   /* ret-val ignored for MG_EV_CLOSE */
}

/**
 * Since 'mg_straddr()' was removed in latest version.
 * Optionally print the hostname if found in `/etc/hosts` or
 * DNS cache.
 */
static char *net_str_addr (const mg_addr *a, char *buf, size_t len)
{
  const char *h_name = NULL;
  size_t      h_len = len - 7;  /* make room for ":port" */
  size_t      mg_len;

  if (Modes.show_host_name)
  {
    const struct hostent *h = gethostbyaddr ((char*)&a->ip, sizeof(a->ip), AF_INET);
    h_name = h ? h->h_name : NULL;
  }

  if (h_name)
       mg_len = mg_snprintf (buf, h_len, "%s", h_name);
  else mg_len = mg_snprintf (buf, h_len, "%M", mg_print_ip, a);

  mg_snprintf (buf + mg_len, len - mg_len, ":%hu", mg_ntohs(a->port));
  return (buf);
}

/**
 * Parse and split a `[udp://|tcp://]host[:port]` string into a host and port.
 * Set default port if the `:port` is missing.
 */
bool net_set_host_port (const char *host_port, net_service *serv, uint16_t def_port)
{
  mg_str       str;
  mg_addr      addr;
  mg_host_name name;
  bool         is_udp = false;
  int          is_ip6 = -1;

  if (!strnicmp("tcp://", host_port, 6))
  {
    host_port += 6;
  }
  else if (!strnicmp("udp://", host_port, 6))
  {
    is_udp = true;
    host_port += 6;
  }

  str = mg_url_host (host_port);
  memset (&addr, '\0', sizeof(addr));
  addr.port = mg_url_port (host_port);
  mg_aton (str, &addr);
  is_ip6 = addr.is_ip6;
  snprintf (name, sizeof(name), "%.*s", (int)str.len, str.buf);

  if (addr.port == 0)
     addr.port = def_port;

  DEBUG (DEBUG_NET, "host_port: '%s', name: '%s', addr.port: %u\n",
         host_port, name, addr.port);

  if (is_ip6 == -1 && strstr(host_port, "::"))
  {
    printf ("Illegal address: '%s'. Try '[::ffff:a.b.c.d]:port' instead.\n", host_port);
    return (false);
  }

  strcpy (serv->host, name);
  serv->port       = addr.port;
  serv->is_udp     = (is_udp == true);
  serv->is_ip6     = (is_ip6 == 1);
  DEBUG (DEBUG_NET, "is_ip6: %d, host: %s, port: %u.\n", is_ip6, serv->host, serv->port);
  return (true);
}

/**
 * Functions for loading `web-pages.dll;[1-9]`.
 *
 * If program called with `--web-page web-pages.dll;1` for the 1st resource,
 * load all `mg_*_1()` functions dynamically. Similar for `--web-page web-pages.dll;2`.
 *
 * But if program was called with `--web-page foo/index.html`, simply skip the loading
 * of the .DLL and check the regular Web-page `foo/index.html`.
 */
#if defined(USE_PACKED_DLL)
  #define ADD_FUNC(func) { false, NULL, "", #func, (void**)&p_##func }
                        /* ^__ no functions are optional */

  static struct dyn_struct web_page_funcs [] = {
                           ADD_FUNC (mg_unpack),
                           ADD_FUNC (mg_unlist)
                         };
  static char *web_funcs [DIM(web_page_funcs)];

  static bool load_web_dll (char *web_dll)
  {
    char  *res_ptr;
    size_t num;
    int    missing, resource = -1;
    bool   rc;
    struct stat st;

    res_ptr = strchr (web_dll, ';');
    if (!res_ptr || !isdigit(res_ptr[1]))
    {
      LOG_STDERR ("The web-page \"%s\" has no resource number!\n", Modes.web_page);
      return (false);
    }

    *res_ptr++ = '\0';
    resource = (*res_ptr - '0');

    if (!str_endswith(web_dll, ".dll"))
    {
      LOG_STDERR ("The web-page \"%s\" is not a .DLL!\n", Modes.web_page);
      return (false);
    }

    errno = 0;
    if (stat(web_dll, &st) != 0)
    {
      LOG_STDERR ("The web-page \"%s\" does not exist. errno: %d\n", web_dll, errno);
      return (false);
    }

    LOG_STDOUT ("Trying '--web-page \"%s\"' and resource %d.\n", web_dll, resource);
    for (num = 0; num < DIM(web_page_funcs); num++)
    {
      web_page_funcs [num].mod_name  = web_dll;
      web_page_funcs [num].func_name = mg_mprintf ("%s_%d", web_page_funcs [num].func_name, resource);
      if (!web_page_funcs [num].func_name)
      {
        LOG_STDERR ("Memory alloc for the web-page \"%s\" failed!.\n", web_dll);
        return (false);
      }
      web_funcs [num] = (char*) web_page_funcs [num].func_name;
    }

    rc = true;
    missing = (num - load_dynamic_table(web_page_funcs, num));

    if (!web_page_funcs [0].mod_handle || web_page_funcs [0].mod_handle == INVALID_HANDLE_VALUE)
    {
      LOG_STDERR ("The web-page \"%s\" failed to load; %s.\n", web_dll, win_strerror(GetLastError()));
      rc = false;
    }
    else if (missing)
    {
      LOG_STDERR ("The web-page \"%s\" is missing %d functions.\n", web_dll, missing);
      rc = false;
    }
    return (rc);
  }

  /*
   * Free the memory allocated above. And unload the .DLL.
   */
  static void unload_web_dll (void)
  {
    size_t i;

    if (!use_packed_dll)
       return;

    for (i = 0; i < DIM(web_funcs); i++)
       free (web_funcs [i]);
    unload_dynamic_table (web_page_funcs, DIM(web_page_funcs));
  }

  static void touch_web_dll (void)
  {
    /** todo */
  }

  static int compare_on_name (const void *_a, const void *_b)
  {
    const file_packed *a = (const file_packed*) _a;
    const file_packed *b = (const file_packed*) _b;
    int   rc = strcmp (a->name, b->name);

    num_lookups++;
    if (rc)
       num_misses++;
    return (rc);
  }

  static size_t count_packed_fs (bool *have_index_html)
  {
    const char *fname;
    size_t      num, fsize;

    *have_index_html = false;
    DEBUG (DEBUG_NET2, "%s():\n", __FUNCTION__);

    for (num = 0; (fname = (*p_mg_unlist) (num)) != NULL; num++)
    {
      (*p_mg_unpack) (fname, &fsize, NULL);
      DEBUG (DEBUG_NET2, "  %-50s -> %7zu bytes\n", fname, fsize);
      if (*have_index_html == false && !strcmp(basename(fname), "index.html"))
         *have_index_html = true;
    }

    if (*have_index_html)
       strcpy (Modes.web_page, "index.html");
    return (num);
  }

  static bool check_packed_web_page (void)
  {
    bool   have_index_html;
    size_t num;

    if (!use_packed_dll)
       return (true);

    num = count_packed_fs (&have_index_html);
    if (num == 0)
    {
      LOG_STDERR ("The \"%s\" has no files!\n", Modes.web_page);
      return (false);
    }

    if (!have_index_html)
    {
      LOG_STDERR ("The \"%s\" has no \"index.html\" file!\n", Modes.web_page);
      return (false);
    }

    /* Create a sorted list for a bsearch() lookup in 'mg_unpack()' below
     */
    if (use_bsearch)
    {
      const char *fname;

      lookup_table    = malloc (sizeof(*lookup_table) * num);
      lookup_table_sz = num;
      if (!lookup_table)
      {
        use_bsearch = false;
        return (true);
      }
      for (num = 0; (fname = (*p_mg_unlist)(num)) != NULL; num++)
      {
        lookup_table [num].name = fname;
        lookup_table [num].data = (const unsigned char*) (*p_mg_unpack) (fname, &lookup_table [num].size, &lookup_table [num].mtime);
      }
      qsort (lookup_table, num, sizeof(*lookup_table), compare_on_name);
    }
    return (true);
  }

  /*
   * This is called from 'externals/mongoose.c' when using a packed File-system.
   * I.e. when `opts.fs = &mg_fs_packed;` above.
   */
  const char *mg_unpack (const char *fname, size_t *fsize, time_t *ftime)
  {
    const file_packed *p;
    file_packed        key;

    if (!use_bsearch)
       return (*p_mg_unpack) (fname, fsize, ftime);

    key.name = fname;
    p = bsearch (&key, lookup_table, lookup_table_sz, sizeof(*lookup_table), compare_on_name);

    if (fsize)
       *fsize = (p ? p->size - 1 : 0);
    if (ftime)
       *ftime = (p ? p->mtime : 0);

    if (ftime == NULL &&     /* When called from 'packed_open()' */
        !str_endswith(fname, ".gz"))
    {
      LOG_FILEONLY ("found: %d, lookups: %u/%u, fname: '%s'\n", p ? 1 : 0, num_lookups, num_misses, fname);
      num_lookups = num_misses = 0;
    }
    return (p ? (const char*)p->data : NULL);
  }

  const char *mg_unlist (size_t i)
  {
    return (*p_mg_unlist) (i);
  }

#else
  static bool check_packed_web_page (void)
  {
    use_packed_dll = false;
    return (true);
  }
#endif  /* USE_PACKED_DLL */

/*
 * Check a regular Web-page
 */
static bool check_web_page (void)
{
  mg_file_path full_name;
  struct stat  st;

  snprintf (full_name, sizeof(full_name), "%s/%s", Modes.web_root, Modes.web_page);
  DEBUG (DEBUG_NET, "Web-page: \"%s\"\n", full_name);

  if (stat(full_name, &st) != 0)
  {
    LOG_STDERR ("Web-page \"%s\" does not exist.\n", full_name);
    return (false);
  }
  if (((st.st_mode) & _S_IFMT) != _S_IFREG)
  {
    LOG_STDERR ("Web-page \"%s\" is not a regular file.\n", full_name);
    return (false);
  }
  return (true);
}

static int net_show_server_errors (void)
{
  int service, num = 0;

  for (service = MODES_NET_SERVICE_FIRST; service <= MODES_NET_SERVICE_LAST; service++)
  {
    const char *err = net_service_error (service);

    if (!err)
       continue;
    LOG_STDOUT ("  %s: %s.\n", net_service_descr(service), err);
    net_store_error (service, NULL);
    num++;
  }
  return (num);
}

static bool show_raw_common (int s)
{
  const char *url = net_service_url (s);

  LOG_STDOUT ("  %s (%s):\n", net_service_descr(s), url ? url : "none");

  if (Modes.stat.bytes_recv [s] == 0)
  {
    LOG_STDOUT ("    nothing.\n");
    return (false);
  }
  LOG_STDOUT ("    %s bytes.\n", qword_str(Modes.stat.bytes_recv [s]));
  return (true);
}

/*
 * Show decoder statistics for a RAW_IN service.
 * Only if we had a connection with such a server.
 */
static void show_raw_RAW_IN_stats (void)
{
  if (show_raw_common(MODES_NET_SERVICE_RAW_IN))
  {
    LOG_STDOUT ("  %8llu good messages.\n", Modes.stat.RAW_good);
    LOG_STDOUT ("  %8llu empty messages.\n", Modes.stat.RAW_empty);
    LOG_STDOUT ("  %8llu unrecognized messages.\n", Modes.stat.RAW_unrecognized);
  }
}

static void show_raw_SBS_IN_stats (void)
{
  if (show_raw_common(MODES_NET_SERVICE_SBS_IN))
  {
    LOG_STDOUT ("  %8llu good messages.\n", Modes.stat.SBS_good);
    LOG_STDOUT ("  %8llu MSG messages.\n", Modes.stat.SBS_MSG_msg);
    LOG_STDOUT ("  %8llu AIR messages.\n", Modes.stat.SBS_AIR_msg);
    LOG_STDOUT ("  %8llu STA messages.\n", Modes.stat.SBS_STA_msg);
    LOG_STDOUT ("  %8llu unrecognized messages.\n", Modes.stat.SBS_unrecognized);
  }
}

static void show_rtl_tcp_IN_stats (void)
{
  if (show_raw_common(MODES_NET_SERVICE_RTL_TCP))
  {
//  LOG_STDOUT ("  %8llu packets.\n", Modes.stat.rtltcp.packets);
  }
}

void net_show_stats (void)
{
  int s;

  LOG_STDOUT ("\nNetwork statistics:\n");

  for (s = MODES_NET_SERVICE_FIRST; s <= MODES_NET_SERVICE_LAST; s++)
  {
    const char *url = net_service_url (s);
    uint64_t    sum;

    if (s == MODES_NET_SERVICE_RAW_IN ||  /* These are printed separately */
        s == MODES_NET_SERVICE_SBS_IN ||
        s == MODES_NET_SERVICE_RTL_TCP)
       continue;

    LOG_STDOUT ("  %s (%s):\n", net_service_descr(s), url ? url : "none");

    if (Modes.net_active)
         sum = Modes.stat.srv_connected [s] + Modes.stat.srv_removed [s] + Modes.stat.srv_unknown [s];
    else sum = Modes.stat.cli_accepted [s]  + Modes.stat.cli_removed [s] + Modes.stat.cli_unknown [s];

    sum += Modes.stat.bytes_sent [s] + Modes.stat.bytes_recv [s] + *net_num_connections (s);
    if (sum == 0ULL)
    {
      LOG_STDOUT ("    Nothing.\n");
      continue;
    }

    LOG_STDOUT ("    %8llu bytes sent.\n", Modes.stat.bytes_sent [s]);
    LOG_STDOUT ("    %8llu bytes recv.\n", Modes.stat.bytes_recv [s]);

    if (s == MODES_NET_SERVICE_HTTP)
    {
      LOG_STDOUT ("    %8llu HTTP GET requests received.\n", Modes.stat.HTTP_get_requests);
      LOG_STDOUT ("    %8llu HTTP 400 replies sent.\n", Modes.stat.HTTP_400_responses);
      LOG_STDOUT ("    %8llu HTTP 404 replies sent.\n", Modes.stat.HTTP_404_responses);
      LOG_STDOUT ("    %8llu HTTP/WebSocket upgrades.\n", Modes.stat.HTTP_websockets);
      LOG_STDOUT ("    %8llu HTP/TLS handshakes.\n", Modes.stat.HTTP_tls_handshakes);
      LOG_STDOUT ("    %8llu server connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_sent);
      LOG_STDOUT ("    %8llu client connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_recv);
    }

    if (Modes.net_active)
    {
      LOG_STDOUT ("    %8llu server connections done.\n", Modes.stat.srv_connected [s]);
      LOG_STDOUT ("    %8llu server connections removed.\n", Modes.stat.srv_removed [s]);
      LOG_STDOUT ("    %8llu server connections unknown.\n", Modes.stat.srv_unknown [s]);
      LOG_STDOUT ("    %8u server connections now.\n", *net_num_connections(s));
    }
    else
    {
      LOG_STDOUT ("    %8llu client connections accepted.\n", Modes.stat.cli_accepted [s]);
      LOG_STDOUT ("    %8llu client connections removed.\n", Modes.stat.cli_removed [s]);
      LOG_STDOUT ("    %8llu client connections unknown.\n", Modes.stat.cli_unknown [s]);
      LOG_STDOUT ("    %8u client(s) now.\n", *net_num_connections(s));
    }
    unique_ips_print (s);
  }

  EnterCriticalSection (&Modes.print_mutex);
  show_raw_SBS_IN_stats();
  show_raw_RAW_IN_stats();
  show_rtl_tcp_IN_stats();

  net_show_server_errors();
  LeaveCriticalSection (&Modes.print_mutex);
}

static void unique_ip_add_hostile (const char *ip_str, int service)
{
  mg_addr    addr;
  unique_IP *ip;
  mg_str     str;

  DENY_LIST_ADD (&addr, ip_str);

  if (client_is_unique(&addr, service, &ip))
     Modes.stat.unique_clients [service]++;
  ip->denied++;
}

/**
 * Test `g_unique_ips` by filling it with 2 hostile and
 * 50 random IPv4 addresses.
 */
static void unique_ip_tests (void)
{
  const unique_IP *ip;
  unique_IP       *ip2;
  int              i, service = MODES_NET_SERVICE_HTTP;
  uint64_t         num;
  mg_addr          addr;

  printf ("\n%s():\n", __FUNCTION__);
  memset (&addr, '\0', sizeof(addr));

  unique_ip_add_hostile (HOSTILE_IP_1, service);
  unique_ip_add_hostile (HOSTILE_IP_2, service);

  *(uint32_t*) &addr.ip = htonl (INADDR_LOOPBACK);   /* == 127.0.0.1 */
  if (client_is_unique(&addr, service, &ip2))
     Modes.stat.unique_clients [service]++;

  *(uint32_t*) &addr.ip = htonl (INADDR_LOOPBACK+1); /* == 127.0.0.2 */
  if (client_is_unique(&addr, service, &ip2))
     Modes.stat.unique_clients [service]++;

  for (i = 0; i < 50; i++)
  {
    mg_random (&addr.ip, sizeof(addr.ip));
    if (client_is_unique(&addr, service, &ip2))
       Modes.stat.unique_clients [service]++;
    Modes.stat.bytes_recv [service] += 10;
  }

  *(uint32_t*) &addr.ip = htonl (INADDR_LOOPBACK);    /* == 127.0.0.1 */
  if (client_is_unique(&addr, service, &ip2))
     Modes.stat.unique_clients [service]++;

  *(uint32_t*) &addr.ip = htonl (INADDR_LOOPBACK+1);  /* == 127.0.0.2 */
  if (client_is_unique(&addr, service, &ip2))
     Modes.stat.unique_clients [service]++;

  for (num = 0, ip = g_unique_ips; ip; ip = ip->next)
      num++;
  assert (num == Modes.stat.unique_clients [service]);
}

/**
 * Find the DNSv4 and DNSv6 server address(es).
 * Use Windows's IPHelper API.
 */
static bool net_init_dns (char **dns4_p, char **dns6_p)
{
  FIXED_INFO     *fi = alloca (sizeof(*fi));
  DWORD           size = 0;
  IP_ADDR_STRING *ip;
  FILE           *f = NULL;
  int             i;
  const char     *ping6_cmd = "ping.exe -6 -n 1 ipv6.google.com 2> NUL";
  char            ping6_buf [500];
  char            ping6_addr[50];

  *dns4_p = NULL;
  *dns6_p = NULL;

  if (GetNetworkParams(fi, &size) != ERROR_BUFFER_OVERFLOW)
  {
    LOG_STDERR ("  error: %s\n", win_strerror(GetLastError()));
    return (false);
  }
  fi = alloca (size);
  if (GetNetworkParams(fi, &size) != ERROR_SUCCESS)
  {
    LOG_STDERR ("  error: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  DEBUG (DEBUG_NET, "  Host Name:   %s\n", fi->HostName);
  DEBUG (DEBUG_NET, "  Domain Name: %s\n", fi->DomainName [0] ? fi->DomainName : "<None>");
  DEBUG (DEBUG_NET, "  Node Type:   %u\n", fi->NodeType);
  DEBUG (DEBUG_NET, "  DHCP scope:  %s\n", fi->ScopeId [0] ? fi->ScopeId : "<None>");
  DEBUG (DEBUG_NET, "  Routing:     %s\n", fi->EnableRouting ? "Enabled" : "Disabled");
  DEBUG (DEBUG_NET, "  ARP proxy:   %s\n", fi->EnableProxy   ? "Enabled" : "Disabled");
  DEBUG (DEBUG_NET, "  DNS enabled: %s\n", fi->EnableDns     ? "Yes"     : "No");
  DEBUG (DEBUG_NET, "  DNS Servers: %-15s (primary)\n", fi->DnsServerList.IpAddress.String);

  for (i = 1, ip = fi->DnsServerList.Next; ip; ip = ip->Next, i++)
     DEBUG (DEBUG_NET, "               %-15s (secondary %d)\n%s",
            ip->IpAddress.String, i, ip->Next ? "" : "\n");

  /* Return a malloced string of the primary DNS server
   */
  *dns4_p = mg_mprintf ("udp://%s:53", fi->DnsServerList.IpAddress.String);

  /*
   * Fake alert:
   *  If a `system ("ping.exe -6 -n 1 ipv6.google.com")` works, just assume that
   *  the `Reply from <ping6_addr> time=zz sec' will work as the DNS6 address.
   * Note:
   *   `ipv6.google.com` does not have IPv4 address, only IPv6, therefore
   *   it is guaranteed to hit IPv6 resolution path.
   */
  _set_errno (0);
  f = _popen (ping6_cmd, "r");
  if (!f)
  {
    DEBUG (DEBUG_NET, "_popen() failed: errno: %d/%s\n", errno, strerror(errno));
    return (true);
  }

  while (fgets(ping6_buf, sizeof(ping6_buf)-1, f))
  {
    str_rtrim (ping6_buf);
    if (!ping6_buf[0] || ping6_buf[0] == '\n')
       continue;

    DEBUG (DEBUG_NET, "_popen(): '%s'\n", ping6_buf);
    if (sscanf(ping6_buf, "Reply from %s", ping6_addr) == 1)
    {
      DEBUG (DEBUG_NET, "ping6_addr: '%s'\n", ping6_addr);
      *dns6_p = strdup (ping6_addr);
      break;
    }
  }

  if (f)
     _pclose (f);
  return (true);
}

/**
 * Initialize all network stuff:
 *  \li Allocate the list for unique IPs.
 *  \li Load and check the `web-pages.dll`.
 *  \li Initialize the Mongoose network manager.
 *  \li Set the default DNSv4 server address from Windows' IPHelper API.
 *  \li Start the active service RTL_TCP (or rename it to "RTL-UDP").
 *  \li Start the 2 active network services (RAW_IN + SBS_IN).
 *  \li Or start the 4 listening (passive) network services.
 *  \li If HTTP-server is enabled, check the precence of the Web-page.
 *  \li If `--test` was used, do some tests.
 */
bool net_init (void)
{
  mg_file_path web_dll;

  snprintf (web_dll, sizeof(web_dll), "%s/%s", Modes.web_root, Modes.web_page);
  strlwr (web_dll);

  if (strstr(web_dll, ".dll;"))
     use_packed_dll = true;

  if (use_packed_dll && !Modes.net_active)
  {
#if defined(USE_PACKED_DLL)
    if (!load_web_dll(web_dll))
    {
      use_packed_dll = false;
      return (false);
    }
#else
    LOG_STDERR ("Using a .DLL when built without 'USE_PACKED_DLL' is not possible.\n"
                "Disable the \"web-page = XX.dll;y\" setting in '%s'.\n", Modes.cfg_file);
    return (false);
#endif
  }

  if (Modes.web_root_touch)
  {
#if defined(USE_PACKED_DLL)
    touch_web_dll();
#endif
#if MG_ENABLE_FILE
    touch_dir (Modes.web_root, true);
#endif
  }

  mg_mgr_init (&Modes.mgr);

#if defined(__DOXYGEN__) || 0
  mg_wakeup_init (&Modes.mgr);

  /**
   * \todo
   * Replace some (or all?) of `background_tasks()` with `background_tasks_thread()`.
   * Ref: https://mongoose.ws/documentation/tutorials/core/multi-threaded/
   */
  int background_tasks_thread (void *arg)
  {
    thread_data *t = arg;

    while (!Modes.exit)
    {
      Sleep (MODES_INTERACTIVE_REFRESH_TIME);
      mg_wakeup (t->mgr, t->conn_id, &t->task_num, sizeof(t->task_num));
      if (++t->task_num > t->task_max)
         t->task_num = 0;
    }
  }
  /* And in net_handler() above
   */
  if (ev == MG_EV_WAKEUP)
  {
    const thread_data *t = ev_data;

    switch (t->task_num)
    {
      case 0:
           interactive_show_data (now);
           break;
      case 1:
           location_poll (&pos);
           break;
      case 2:
           aircraft_remove_stale (now);
           break;
      case 3:
           airports_background (now);
           break;
      case 4:
           interactive_title_stats();
           interactive_update_gain();
           interactive_other_stats();
           break;
       default:
           assert (0);
           break;
    }
  }

  /* Initialize the above idea:
   */
  unsigned    wakeup_tid;
  uintptr_t   wakeup_hnd;
  thread_data t;

  t.task_num = 0;
  t.task_max = 4;
  wakeup_hnd = _beginthreadex (NULL, 0, background_tasks_thread, &t, 0, &wakeup_tid);
#endif

  net_init_dns (&Modes.dns4, &Modes.dns6);

  if (Modes.dns4)
     Modes.mgr.dns4.url = Modes.dns4;
  if (Modes.dns6)
     Modes.mgr.dns6.url = Modes.dns6;

  LOG_FILEONLY ("Added %zu IPv4 and %zu IPv6 addresses to deny.\n",
               deny_list_num4(), deny_list_num6());

  /* Setup the RTL_TCP service and possibly rename if '--device udp://host:port' was used.
   */
  if (modeS_net_services [MODES_NET_SERVICE_RTL_TCP].host [0])
  {
    if (!connection_setup_active(MODES_NET_SERVICE_RTL_TCP, &Modes.rtl_tcp_in))
        return (false);

    if (modeS_net_services [MODES_NET_SERVICE_RTL_TCP].is_udp)
    {
      strcpy (modeS_net_services [MODES_NET_SERVICE_RTL_TCP].descr, "RTL-UDP input");
      strcpy (modeS_net_services [MODES_NET_SERVICE_RTL_TCP].protocol, "udp");
    }
  }

  /* If RAW-IN is UDP, rename description and protocol.
   */
  if (modeS_net_services [MODES_NET_SERVICE_RAW_IN].is_udp)
  {
    strcpy (modeS_net_services [MODES_NET_SERVICE_RAW_IN].descr, "Raw UDP input");
    strcpy (modeS_net_services [MODES_NET_SERVICE_RAW_IN].protocol, "udp");
  }

  if (Modes.net_active)
  {
    if (!modeS_net_services [MODES_NET_SERVICE_RAW_IN].host [0] &&
        !modeS_net_services [MODES_NET_SERVICE_SBS_IN].host [0])
    {
      LOG_STDERR ("No hosts for any `--net-active' services specified.\n");
      return (false);
    }

    if (modeS_net_services [MODES_NET_SERVICE_RAW_IN].host [0] &&
        !connection_setup_active(MODES_NET_SERVICE_RAW_IN, &Modes.raw_in))
       return (false);

    if (modeS_net_services [MODES_NET_SERVICE_SBS_IN].host [0] &&
        !connection_setup_active(MODES_NET_SERVICE_SBS_IN, &Modes.sbs_in))
       return (false);
  }
  else
  {
    if (!connection_setup_listen(MODES_NET_SERVICE_RAW_IN, &Modes.raw_in, false))
       return (false);

    if (!connection_setup_listen(MODES_NET_SERVICE_RAW_OUT, &Modes.raw_out, true))
       return (false);

    if (!connection_setup_listen(MODES_NET_SERVICE_SBS_OUT, &Modes.sbs_out, true))
       return (false);

    if (!connection_setup_listen(MODES_NET_SERVICE_HTTP, &Modes.http_out, true))
       return (false);
  }

  if (Modes.http_out && !check_packed_web_page() && !check_web_page())
     return (false);

  if (test_contains(Modes.tests, "net"))
  {
    unique_ip_tests();
    deny_lists_tests();
  }

  return (true);
}

bool net_exit (void)
{
  uint32_t num = net_conn_free_all();

  net_timer_del_all();
  unique_ips_free();
  deny_list_free();

#if defined(USE_PACKED_DLL)
  unload_web_dll();
  free (lookup_table);
#endif

  net_flush_all();
  mg_mgr_free (&Modes.mgr); /* This calls free() on all timers */

  if (Modes.dns4)
     free (Modes.dns4);

  if (Modes.dns6)
     free (Modes.dns6);

  if (Modes.rtltcp.info)
     free (Modes.rtltcp.info);

  Modes.mgr.conns = NULL;
  Modes.dns4 = Modes.dns6 = NULL;

  if (num > 0)
     Sleep (100);
  return (num > 0);
}

void net_poll (void)
{
  static uint64_t tc_last = 0;
  uint64_t        tc_now;

  /* Poll Mongoose for network events
   */
  mg_mgr_poll (&Modes.mgr, MODES_INTERACTIVE_REFRESH_TIME / 2);   /* == 125 msec max */

  /* If the RTL_TCP server went away, that's fatal
   */
  if (Modes.stat.srv_removed [MODES_NET_SERVICE_RTL_TCP] > 0)
  {
    LOG_STDERR ("RTL_TCP-server at '%s' vanished!\n", net_service_url (MODES_NET_SERVICE_RTL_TCP));
    Modes.exit = true;
  }

  tc_now = GetTickCount64();
  if (tc_now - tc_last >= 30000)  /* approx. every 30 sec */
  {
    tc_last = tc_now;
    if (Modes.log)
    {
      fflush (Modes.log);
      _commit (fileno(Modes.log));
    }
  }
}

/**
 * Network functions for communicating with
 * a remote RTLSDR device.
 */
static const char *get_tuner_type (int type)
{
  type = ntohl (type);

  return (type == RTLSDR_TUNER_UNKNOWN ? "Unknown" :
          type == RTLSDR_TUNER_E4000   ? "E4000"   :
          type == RTLSDR_TUNER_FC0012  ? "FC0012"  :
          type == RTLSDR_TUNER_FC0013  ? "FC09013" :
          type == RTLSDR_TUNER_FC2580  ? "FC2580"  :
          type == RTLSDR_TUNER_R820T   ? "R820T"   :
          type == RTLSDR_TUNER_R828D   ? "R828D"   : "?");
}

/**
 * The gain-values depends on tuner type.
 */
static bool get_gain_values (const RTL_TCP_info *info, int **gains, int *gain_count)
{
  static int e4k_gains []    = { -10,  15,  40,  65,  90, 115, 140,
                                 165, 190, 215, 240, 290, 340, 420
                               };
  static int fc0012_gains [] = { -99, -40,  71, 179, 192 };
  static int fc0013_gains [] = { -99, -73, -65, -63, -60, -58, -54,  58,
                                  61,  63,  65,  67,  68,  70,  71, 179,
                                 181, 182, 184, 186, 188, 191, 197
                              };
  static int r82xx_gains [] = {   0,   9,  14,  27,  37,  77,  87, 125,
                                144, 157, 166, 197, 207, 229, 254, 280,
                                297, 328, 338, 364, 372, 386, 402, 421,
                                434, 439, 445, 480, 496
                              };
  uint32_t gcount = ntohl (info->tuner_gain_count);

  switch (info->tuner_type)
  {
    case RTLSDR_TUNER_E4000:
         *gains      = e4k_gains;
         *gain_count = DIM (e4k_gains);
         break;

    case RTLSDR_TUNER_FC0012:
         *gains      = fc0012_gains;
         *gain_count = DIM (fc0012_gains);
         break;

    case RTLSDR_TUNER_FC0013:
         *gains      = fc0013_gains;
         *gain_count = DIM (fc0013_gains);
         break;

    case RTLSDR_TUNER_R820T:
    case RTLSDR_TUNER_R828D:
         *gains      = r82xx_gains;
         *gain_count = DIM (r82xx_gains);
         break;

    case RTLSDR_TUNER_FC2580:
    default:
         *gains      = NULL;
         *gain_count = 0;
         LOG_STDERR ("No gain values, tuner: %s\n", get_tuner_type(info->tuner_type));
         return (false);
  }

  if (gcount != (uint32_t) *gain_count)
  {
    LOG_STDERR ("Unexpected number of gain values reported by server: %u vs. %d (tuner: %s)\n",
                gcount, *gain_count, get_tuner_type(info->tuner_type));
    return (false);
  }
   return (true);
}

/**
 * Similar to `nearest_gain()` for a local RTLSDR device.
 */
static bool set_nearest_gain (RTL_TCP_info *info, uint16_t *target_gain)
{
  int      gain_in;
  int      i, err1, err2, nearest;
  int     *gains, gain_count;
  char     gbuf [200], *p = gbuf;
  size_t   left = sizeof(gbuf);
  uint32_t gcount = ntohl (info->tuner_gain_count);

  if (gcount <= 0 || !get_gain_values(info, &gains, &gain_count))
     return (false);

  Modes.rtltcp.gain_count = gain_count;
  Modes.rtltcp.gains      = memdup ((void*)gains, sizeof(int) * gain_count);
  nearest = Modes.rtltcp.gains [0];
  if (!target_gain)
     return (true);

  gain_in = *target_gain;

  for (i = 0; i < Modes.rtltcp.gain_count; i++)
  {
    err1 = abs (gain_in - nearest);
    err2 = abs (gain_in - Modes.rtltcp.gains [i]);

    p += snprintf (p, left, "%.1f, ", Modes.rtltcp.gains [i] / 10.0);
    left = sizeof(gbuf) - (p - gbuf) - 1;
    if (err2 < err1)
       nearest = Modes.rtltcp.gains [i];
  }
  p [-2] = '\0';
  LOG_STDOUT ("Supported gains: %s.\n", gbuf);
  *target_gain = (uint16_t) nearest;
  return (true);
}

/**
 * The read event handler expecting the RTL_TCP welcome-message.
 */
static void rtl_tcp_recv_info (mg_iobuf *msg)
{
  if (msg->len >= sizeof(Modes.rtltcp.info) &&
      !memcmp(msg->buf, RTL_TCP_MAGIC, sizeof(RTL_TCP_MAGIC)-1))
  {
    RTL_TCP_info *info = memdup (msg->buf, sizeof(*info));

    Modes.rtltcp.info = info;  /* a copy */

    DEBUG (DEBUG_NET, "tuner_type: \"%s\", gain_count: %lu.\n",
           get_tuner_type(info->tuner_type), ntohl(info->tuner_gain_count));

    net_timer_del (MODES_NET_SERVICE_RTL_TCP);
    mg_iobuf_del (msg, 0, sizeof(*info));

    if (set_nearest_gain (info, Modes.gain_auto ? NULL : &Modes.gain))
    {
      rtl_tcp_set_gain_mode (Modes.rtl_tcp_in, Modes.gain_auto);
      rtl_tcp_set_gain (Modes.rtl_tcp_in, Modes.gain);
    }
  }
}

/**
 * The read event handler for the RTL_TCP raw IQ data.
 */
static void rtl_tcp_recv_data (mg_iobuf *msg)
{
  rx_callback (msg->buf, msg->len, (void*)&Modes.exit);
  mg_iobuf_del (msg, 0, msg->len);
}

/**
 * The read event handler for all RTL_TCP messages.
 */
static bool rtl_tcp_decode (mg_iobuf *msg, int loop_cnt)
{
  if (msg->len == 0)  /* all was consumed */
     return (false);

  if (!Modes.rtltcp.info)
       rtl_tcp_recv_info (msg);
  else rtl_tcp_recv_data (msg);

  (void) loop_cnt;
  return (true);
}

/**
 * Send a single command to the RTL_TCP service.
 */
static bool rtl_tcp_command (mg_connection *c, uint8_t command, uint32_t param)
{
  RTL_TCP_cmd cmd;

  cmd.cmd   = command;
  cmd.param = htonl (param);
  return mg_send (c, &cmd, sizeof(cmd));
}

/**
 * The `MG_EV_CONNECT` handler for the RTL_TCP service.
 * Send the setup parameters to the remote server.
 */
static bool rtl_tcp_connect (mg_connection *c)
{
  INT_PTR service = MODES_NET_SERVICE_RTL_TCP;

  DEBUG (DEBUG_NET, "Setting sample-rate: %.2f MS/s.\n", (double)Modes.sample_rate/1E6);
  if (!rtl_tcp_command (c, RTL_SET_SAMPLE_RATE, Modes.sample_rate))
     goto failed;

  DEBUG (DEBUG_NET, "Setting frequency: %.2f MHz.\n", (double)Modes.freq/1E6);
  if (!rtl_tcp_command (c, RTL_SET_FREQUENCY, Modes.freq))
     goto failed;

  DEBUG (DEBUG_NET, "Setting PPM: %d.\n", Modes.rtltcp.ppm_error);
  if (!rtl_tcp_command (c, RTL_SET_FREQ_CORRECTION, Modes.rtltcp.ppm_error))
     goto failed;

  /* If we do not get the `RTL_TCP_MAGIC` welcome message.
   * Or if we stop getting data, add a timer to detect it.
   */
  net_timer_add (service, MODES_DATA_TIMEOUT, MG_TIMER_REPEAT);
  return (true);

failed:
  c->is_closing = 1;
  return (false);
}

bool rtl_tcp_set_gain (mg_connection *c, int16_t gain)
{
  return rtl_tcp_command (c, RTL_SET_GAIN, gain);
}

bool rtl_tcp_set_gain_mode (mg_connection *c, bool autogain)
{
  return rtl_tcp_command (c, RTL_SET_GAIN_MODE, autogain);
}

