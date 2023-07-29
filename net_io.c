/**\file    net_io.c
 * \ingroup Main
 * \brief   Most network functions and handling of network services.
 */
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>

#include "misc.h"
#include "aircraft.h"
#include "net_io.h"

/**
 * Handlers for the network services.
 *
 * We use Mongoose for handling all the server and low-level network I/O. <br>
 * We register event-handlers that gets called on important network events.
 *
 * Keep the data for our 5 network services in this structure.
 */
net_service modeS_net_services [MODES_NET_SERVICES_NUM] = {
          { &Modes.raw_out,  "Raw TCP output", "tcp", MODES_NET_PORT_RAW_OUT }, // MODES_NET_SERVICE_RAW_OUT
          { &Modes.raw_in,   "Raw TCP input",  "tcp", MODES_NET_PORT_RAW_IN },  // MODES_NET_SERVICE_RAW_IN
          { &Modes.sbs_out,  "SBS TCP output", "tcp", MODES_NET_PORT_SBS },     // MODES_NET_SERVICE_SBS_OUT
          { &Modes.sbs_in,   "SBS TCP input",  "tcp", MODES_NET_PORT_SBS },     // MODES_NET_SERVICE_SBS_IN
          { &Modes.http_out, "HTTP server",    "tcp", MODES_NET_PORT_HTTP }     // MODES_NET_SERVICE_HTTP
        };

#if defined(PACKED_WEB_ROOT)
  extern const char *mg_unpack (const char *name, size_t *size, time_t *mtime);
  extern const char *mg_unlist (size_t i);
  extern unsigned    mg_usage_count (size_t i);
#endif

static void        net_handler (mg_connection *c, int ev, void *ev_data, void *fn_data);
static void        net_timeout (void *fn_data);
static void        net_conn_free (connection *conn, intptr_t service);
static char       *net_store_error (intptr_t service, const char *err);
static char       *net_error_details (mg_connection *c, const char *in_out, const void *ev_data);
static char       *net_str_addr (const mg_addr *a, char *buf, size_t len);

static uint16_t   *net_num_connections (intptr_t service);
static uint64_t    net_mem_allocated (intptr_t service, int size);
static const char *net_service_descr (intptr_t service);
static char       *net_service_error (intptr_t service);
static char       *net_service_url (intptr_t service);

static bool  client_handler (const mg_connection *c, intptr_t service, int ev);
static bool  client_is_unique (const mg_addr *addr);
static bool  client_is_extern (const mg_addr *addr);
static bool  client_deny (const mg_addr *addr, intptr_t service);

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
          ev == MG_EV_HTTP_CHUNK ? "MG_EV_HTTP_CHUNK" :
          ev == MG_EV_WS_OPEN    ? "MG_EV_WS_OPEN" :
          ev == MG_EV_WS_MSG     ? "MG_EV_WS_MSG" :
          ev == MG_EV_WS_CTL     ? "MG_EV_WS_CTL" :
          ev == MG_EV_MQTT_CMD   ? "MG_EV_MQTT_CMD" :   /* Can never occur here */
          ev == MG_EV_MQTT_MSG   ? "MG_EV_MQTT_MSG" :   /* Can never occur here */
          ev == MG_EV_MQTT_OPEN  ? "MG_EV_MQTT_OPEN" :  /* Can never occur here */
          ev == MG_EV_SNTP_TIME  ? "MG_EV_SNTP_TIME"    /* Can never occur here */
                                 : "?");
}

/**
 * Setup a connection for a service.
 * Active or passive (`listen == true`).
 * If it's active, we could use udp.
 */
static mg_connection *connection_setup (intptr_t service, bool listen, bool sending)
{
  mg_connection *c = NULL;
  bool           allow_udp = (service == MODES_NET_SERVICE_RAW_IN);
  bool           use_udp   = (modeS_net_services[service].is_udp && !modeS_net_services[service].is_ip6);
  char           url [sizeof(mg_host_name)+6];

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
                modeS_net_services[service].host,
                modeS_net_services[service].port,
                net_service_descr(service));
    goto quit;
  }

  modeS_net_services [service].active_send = sending;

  if (listen)
  {
    snprintf (url, sizeof(url), "%s://0.0.0.0:%u",
              modeS_net_services[service].protocol,
              modeS_net_services[service].port);

    modeS_net_services[service].url = strdup (url);

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

    if (modeS_net_services[service].is_udp)
       timeout = -1;      /* Should UDP expire? */

    snprintf (url, sizeof(url), "%s://%s:%u",
              modeS_net_services[service].protocol,
              modeS_net_services[service].host,
              modeS_net_services[service].port);

    modeS_net_services[service].url = strdup (url);

    if (timeout > 0)
       mg_timer_init (&Modes.mgr.timers, &modeS_net_services[service].timer,
                      timeout, MG_TIMER_ONCE, net_timeout, (void*)service);

    DEBUG (DEBUG_NET, "Connecting to '%s' (service \"%s\", timeout: %d).\n",
           url, net_service_descr(service), timeout);

    c = mg_connect (&Modes.mgr, url, net_handler, (void*) service);
  }

  if (c && (Modes.debug & DEBUG_MONGOOSE2))
     c->is_hexdumping = 1;

quit:
  modeS_err_set (false);
  modeS_set_log();         /* restore previous log-settings */
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
void net_connection_recv (connection *conn, net_msg_handler handler, bool is_server)
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

  for (conn = Modes.connections[service]; conn; conn = conn->next)
  {
    if (conn->service != service)
       continue;

    mg_send (conn->c, msg, len);   /* if write fails, the client gets freed in net_handler() */
    found++;
  }
  if (found > 0)
     DEBUG (DEBUG_NET, "Sent %zd bytes to %d clients in service \"%s\".\n",
            len, found, net_service_descr(service));
}

/**
 * Returns a `connection *` based on the remote `addr` and `service`.
 * This can be either client or server.
 */
connection *connection_get (mg_connection *c, intptr_t service, int ev, bool is_server)
{
  connection *conn;

  ASSERT_SERVICE (service);

  for (conn = Modes.connections[service]; conn; conn = conn->next)
  {
    if (conn->service == service && !memcmp(&conn->rem, &c->rem, sizeof(mg_addr)))
       return (conn);
  }

  if (ev != MG_EV_CLOSE)
  {
    mg_host_name addr_buf;

    LOG_FILEONLY ("No conn-id for remote %s (event: %s, conn-id: %lu, closing: %d, service: \"%s\")\n",
                  net_str_addr(&c->rem, addr_buf, sizeof(addr_buf)),
                  event_name(ev), c->id, c->is_closing, net_service_descr(service));
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
 * Generated arrays from
 *   xxd -i favicon.png
 *   xxd -i favicon.ico
 */
#include "favicon.c"

static void send_favicon (mg_connection *c,
                          connection    *cli,
                          const uint8_t *data,
                          size_t         data_len,
                          const char    *content_type)
{
  DEBUG (DEBUG_NET, "Sending favicon (%s, %zu bytes, conn-id: %lu).\n",
         content_type, data_len, c->id);

  mg_printf (c, "HTTP/1.1 200 OK\r\n"
                "Content-Length: %lu\r\n"
                "%s\r\n", data_len, set_headers(cli, content_type));
  mg_send (c, data, data_len);
  c->is_resp = 0;
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
  if (!Modes.json_aircraft_history [history_size].ptr)
     history_size = Modes.json_aircraft_history_next;

  return mg_mprintf ("{\"version\": \"%s\", "
                      "\"refresh\": %llu, "
                      "\"history\": %d, "
                      "\"lat\": %.6g, "          /* if 'Modes.home_pos_ok == false', this is 0. */
                      "\"lon\": %.6g}",          /* ditto */
                      PROG_VERSION,
                      Modes.json_interval,
                      history_size,
                      Modes.home_pos.lat,
                      Modes.home_pos.lon);
}

/**
 * The event handler for all HTTP traffic.
 */
static int net_handler_http (mg_connection *c, mg_http_message *hm, char *request_uri, size_t request_uri_sz)
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
  len = min (request_uri_sz-1, hm->uri.len);
  uri = strncpy (request_uri, hm->uri.ptr, len);
  request_uri [len] = '\0';

  first_nl = strchr (hm->head.ptr, '\r');
  len = hm->head.len;

  if (first_nl > hm->head.ptr - 1)
     len = first_nl - hm->head.ptr;

  DEBUG (DEBUG_NET, "\n"
         "  MG_EV_HTTP_MSG: (conn-id: %lu)\n"
         "    head:    '%.*s' ...\n"     // 1st line in request
         "    uri:     '%s'\n"
         "    method:  '%.*s'\n",
         c->id, (int)len, hm->head.ptr, uri, (int)hm->method.len, hm->method.ptr);

  is_GET  = (mg_vcasecmp(&hm->method, "GET") == 0);
  is_HEAD = (mg_vcasecmp(&hm->method, "HEAD") == 0);

  if (!is_GET && !is_HEAD)
  {
    DEBUG (DEBUG_NET, "Bad Request: '%.*s %s' from %s (conn-id: %lu)\n",
           (int)hm->method.len, hm->method.ptr, uri,
           net_str_addr(&c->rem, addr_buf, sizeof(addr_buf)), c->id);

    Modes.stat.HTTP_400_responses++;
    return (400);
  }

  cli = connection_get (c, MODES_NET_SERVICE_HTTP, MG_EV_HTTP_MSG, false);
  if (!cli)
     return (505);

  Modes.stat.HTTP_get_requests++;

  header = mg_http_get_header (hm, "Connection");
  if (header && !mg_vcasecmp(header, "keep-alive"))
  {
    DEBUG (DEBUG_NET2, "Connection: '%.*s'\n", (int)header->len, header->ptr);
    Modes.stat.HTTP_keep_alive_recv++;
    cli->keep_alive = true;
  }

  header = mg_http_get_header (hm, "Accept-Encoding");
  if (header && !mg_vcasecmp(header, "gzip"))
  {
    DEBUG (DEBUG_NET, "Accept-Encoding: '%.*s'\n", (int)header->len, header->ptr);
    cli->encoding_gzip = true;  /**\todo Add gzip compression */
  }

  /* Redirect a 'GET /' to a 'GET /' + 'web_page'
   */
  if (!strcmp(uri, "/"))
  {
    mg_printf (c, "HTTP/1.1 301 Moved\r\n"
                  "Location: %s\r\n"
                  "Content-Length: 0\r\n\r\n", Modes.web_page);

    DEBUG (DEBUG_NET, "301 redirect to: '%s/%s'\n", Modes.web_root, Modes.web_page);
    return (301);
  }

  /**
   * \todo Check header for a "Upgrade: websocket" and call mg_ws_upgrade()?
   */
  if (!stricmp(uri, "/echo"))
  {
    DEBUG (DEBUG_NET, "Got WebSocket echo:\n'%.*s'.\n", (int)hm->head.len, hm->head.ptr);
    mg_ws_upgrade (c, hm, "WS test");
    return (200);
  }

  if (!stricmp(uri, "/data/receiver.json"))
  {
    char *data = receiver_to_json();

    DEBUG (DEBUG_NET, "Feeding conn-id %lu with receiver-data:\n%.100s\n", c->id, data);

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
    int rc = 200;        /* Assume status 200 OK */

    if (!stricmp(uri, "/favicon.png"))
       send_favicon (c, cli, favicon_png, favicon_png_len, MODES_CONTENT_TYPE_PNG);

    else if (!stricmp(uri, "/favicon.ico"))   /* Some browsers may want a 'favicon.ico' file */
       send_favicon (c, cli, favicon_ico, favicon_ico_len, MODES_CONTENT_TYPE_ICON);

    else
    {
      mg_http_serve_opts opts;
      mg_file_path       file;
      bool               found;
      const char        *packed = "";

      memset (&opts, '\0', sizeof(opts));
      opts.page404       = NULL;
      opts.extra_headers = set_headers (cli, content_type);

      snprintf (file, sizeof(file), "%s/%s", Modes.web_root, uri+1);
      DEBUG (DEBUG_NET, "file: '%s'.\n", file);
      fflush (stdout);

#if defined(PACKED_WEB_ROOT)
      opts.fs = &mg_fs_packed;
      packed  = " packed";
      found = (mg_unpack(file, NULL, NULL) != NULL);
#else
      found = (access(file, 0) == 0);
#endif

      DEBUG (DEBUG_NET, "Serving %sfile: '%s', found: %d.\n", packed, file, found);
      DEBUG (DEBUG_NET, "extra-headers: '%s'.\n", opts.extra_headers);

      mg_http_serve_file (c, hm, file, &opts);

      if (!found)
      {
        Modes.stat.HTTP_404_responses++;
        rc = 404;
      }
    }
    return (rc);
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
    HEX_DUMP (ws->data.ptr, ws->data.len);
  }
  else if (ev == MG_EV_WS_MSG)
  {
    DEBUG (DEBUG_MONGOOSE2, "WebSock message from conn-id: %lu:\n", c->id);
    HEX_DUMP (ws->data.ptr, ws->data.len);
  }
  else if (ev == MG_EV_WS_CTL)
  {
    DEBUG (DEBUG_MONGOOSE2, "WebSock control from conn-id: %lu:\n", c->id);
    HEX_DUMP (ws->data.ptr, ws->data.len);
    Modes.stat.HTTP_websockets++;
  }
  return (1);
}

/**
 * The timer callback for an active `connect()`.
 */
static void net_timeout (void *fn_data)
{
  INT_PTR service = (int)(INT_PTR) fn_data;
  char    err [200];

  snprintf (err, sizeof(err), "Timeout in connection to host %s (service: \"%s\")",
            net_service_url(service), net_service_descr(service));
  net_store_error (service, err);

  modeS_signal_handler (0);  /* break out of main_data_loop()  */
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
  bool        get_WSAE = false;

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
      orig_err[0] = '\0';
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
  connection *conn = connection_get (c, service, MG_EV_ERROR, true);
  const char *err;

  err = net_error_details (c, "Connection in ", ev_data);
  net_store_error (service, err);
  net_conn_free (conn, service);
}

/**
 * The event handler for ALL network I/O.
 */
static void net_handler (mg_connection *c, int ev, void *ev_data, void *fn_data)
{
  connection  *conn;
  char        *remote;
  mg_host_name remote_buf;
  long         bytes;                              /* bytes read or written */
  INT_PTR      service = (int)(INT_PTR) fn_data;   /* 'fn_data' is arbitrary user data */

  if (Modes.exit)
     return;

  if (ev == MG_EV_POLL || ev == MG_EV_OPEN)    /* Ignore thes events */
     return;

  if (ev == MG_EV_ERROR)
  {
    remote = modeS_net_services [service].host;

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
        mg_timer_free (&Modes.mgr.timers, &modeS_net_services[service].timer);
        modeS_signal_handler (0);   /* break out of main_data_loop()  */
      }
    }
    return;
  }

  remote = net_str_addr (&c->rem, remote_buf, sizeof(remote_buf));

  if (ev == MG_EV_RESOLVE)
  {
    DEBUG (DEBUG_NET, "MG_EV_RESOLVE: address %s (service: \"%s\")\n", remote, net_service_url(service));
    return;
  }

  if (ev == MG_EV_CONNECT)
  {
    DEBUG (DEBUG_NET, "Stopping timer for host %s (service \"%s\").\n", remote, net_service_descr(service));
    mg_timer_free (&Modes.mgr.timers, &modeS_net_services[service].timer);

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

    LIST_ADD_TAIL (connection, &Modes.connections[service], conn);
    ++ (*net_num_connections (service));  /* should never go above 1 */
    net_mem_allocated (service, sizeof(*conn));

    Modes.stat.srv_connected [service]++;

    DEBUG (DEBUG_NET, "Connected to host %s (service \"%s\")\n", remote, net_service_descr(service));
    return;
  }

  if (ev == MG_EV_ACCEPT)
  {
    if (!client_handler(c, service, MG_EV_ACCEPT))    /* Drop this remote? */
    {
      shutdown ((SOCKET) ((size_t) c->fd), SD_BOTH);
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

    LIST_ADD_TAIL (connection, &Modes.connections[service], conn);
    ++ (*net_num_connections (service));
    net_mem_allocated (service, (int)sizeof(*conn));

    Modes.stat.cli_accepted [service]++;
    return;
  }

  if (ev == MG_EV_READ)
  {
    bytes = *(const long*) ev_data;
    Modes.stat.bytes_recv [service] += bytes;

    DEBUG (DEBUG_NET2, "MG_EV_READ: %lu bytes from %s (service \"%s\")\n",
           bytes, remote, net_service_descr(service));

    if (service == MODES_NET_SERVICE_RAW_IN)
    {
      conn = connection_get (c, service, MG_EV_READ, false);
      net_connection_recv (conn, decode_RAW_message, false);

      conn = connection_get (c, service, MG_EV_READ, true);
      net_connection_recv (conn, decode_RAW_message, true);
    }
    else if (service == MODES_NET_SERVICE_SBS_IN)
    {
      conn = connection_get (c, service, MG_EV_READ, true);
      net_connection_recv (conn, decode_SBS_message, true);
    }
    return;
  }

  if (ev == MG_EV_WRITE)         /* Increment our own send() bytes */
  {
    bytes = *(const long*) ev_data;
    Modes.stat.bytes_sent [service] += bytes;
    DEBUG (DEBUG_NET2, "MG_EV_WRITE: %ld bytes to %s (\"%s\").\n",
           bytes, remote, net_service_descr(service));
    return;
  }

  if (ev == MG_EV_CLOSE)
  {
    client_handler (c, service, MG_EV_CLOSE);

    conn = connection_get (c, service, ev, false);
    net_conn_free (conn, service);

    conn = connection_get (c, service, ev, true);
    net_conn_free (conn, service);

    -- (*net_num_connections (service));
    return;
  }

  if (service == MODES_NET_SERVICE_HTTP)
  {
    mg_http_message *hm = ev_data;
    mg_ws_message   *ws = ev_data;
    char             request_uri [256];
    int              status;

    if (ev == MG_EV_WS_OPEN || ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL)
    {
      status = net_handler_websocket (c, ws, ev);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
      status = net_handler_http (c, hm, request_uri, sizeof(request_uri));

      DEBUG (DEBUG_NET, "HTTP %d for '%.*s' (conn-id: %lu)\n",
             status, (int)hm->uri.len, hm->uri.ptr, c->id);
    }
    else if (ev == MG_EV_HTTP_CHUNK)
    {
      DEBUG (DEBUG_MONGOOSE2, "HTTP chunk (conn-id: %lu):\n", c->id);
      HEX_DUMP (hm->message.ptr, hm->message.len);
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
    LOG_STDERR ("Listen socket for \"%s\" failed; %s.\n",
                net_service_descr(service), err);
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
  uint64_t     mem_now = 0ULL;
  mg_host_name addr;

  if (!this_conn)
     return;

  for (conn = Modes.connections[service]; conn; conn = conn->next)
  {
    if (conn != this_conn)
       continue;

    LIST_DELETE (connection, &Modes.connections[service], conn);
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

    mem_now = net_mem_allocated (service, - (int)sizeof(*conn));
    break;
  }

  DEBUG (DEBUG_NET, "Freeing %s at %s (conn-id: %lu, url: %s, service: \"%s\", mem_now: %llu).\n",
         is_server == 1 ? "server" :
         is_server == 0 ? "client" : "?", addr, id,
         net_service_url(service), net_service_descr(service), mem_now);
}

/*
 * Free all connections in all services.
 */
static uint32_t net_conn_free_all (void)
{
  intptr_t service;
  uint32_t num = 0;

  for (service = MODES_NET_SERVICE_FIRST; service <= MODES_NET_SERVICE_LAST; service++)
  {
    connection *conn, *conn_next;

    for (conn = Modes.connections[service]; conn; conn = conn_next)
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

static uint64_t net_mem_allocated (intptr_t service, int size)
{
  ASSERT_SERVICE (service);
  assert (modeS_net_services[service].mem_allocated + size >= 0);
  assert (modeS_net_services[service].mem_allocated + size < UINT64_MAX);
  modeS_net_services [service].mem_allocated += size;
  return (modeS_net_services [service].mem_allocated);
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
  return (modeS_net_services[service].url);
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

static void net_flushall (void)
{
  mg_connection *c;
  uint32_t       num_active  = 0;
  uint32_t       num_passive = 0;
  uint32_t       num_unknown = 0;
  size_t         total_rx = 0;
  size_t         total_tx = 0;

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
         "Flushed %u active connections, %u passive, %u unknown. Remaining bytes: %zu Rx, %zu Tx.\n",
         num_active, num_passive, num_unknown, total_rx, total_tx);
}

static bool client_handler (const mg_connection *c, intptr_t service, int ev)
{
  const mg_addr *addr = &c->rem;
  mg_host_name   addr_buf;
  bool           rc = true;

  assert (ev == MG_EV_ACCEPT || ev == MG_EV_CLOSE);

  if (ev == MG_EV_ACCEPT)
  {
    if (client_is_unique(addr))     /* Have we seen this IP-address before? */
       Modes.stat.unique_clients [service]++;

    if (client_is_extern(addr))     /* Not from 127.0.0.1 */
    {
      if (client_deny(addr, service))
         rc = false;

      if (Modes.debug & DEBUG_NET)
         Beep (rc ? 800 : 1200, 20);

      LOG_FILEONLY ("Opening connection: %s %s (conn-id: %lu, service: \"%s\").\n",
                    net_str_addr(addr, addr_buf, sizeof(addr_buf)),
                    rc ? "accepted" : "denied", c->id, net_service_descr(service));
    }
  }
  else if (client_is_extern(addr))      /* Not from 127.0.0.1 */
  {
    LOG_FILEONLY ("Closing connection: %s (conn-id: %lu, service: \"%s\").\n",
                  net_str_addr(addr, addr_buf, sizeof(addr_buf)), c->id, net_service_descr(service));
  }
  return (rc);
}

/**
 * \todo Fix this.
 * Assume yes for now.
 */
static bool client_is_unique (const mg_addr *addr)
{
  (void) addr;
  return (true);
}

static bool client_is_extern (const mg_addr *addr)
{
  uint32_t ip4;

  if (addr->is_ip6)
     return (false);            /**\todo fix this */

  ip4 = *(const uint32_t*) &addr->ip;
  return (ip4 != 0x0100007F);    /* ip4 !== 127.0.0.1 */
}

/**
 * \todo
 * Loop over `ModeS_net_services [service].deny_list4/6` to find a match
 * using `mg_check_ip_acl()`.
 */
static bool client_deny (const mg_addr *addr, intptr_t service)
{
#if 1
  // test: deny all '1-126.*' networks
  if (!addr->is_ip6 && addr->ip[0] >= 1 && addr->ip[0] <= 126)
        return (true);
#else
  (void) addr;
#endif

  (void) service;
  return (false);
}

/**
 * Since 'mg_straddr()' was removed in latest version
 */
static char *net_str_addr (const mg_addr *a, char *buf, size_t len)
{
  mg_snprintf (buf, len, "%M", mg_print_ip_port, a);
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
  snprintf (name, sizeof(name), "%.*s", (int)str.len, str.ptr);

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

#if defined(PACKED_WEB_ROOT)
/**
 * Functions for a "Packed Web Filesystem".
 *
 * \todo
 * Support multiple "Packed Web Filesystem" in a resource .DLL
 * with option:
 *   \li `--web-page some.dll;1` for the 1st resource.
 *   \li `--web-page some.dll;2` for the 2nd resource etc.
 *
 * Functions in the generated '$(OBJ_DIR)/packed_webfs.c' file.
 */
static size_t num_packed = 0;
static bool   has_index_html = false;

static void count_packed_fs (void)
{
  const char *fname;
  size_t      i;

  for (i = 0; (fname = mg_unlist(i)) != NULL; i++)
  {
    if (!strcmp(basename(fname), "index.html"))
       has_index_html = true;
  }
  num_packed = i;
}

void net_show_packed_usage (void)
{
  unsigned i, count;
  const char *fname;

  LOG_FILEONLY ("\nPacked-Web statistics:\n");

  for (i = 0; (fname = mg_unlist(i)) != NULL; i++)
  {
    count = mg_usage_count (i);
    if (count > 0)
       LOG_FILEONLY ("  %3u: %s\n", count, fname);
  }
  if (i == 0)
     LOG_FILEONLY ("  <None>\n");
}

static bool check_web_page (void)
{
  if (num_packed == 0)
  {
    LOG_STDERR ("The Packed Filesystem has no files!\n");
    return (false);
  }
  if (!has_index_html)
  {
    LOG_STDERR ("The Packed Filesystem has no 'index.html' file!\n");
    return (false);
  }
  return (true);
}

#else
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

void net_show_packed_usage (void)
{
  LOG_FILEONLY ("\nPacked-Web statistics:\n");
  LOG_FILEONLY ("  <N/A>\n");
}
#endif  /* PACKED_WEB_ROOT */

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

  if (Modes.stat.bytes_recv[s] == 0)
  {
    LOG_STDOUT ("    nothing.\n");
    return (false);
  }
  LOG_STDOUT ("  %8llu bytes.\n", Modes.stat.bytes_recv[s]);
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
    LOG_STDOUT ("  %8llu good messages.\n", Modes.stat.good_raw);
    LOG_STDOUT ("  %8llu empty messages.\n", Modes.stat.empty_raw);
    LOG_STDOUT ("  %8llu unrecognized messages.\n", Modes.stat.unrecognized_raw);
  }
}

static void show_raw_SBS_IN_stats (void)
{
  if (show_raw_common(MODES_NET_SERVICE_SBS_IN))
  {
    LOG_STDOUT ("  %8llu good messages.\n", Modes.stat.good_SBS);
    LOG_STDOUT ("  %8llu empty messages.\n", Modes.stat.empty_SBS);
    LOG_STDOUT ("  %8llu unrecognized messages.\n", Modes.stat.unrecognized_SBS);
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
        s == MODES_NET_SERVICE_SBS_IN)
       continue;

    LOG_STDOUT ("  %s (%s):\n", net_service_descr(s), url ? url : "none");

    if (Modes.net_active)
         sum = Modes.stat.srv_connected[s] + Modes.stat.srv_removed[s] + Modes.stat.srv_unknown[s];
    else sum = Modes.stat.cli_accepted[s]  + Modes.stat.cli_removed[s] + Modes.stat.cli_unknown[s];

    sum += Modes.stat.bytes_sent[s] + Modes.stat.bytes_recv[s] + *net_num_connections (s);
    if (sum == 0ULL)
    {
      LOG_STDOUT ("    Nothing.\n");
      continue;
    }

    if (s == MODES_NET_SERVICE_HTTP)
    {
      LOG_STDOUT ("    %8llu HTTP GET requests received.\n", Modes.stat.HTTP_get_requests);
      LOG_STDOUT ("    %8llu HTTP 400 replies sent.\n", Modes.stat.HTTP_400_responses);
      LOG_STDOUT ("    %8llu HTTP 404 replies sent.\n", Modes.stat.HTTP_404_responses);
      LOG_STDOUT ("    %8llu HTTP/WebSocket upgrades.\n", Modes.stat.HTTP_websockets);
      LOG_STDOUT ("    %8llu server connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_sent);
      LOG_STDOUT ("    %8llu client connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_recv);
    }

    if (Modes.net_active)
    {
      LOG_STDOUT ("    %8llu server connections done.\n", Modes.stat.srv_connected[s]);
      LOG_STDOUT ("    %8llu server connections removed.\n", Modes.stat.srv_removed[s]);
      LOG_STDOUT ("    %8llu server connections unknown.\n", Modes.stat.srv_unknown[s]);
      LOG_STDOUT ("    %8u server connections now.\n", *net_num_connections(s));
    }
    else
    {
      LOG_STDOUT ("    %8llu client connections accepted.\n", Modes.stat.cli_accepted[s]);
      LOG_STDOUT ("    %8llu client connections removed.\n", Modes.stat.cli_removed[s]);
      LOG_STDOUT ("    %8llu client connections unknown.\n", Modes.stat.cli_unknown[s]);
      LOG_STDOUT ("    %8u client(s) now.\n", *net_num_connections(s));
    }

    LOG_STDOUT ("    %8llu unique clients.\n", Modes.stat.unique_clients[s]);
    LOG_STDOUT ("    %8llu bytes sent.\n", Modes.stat.bytes_sent[s]);
    LOG_STDOUT ("    %8llu bytes recv.\n", Modes.stat.bytes_recv[s]);
  }

  if (Modes.net_active)
  {
    show_raw_SBS_IN_stats();
    show_raw_RAW_IN_stats();
  }

  if (Modes.stat.cli_accepted[MODES_NET_SERVICE_HTTP] > 0)
     net_show_packed_usage();

  net_show_server_errors();
}

/**
 * Initialize the Mongoose network manager and:
 *  \li start the 2 active network services (RAW_IN + SBS_IN).
 *  \li or start the 4 listening (passive) network services.
 *  \li if HTTP-server is enabled, check the precence of the Web-page.
 */
bool net_init (void)
{
#if defined(PACKED_WEB_ROOT)
  Modes.touch_web_root = false;
  LOG_STDOUT ("Ignoring the '--web-page %s/%s' option since we use a built-in 'Packed Filesystem'.\n",
              Modes.web_root, Modes.web_page);

  strncpy (Modes.web_root, PACKED_WEB_ROOT, sizeof(Modes.web_root));
  strcpy (Modes.web_page, "index.html");
  count_packed_fs();
#endif

#if MG_ENABLE_FILE
  if (Modes.touch_web_root)
     touch_dir (Modes.web_root, true);
#endif

  mg_mgr_init (&Modes.mgr);

  /* If RAW-IN is UDP, rename description and protocol.
   */
  if (modeS_net_services[MODES_NET_SERVICE_RAW_IN].is_udp)
  {
    strcpy (modeS_net_services[MODES_NET_SERVICE_RAW_IN].descr, "Raw UDP input");
    strcpy (modeS_net_services[MODES_NET_SERVICE_RAW_IN].protocol, "udp");
  }

  if (Modes.net_active)
  {
    if (!modeS_net_services[MODES_NET_SERVICE_RAW_IN].host[0] &&
        !modeS_net_services[MODES_NET_SERVICE_SBS_IN].host[0])
    {
      LOG_STDERR ("No hosts for any `--net-active' services specified.\n");
      return (false);
    }

    if (modeS_net_services[MODES_NET_SERVICE_RAW_IN].host[0] &&
        !connection_setup_active(MODES_NET_SERVICE_RAW_IN, &Modes.raw_in))
       return (false);

    if (modeS_net_services[MODES_NET_SERVICE_SBS_IN].host[0] &&
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
  if (Modes.http_out && !check_web_page())
     return (false);
  return (true);
}

bool net_exit (void)
{
  uint32_t num = net_conn_free_all();

  net_flushall();
  mg_mgr_free (&Modes.mgr);
  Modes.mgr.conns = NULL;
  if (num > 0)
     Sleep (100);
  return (num > 0);
}

void net_poll (void)
{
  static uint32_t net_stat_count = 0;

  /* Poll Mongoose for network events
   */
  mg_mgr_poll (&Modes.mgr, MODES_INTERACTIVE_REFRESH_TIME / 2);   /* == 125 msec */

  if ((++net_stat_count % 80) == 0)  /* every 10 sec */
  {
    if (Modes.debug & DEBUG_NET)
       LOG_FILEONLY ("%6llu: mem_alloc: %llu\n", MSEC_TIME()/1000, net_mem_allocated(MODES_NET_SERVICE_HTTP, 0));

    if (Modes.log)
       fflush (Modes.log);
  }
}

