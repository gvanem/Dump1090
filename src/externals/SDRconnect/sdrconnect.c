/**
 * \file    sdrconnect.c
 * \ingroup Samplers
 * \brief   The Web-socket interface for remote SDRConnect server.
 */
#include "misc.h"
#include "net_io.h"
#include "sdrconnect.h"

#undef  TRACE
#define TRACE(fmt, ...)                                \
        do {                                           \
          if (Modes.debug & DEBUG_WEBSOCKET)           \
             modeS_flogf (stdout, "sdrconnect.c(%u): " \
                fmt, __LINE__,  ## __VA_ARGS__);       \
        } while (0)

#undef  TRACE2
#define TRACE2(fmt, ...)                               \
        do {                                           \
          if (Modes.debug & DEBUG_WEBSOCKET2)          \
             modeS_flogf (stdout, "sdrconnect.c(%u): " \
                fmt, __LINE__,  ## __VA_ARGS__);       \
        } while (0)

#define HEX_DUMP(buf, len, what)              \
        do {                                  \
          if (Modes.debug & DEBUG_WEBSOCKET2) \
             hex_dump ((const uint8_t*)(buf), \
                       len, __LINE__, what);  \
        } while (0)

#define SET_PROPERTY(what, str_value)                 \
        websock_print3 ("event_type", "set_property", \
                        "property", what, "value", str_value)

#define GET_PROPERTY(what)                            \
        websock_print3 ("event_type", "get_property", \
                        "property", what, NULL, NULL)

#define STREAM_ENABLE(enable)                             \
        websock_print3 ("event_type", "iq_stream_enable", \
                        "property", "", "value", enable)

#define MAX_KEY_LEN   20
#define MAX_VAL_LEN  300   /* "valid_devices" can be rather long */

typedef bool (*state_func) (const char *key, const char *val);

typedef struct SDRConnect_priv {
        state_func state;
        char       tmp_str [20];
        bool       got_ws_open;
        bool       done_init;
        bool       stream_init;

        /* Filled by `json_parse()` via the `g_data.state()` state-functions:
         */
        char       api_version [20];
        char       valid_devices [MAX_VAL_LEN];
        char       active_device [20];
        char       active_antenna [20];
        uint64_t   device_center_frequency;
        uint64_t   device_vfo_frequency;
        double     device_sample_rate;
        double     signal_snr;
        double     signal_power;
      } SDRConnect_priv;

static SDRConnect_priv g_data;

static bool state_normal (const char *key, const char *val);
static bool state_api_version (const char *key, const char *val);
static bool state_valid_devices (const char *key, const char *val);
static bool state_active_device (const char *key, const char *val);
static bool state_active_antenna (const char *key, const char *val);
static bool state_signal_snr (const char *key, const char *val);
static bool state_signal_power (const char *key, const char *val);
static bool state_device_center_frequency (const char *key, const char *val);
static bool state_device_vfo_frequency (const char *key, const char *val);
static bool state_device_sample_rate (const char *key, const char *val);
static void json_parse (mg_str json);
static void hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what);
static void websock_print3 (const char *key1, const char *val1,
                            const char *key2, const char *val2,
                            const char *key3, const char *val3);

/**
 * Pass binary data on to `rx_callback()`.
 * Possibly in 2 chunks.
 */
static void binary_handler (const mg_ws_message *ws)
{
  uint16_t bin_type = *(const uint16_t*) &ws->data.buf [0];
  uint8_t *data;
  uint32_t len;

  if (bin_type == 2)    /* Signed 16-bit interleaved IQ (IQIQ) */
  {
    len  = min (ws->data.len - 2, MODES_ASYNC_BUF_SIZE);
    data = (uint8_t*) &ws->data.buf [2];
    rx_callback (data, len, (void*)&Modes.exit);

    if (ws->data.len - 2 > MODES_ASYNC_BUF_SIZE)
    {
      data += len;
      len = min (ws->data.len - 2 - len, MODES_ASYNC_BUF_SIZE);
      rx_callback (data, len, (void*)&Modes.exit);
    }
    Modes.stat.websock.samples_recv += ws->data.len / Modes.bytes_per_sample;
  }
}

/**
 * Handle WebSocket TEXT data.
 */
static void text_handler (const mg_ws_message *ws)
{
  mg_str json = ws->data;
  mg_str event_type = mg_json_get_tok (json, "$.event_type");
  mg_str property   = mg_json_get_tok (json, "$.property");

  TRACE2 ("event_type: %.*s, property: %.*s\n",
          (int)event_type.len, event_type.buf,
          (int)property.len,   property.buf);

  HEX_DUMP (json.buf, json.len, ", recv");

  json_parse (json);
}

/**
 * To replace `strncpy()` which does not always 0-terminate.
 * Copies maximum `dest_size - 1` characters into `dest`.
 * We do not care for any return value.
 */
static void json_strlcpy (char *dest, const char *src, size_t dest_size)
{
  assert (dest_size > 0);
  while (*src && dest_size > 1)
  {
    *dest++ = *src++;
    dest_size--;
  }
  *dest = '\0';  /* always 0-terminate */
}

/**
 * Since floating-point JSON-values contains `,`.
 * Convert to `.` and return a `double`.
 */
static double json_strtof (const char *val)
{
  char copy [MAX_VAL_LEN], *p;

  json_strlcpy (copy, val, sizeof(copy));
  p = strchr (copy, ',');
  if (p)
     *p = '.';
  return strtof (copy, NULL);
}

/**
 * Top-state `state_normal()`;
 * all actions springs out of this function.
 */
static bool state_normal (const char *key, const char *val)
{
  if (!stricmp(val, "api_version"))
     g_data.state = state_api_version;

  else if (!stricmp(val, "active_device"))
     g_data.state = state_active_device;

  else if (!stricmp(val, "valid_devices"))
     g_data.state = state_valid_devices;

  else if (!stricmp(val, "active_antenna"))
     g_data.state = state_active_antenna;

  else if (!stricmp(val, "signal_snr"))
     g_data.state = state_signal_snr;

  else if (!stricmp(val, "signal_power"))
     g_data.state = state_signal_power;

  else if (!stricmp(val, "device_center_frequency"))
     g_data.state = state_device_center_frequency;

  else if (!stricmp(val, "device_vfo_frequency"))
     g_data.state = state_device_vfo_frequency;

  else if (!stricmp(val, "device_sample_rate"))
     g_data.state = state_device_sample_rate;

  return (false);  /* Do not change state */
}

static bool state_api_version (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    json_strlcpy (g_data.api_version, val, sizeof(g_data.api_version));
    TRACE ("g_data.api_version:   '%s'\n", g_data.api_version);
    return (true);
  }
  return (false);
}

static bool state_active_device (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    json_strlcpy (g_data.active_device, val, sizeof(g_data.active_device));
    TRACE ("g_data.active_device: '%s'\n", g_data.active_device);
    return (true);
  }
  return (false);
}

static bool state_valid_devices (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    json_strlcpy (g_data.valid_devices, val, sizeof(g_data.valid_devices));
    TRACE ("g_data.valid_devices: '%s'\n", g_data.valid_devices);
    return (true);
  }
  return (false);
}

static bool state_active_antenna (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    json_strlcpy (g_data.active_antenna, val, sizeof(g_data.active_antenna));
    TRACE ("g_data.active_antenna: '%s'\n", g_data.active_antenna);
    return (true);
  }
  return (false);
}

static bool state_signal_snr (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.signal_snr = json_strtof (val);
    TRACE2 ("g_data.signal_snr:   %.6f dB\n", g_data.signal_snr);
    return (true);
  }
  return (false);
}

static bool state_signal_power (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.signal_power = json_strtof (val);
    TRACE2 ("g_data.signal_power: %.6f dBm\n", g_data.signal_power);
    return (true);
  }
  return (false);
}

static bool state_device_center_frequency (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.device_center_frequency = strtoull (val, NULL, 10);
    TRACE ("g_data.device_center_frequency: %.2f MHz\n", (double)g_data.device_center_frequency / 1E6);
    return (true);
  }
  return (false);
}

static bool state_device_vfo_frequency (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.device_vfo_frequency = strtoull (val, NULL, 10);
    TRACE ("g_data.device_vfo_frequency:    %.2f MHz\n", (double)g_data.device_vfo_frequency / 1E6);
    return (true);
  }
  return (false);
}

static bool state_device_sample_rate (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.device_sample_rate = json_strtof (val);
    TRACE ("g_data.device_sample_rate:         %.2f MS/s\n", g_data.device_sample_rate/1E6);
    return (true);
  }
  return (false);
}

/**
 * Rewritten from `json_scan()` in Mongoose's `test/unit_test.c`.
 * But our JSON-data is "flat"; not recursive (`depth == 0`).
 */
static void json_parse (mg_str json)
{
  int num = 0;
  int idx = mg_json_get (json, "$", &num);

  if (json.buf[idx] != '{')
  {
    TRACE ("Unexpected JSON: num: %d, %.*s\n", num, (int)json.len, json.buf);
    return;
  }

  mg_str key, val;
  mg_str sub = mg_str_n (json.buf + idx, (size_t)num);
  size_t ofs = 0;

  g_data.state = state_normal;

  /* Iterate over elems
   */
  while ((ofs = mg_json_next(sub, ofs, &key, &val)) > 0)
  {
    char _key [MAX_KEY_LEN];
    char _val [MAX_VAL_LEN];

    assert (key.buf[0] == '"');
    assert (val.buf[0] == '"');
    assert (key.len - 2 < sizeof(_key));
    assert (val.len - 2 < sizeof(_val));

    json_strlcpy (_key, &key.buf[1], key.len - 1);
    json_strlcpy (_val, &val.buf[1], val.len - 1);

    /* Add values to the `g_data` structure
     */
    if ((*g_data.state)(_key, _val))
       g_data.state = state_normal;
  }
}

static const char *websock_opcode (const mg_ws_message *ws)
{
  uint8_t opcode = (ws->flags & 15);

  return (opcode == WEBSOCKET_OP_CONTINUE ? "CONTINUE" :
          opcode == WEBSOCKET_OP_TEXT     ? "TEXT"     :
          opcode == WEBSOCKET_OP_BINARY   ? "BINARY"   :
          opcode == WEBSOCKET_OP_CLOSE    ? "CLOSE"    :
          opcode == WEBSOCKET_OP_PING     ? "PING"     :
          opcode == WEBSOCKET_OP_PONG     ? "PONG"     : "?");
}

/**
 * Call-back from net_io.c for all WebSocket events:
 *   if (ev == MG_EV_WS_OPEN); get and set initial properties for SDRConnect.
 *   if (ev == MG_EV_WS_MSG); send requests and handle responses.
 *   if (ev == MG_EV_WS_CTL); do nothing.
 */
static void websock_handler (enum mg_event ev, const mg_ws_message *ws)
{
  bool    is_text   = false;
  bool    is_binary = false;
  uint8_t opcode = (ws->flags & 15);

  if (ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL)
  {
    is_text   = (opcode == WEBSOCKET_OP_TEXT);
    is_binary = (opcode == WEBSOCKET_OP_BINARY);
  }

  switch (opcode)
  {
    case WEBSOCKET_OP_PING:
         Modes.stat.websock.ping++;
         break;
    case WEBSOCKET_OP_PONG:
         Modes.stat.websock.pong++;
         break;
    case WEBSOCKET_OP_TEXT:
         Modes.stat.websock.text++;
         break;
    case WEBSOCKET_OP_CONTINUE:  /* cannot happen here */
    case WEBSOCKET_OP_CLOSE:     /* cannot happen here */
         break;
  }

  TRACE2 ("ev: %s, op: %s, len %zd\n", net_ev_name(ev), websock_opcode(ws), ws->data.len);

  /* Sent from `mg_ws_client_handshake()` when HTTP upgrade was done.
   * Now we're ready to get and set initial properties for SDRConnect.
   */
  if (ev == MG_EV_WS_OPEN)
  {
    g_data.got_ws_open = true;
  }

  if (!g_data.done_init && g_data.got_ws_open)
  {
    g_data.done_init = true;

    GET_PROPERTY ("api_version");
    GET_PROPERTY ("active_device");
    GET_PROPERTY ("valid_devices");
    GET_PROPERTY ("active_antenna");
    SET_PROPERTY ("device_center_frequency", _ui64toa(Modes.freq, g_data.tmp_str, 10));
    SET_PROPERTY ("device_vfo_frequency", _ui64toa(Modes.freq, g_data.tmp_str, 10));
    SET_PROPERTY ("device_sample_rate", _ui64toa(Modes.sample_rate, g_data.tmp_str, 10));
    return;
  }

  if (is_binary)
  {
    if (g_data.stream_init)
       binary_handler (ws);
  }
  else if (is_text)
  {
    text_handler (ws);
    if (!g_data.stream_init)
    {
      STREAM_ENABLE ("true");
      g_data.stream_init = true;
    }
  }
}

void sdrconnect_init (void)
{
  net_ws_handler = websock_handler;
}

void sdrconnect_exit (void)
{
  Modes.debug &= ~(DEBUG_WEBSOCKET | DEBUG_WEBSOCKET2);

  if (g_data.done_init && g_data.got_ws_open)
     STREAM_ENABLE ("false");

  g_data.done_init = g_data.got_ws_open = false;
}

void sdrconnect_stats (void)
{
  int         s = MODES_NET_SERVICE_WEBSOCK;
  const char *url = net_handler_url (s);

  LOG_STDOUT ("! \n");
  LOG_STDOUT ("  %s (%s):\n", net_handler_descr(s), url ? url : "none");
  if (Modes.stat.bytes_recv[s] == 0ULL)
     LOG_STDOUT ("    nothing.\n");
  else
  {
    LOG_STDOUT ("  %15s bytes recv.\n", qword_str(Modes.stat.bytes_recv[s]));
    LOG_STDOUT ("  %15s bytes sent.\n", qword_str(Modes.stat.bytes_sent[s]));
    LOG_STDOUT ("  %15s IQ-samples recv.\n", qword_str(Modes.stat.websock.samples_recv));
    LOG_STDOUT ("  %15s TEXT recv.\n", qword_str(Modes.stat.websock.text));
    LOG_STDOUT ("  %15s PING recv.\n", qword_str(Modes.stat.websock.ping));
    LOG_STDOUT ("  %15s PONG sent.\n", qword_str(Modes.stat.websock.pong));
  }
}

/*
 * If no samples received, no statistics to show.
 */
void sdrconnect_no_stats (intptr_t service)
{
  if (service == MODES_NET_SERVICE_WEBSOCK && Modes.stat.websock.samples_recv == 0)
     Modes.no_stats = true;
}

void sdrconnect_tests (void)
{
  TRACE ("%s() called.\7\n", __FUNCTION__);

  sdrconnect_init();

  while (!Modes.exit)
  {
    /*
     * Poll and handle network events
     */
    background_tasks();
  }
  sdrconnect_exit();
  sdrconnect_stats();
}

/*
 * Local wrapper for `mg_ws_printf()` to ease tracing and hex-dumping.
 * Send 2 or 3 key/value JSON-pairs to SDRConnect.
 */
static void websock_print3 (const char *key1, const char *val1,
                            const char *key2, const char *val2,
                            const char *key3, const char *val3)
{
  char   buf1 [3 * (MAX_KEY_LEN + MAX_VAL_LEN)];   /* More than enough */
  char   buf2 [2 * (MAX_KEY_LEN + MAX_VAL_LEN)] = "";
  size_t len;

  if (key3 && val3)
     snprintf (buf2, sizeof(buf2), ", \"%s\": \"%s\"", key3, val3);

  len = snprintf (buf1, sizeof(buf1),
                 "{\"%s\": \"%s\", \"%s\": \"%s\"%s}",
                  key1, val1, key2, val2, buf2);

  HEX_DUMP (buf1, len, ", send");

  if (mg_ws_send(Modes.websock_in, buf1, len, WEBSOCKET_OP_TEXT) < len)
     TRACE ("mg_ws_send() failed to send: %zd bytes\n", len);
}

/**
 * Use this local hex-dump function; not `mg_hexdump()`.
 */
static void hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what)
{
  static char hex_digits[] = "0123456789ABCDEF";
  size_t i, idx, count = 0;
  char   lbuf [200];
  int    lbuf_idx;

  EnterCriticalSection (&Modes.print_mutex);

  LOG_STDOUT ("sdrconnect.c(%u): len: %zd%s\n", line, len, what ? what : "");

  for (idx = 0; len > 0; len -= count)
  {
    count = (len > 16) ? 16 : len;
    lbuf_idx = snprintf (lbuf, sizeof(lbuf), "%4.4X  ", (int)idx);

    for (i = 0; i < count; i++)
    {
      lbuf [lbuf_idx++] = hex_digits [buf[i] >> 4];
      lbuf [lbuf_idx++] = hex_digits [buf[i] & 15];
      lbuf [lbuf_idx++] = ' ';
    }
    for ( ; i < 16; i++)
    {
      lbuf [lbuf_idx++] = ' ';
      lbuf [lbuf_idx++] = ' ';
      lbuf [lbuf_idx++] = ' ';
    }
    lbuf [lbuf_idx++] = '|';

    for (i = 0; i < count; i++)
    {
      if (buf[i] < ' ' || buf[i] >= 0x7F)
           lbuf [lbuf_idx++] = '.';
      else lbuf [lbuf_idx++] = buf [i];
    }

    for ( ; i < 16; i++)
        lbuf [lbuf_idx++] = ' ';

    lbuf [lbuf_idx++] = '|';
    lbuf [lbuf_idx++] = '\0';

    LOG_STDOUT ("!%s\n", lbuf);

    buf += count;
    idx += count;
  }

  LOG_STDOUT ("! \n");

  LeaveCriticalSection (&Modes.print_mutex);
}

