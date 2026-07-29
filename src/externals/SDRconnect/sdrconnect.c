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

#define SET_PROPERTY(what, str_value)                       \
        websock_printf ("\"event_type\": \"set_property\"," \
                        "\"property\": \"%s\","             \
                        "\"value\": \"%s\"",                \
                        what, str_value)

#define GET_PROPERTY(what) \
        websock_printf ("\"event_type\": \"get_property\"," \
                        "\"property\": \"%s\"", what)

#define STREAM_ENABLE(enable)                                   \
        websock_printf ("\"event_type\": \"iq_stream_enable\"," \
                        "\"property\": \"\","                   \
                        "\"value\": \"%s\"", enable)

#define SPECTRUM_ENABLE(enable)                                \
        websock_printf ("\"event_type\": \"spectrum_enable\"," \
                        "\"property\": \"\","                  \
                        "\"value\": \"%s\"", enable)

#define SET_FREQUENCY(which, freq)  \
        SET_PROPERTY (which, _ui64toa(freq, g_data.tmp_str, 10))

#define SET_SAMPLE_RATE(rate) \
        SET_PROPERTY ("device_sample_rate", _ultoa(rate, g_data.tmp_str, 10))

typedef void (*state_func) (const char *key, const char *val);

typedef struct SDRConnect_priv {
        state_func json_state;
        char       tmp_str [20];
        bool       got_ws_open;
        bool       done_init;
        bool       stream_init;
        uint64_t   num_ws_events;

        /* Filled by `json_parse()`:
         */
        char       api_version [20];
        char       valid_devices [300];
        char       active_device [20];
        char       active_antenna [20];
        uint64_t   device_center_frequency;
        uint64_t   device_vfo_frequency;
        double     device_sample_rate;
        double     signal_snr;
        double     signal_power;
      } SDRConnect_priv;

static SDRConnect_priv g_data;

static void websock_printf (const char *fmt, ...) ATTR_PRINTF(1, 2);
static void hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what);

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

static void state_api_version (const char *key, const char *val);
static void state_valid_devices (const char *key, const char *val);
static void state_active_device (const char *key, const char *val);
static void state_active_antenna (const char *key, const char *val);
static void state_signal_snr (const char *key, const char *val);
static void state_signal_power (const char *key, const char *val);
static void state_device_center_frequency (const char *key, const char *val);
static void state_device_vfo_frequency (const char *key, const char *val);
static void state_device_sample_rate (const char *key, const char *val);

static void state_normal (const char *key, const char *val)
{
  if (!stricmp(val, "api_version"))
     g_data.json_state = state_api_version;

  else if (!stricmp(val, "active_device"))
     g_data.json_state = state_active_device;

  else if (!stricmp(val, "valid_devices"))
     g_data.json_state = state_valid_devices;

  else if (!stricmp(val, "active_antenna"))
     g_data.json_state = state_active_antenna;

  else if (!stricmp(val, "signal_snr"))
     g_data.json_state = state_signal_snr;

  else if (!stricmp(val, "signal_power"))
     g_data.json_state = state_signal_power;

  else if (!stricmp(val, "device_center_frequency"))
     g_data.json_state = state_device_center_frequency;

  else if (!stricmp(val, "device_vfo_frequency"))
     g_data.json_state = state_device_vfo_frequency;

  else if (!stricmp(val, "device_sample_rate"))
     g_data.json_state = state_device_sample_rate;
  (void) key;
}

static void state_api_version (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    strncpy (g_data.api_version, val, sizeof(g_data.api_version));
    TRACE ("api_version: '%s'\n", g_data.api_version);
    g_data.json_state = state_normal;
  }
}

static void state_active_device (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    strncpy (g_data.active_device, val, sizeof(g_data.active_device));
    TRACE ("active_device: '%s'\n", g_data.active_device);
    g_data.json_state = state_normal;
  }
}

static void state_valid_devices (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    strncpy (g_data.valid_devices, val, sizeof(g_data.valid_devices));
    TRACE ("valid_devices: '%s'\n", g_data.valid_devices);
    g_data.json_state = state_normal;
  }
}

static void state_active_antenna (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    strncpy (g_data.active_antenna, val, sizeof(g_data.active_antenna));
    TRACE ("active_antenna: '%s'\n", g_data.active_antenna);
    g_data.json_state = state_normal;
  }
}

/**
 * Since floating-point JSON-values contains `","`.
 * Convert `,` to `.` and return a `double`.
 */
static double val_strtof (const char *val)
{
  char copy [30], *p;

  assert (strlen(val) < sizeof(copy)-1);
  strcpy (copy, val);
  p = strchr (copy, ',');
  if (p)
     *p = '.';
  return strtof (copy, NULL);
}

static void state_signal_snr (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.signal_snr = val_strtof (val);
    TRACE ("signal_snr:   %.6f dB\n", g_data.signal_snr);
    g_data.json_state = state_normal;
  }
}

static void state_signal_power (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.signal_power = val_strtof (val);
    TRACE ("signal_power: %.6f dBm\n", g_data.signal_power);
    g_data.json_state = state_normal;
  }
}

static void state_device_center_frequency (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.device_center_frequency = strtoull (val, NULL, 10);
    TRACE ("device_center_frequency: %llu Hz\n", g_data.device_center_frequency);
    g_data.json_state = state_normal;
  }
}

static void state_device_vfo_frequency (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.device_vfo_frequency = strtoull (val, NULL, 10);
    TRACE ("device_vfo_frequency: %llu Hz\n", g_data.device_vfo_frequency);
    g_data.json_state = state_normal;
  }
}

static void state_device_sample_rate (const char *key, const char *val)
{
  if (!stricmp(key, "value"))
  {
    g_data.device_sample_rate = val_strtof (val);
    TRACE ("device_sample_rate: %.2f\n", g_data.device_sample_rate);
    g_data.json_state = state_normal;
  }
}


/**
 * Rewritten from `json_scan()` in Mongoose's `test/unit_test.c`.
 * But our JSON-data is not recursive (`depth == 0`).
 */
static void json_parse (mg_str json)
{
  int num = 0;
  int idx = mg_json_get (json, "$", &num);

  if (json.buf[idx] == '{')   /* Iterate over elems */
  {
    mg_str key, val;
    mg_str sub = mg_str_n (json.buf + idx, (size_t)num);
    size_t ofs = 0;

    g_data.json_state = state_normal;

    while ((ofs = mg_json_next(sub, ofs, &key, &val)) > 0)
    {
      char _key [30];
      char _val [300];   /* "valid_devices" can be rather long */

      assert (key.buf[0] == '"');
      assert (val.buf[0] == '"');
      assert (key.len - 2 < sizeof(_key));
      assert (val.len - 2 < sizeof(_val));

      strncpy (_key, &key.buf[1], key.len - 2);
      _key [key.len - 2] = '\0';

      strncpy (_val, &val.buf[1], val.len - 2);
      _val [val.len - 2] = '\0';

      /* Add values to the `g_data` structure
       */
      (*g_data.json_state) (_key, _val);
    }
  }
}

static void text_handler (const mg_ws_message *ws)
{
  char   dbg_buf [200];
  mg_str json = ws->data;
  mg_str event_type = mg_json_get_tok (json, "$.event_type");
  mg_str property   = mg_json_get_tok (json, "$.property");

  snprintf (dbg_buf, sizeof(dbg_buf),
            ", recv: event_type: %.*s, property: %.*s",
            (int)event_type.len, event_type.buf,
            (int)property.len,   property.buf);

  HEX_DUMP (json.buf, json.len, dbg_buf);

  json_parse (json);

  if (!g_data.stream_init)
  {
    g_data.stream_init = true;
    STREAM_ENABLE ("true");
    SPECTRUM_ENABLE ("false");
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
 * Call-back from net_io.c on all WebSock events:
 *   if (ev == MG_EV_WS_OPEN) do nothing.
 *   if (ev == MG_EV_WS_MSG) send requests and handle responses.
 *   if (ev == MG_EV_WS_CTL) do nothing.
 */
static void websock_handler (enum mg_event ev, const mg_ws_message *ws)
{
  bool    is_text   = false;
  bool    is_binary = false;
  uint8_t opcode = (ws->flags & 15);

  g_data.num_ws_events++;

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

  if (ev == MG_EV_WS_MSG)
       TRACE2 ("ev: %s, op: %s, len %zd\n", net_ev_name(ev), websock_opcode(ws), ws->data.len);
  else TRACE  ("ev: %s, op: %s, len %zd\n", net_ev_name(ev), websock_opcode(ws), ws->data.len);

  if (ev == MG_EV_WS_OPEN) /* Let mg_ws_connect() finish first */
  {
    g_data.got_ws_open = true;
    return;
  }

  if (!g_data.done_init && g_data.got_ws_open)
  {
    GET_PROPERTY ("api_version");
    GET_PROPERTY ("active_device");
    GET_PROPERTY ("valid_devices");
    GET_PROPERTY ("active_antenna");
    SET_FREQUENCY ("device_center_frequency", Modes.freq);
    SET_SAMPLE_RATE (Modes.sample_rate);
    g_data.done_init = true;
    return;
  }

  if (is_binary)
  {
    if (g_data.stream_init)
       binary_handler (ws);
    return;
  }

  if (is_text)
     text_handler (ws);
}

void sdrconnect_init (void)
{
  memset (&g_data, '\0', sizeof(g_data));
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
    LOG_STDOUT ("    %10llu bytes recv.\n", Modes.stat.bytes_recv [s]);
    LOG_STDOUT ("    %10llu bytes sent.\n", Modes.stat.bytes_sent [s]);
    LOG_STDOUT ("    %10llu IQ-samples recv.\n", Modes.stat.websock.samples_recv);
    LOG_STDOUT ("    %10llu TEXT recv.\n", Modes.stat.websock.text);
    LOG_STDOUT ("    %10llu PING recv.\n", Modes.stat.websock.ping);
    LOG_STDOUT ("    %10llu PONG sent.\n", Modes.stat.websock.pong);
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
    if (g_data.num_ws_events >= 20)
       Modes.exit = true;
  }
  sdrconnect_exit();
  sdrconnect_stats();
}

/**
 * Run 'jq.exe' on the JSON-data to verify it:
 * ```
 * jq.exe < %TEMP%\dump1090\sdrconnect-N.json > NUL
 * ```
 */
static void verify_json (const char *data)
{
  static int file_num = 0;
  char  tmp_file [MAX_PATH];

  snprintf (tmp_file, sizeof(tmp_file), "%s\\sdrconnect-%d.json", Modes.tmp_dir, ++file_num);
  FILE *f = fopen (tmp_file, "w+b");
  if (f)
  {
    char jq_cmd [MAX_PATH + 20];

    fwrite (data, 1, strlen(data), f);
    fclose (f);
    snprintf (jq_cmd, sizeof(jq_cmd), "jq.exe < %s > NUL", tmp_file);
    if (system(jq_cmd) != 0)
       TRACE ("File %s not OK.\n\n", tmp_file);
  }
}

/*
 * Local wrappers for `mg_ws_printf()` to ease tracing and hex-dumping
 */
static void websock_vprintf (const char *fmt, va_list args)
{
  char   buf [200];
  size_t len;

  buf [0] = '{';
  len = (int) vsnprintf (buf+1, sizeof(buf)-2, fmt, args);
  buf [1+len] = '}';
  buf [2+len] = '\0';  /* for verify_json() */
  len += 2;

  if (Modes.debug & DEBUG_WEBSOCKET)
     verify_json (buf);

  HEX_DUMP (buf, len, ", send");

  if (mg_ws_send (Modes.websock_in, buf, len, WEBSOCKET_OP_TEXT) < len)
     TRACE ("mg_ws_send() failed to send: %zd bytes\n", len);
}

static void websock_printf (const char *fmt, ...)
{
  va_list args;

  va_start (args, fmt);
  websock_vprintf (fmt, args);
  va_end (args);
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

