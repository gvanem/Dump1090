/**
 * \file    sdrconnect.c
 * \ingroup Samplers
 * \brief   The Web-socket interface for remote SDRConnect server.
 */
#include "misc.h"
#include "net_io.h"
#include "sdrconnect.h"

#undef  TRACE
#define TRACE(fmt, ...)                          \
        do {                                     \
          if (Modes.debug & DEBUG_WEBSOCKET)     \
             printf ("sdrconnect.c(%u): " fmt,   \
                     __LINE__,  ## __VA_ARGS__); \
        } while (0)

#define HEX_DUMP(buf, len, what)              \
        do {                                  \
          if (Modes.debug & DEBUG_WEBSOCKET)  \
             hex_dump ((const uint8_t*)(buf), \
                       len, __LINE__, what);  \
        } while (0)

static uint64_t stats_iq_samples;
static uint64_t num_ws_events = 0;

static bool done_init   = false;
static bool stream_init = false;

static void        get_api_version (void);
static void        set_frequency (const char *which, uint64_t freq);
static void        set_sample_rate (uint32_t rate);
static void        stream_enable (const char *enable);
static void        spectrum_enable (const char *enable);
static const char *ws_opcode (const mg_ws_message *ws);
static void        hex_dump (const uint8_t *buf, size_t len,  unsigned line, const char *what);

static void binary_handler (const mg_ws_message *ws)
{
  uint16_t bin_type = *(const uint16_t*) &ws->data.buf [0];
  uint8_t *data;
  uint32_t len;

  if (bin_type == 2) /* Signed 16-bit interleaved IQ (IQIQ) */
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
    Modes.stat.samples_recv_sdrconnect += ws->data.len / Modes.bytes_per_sample;
  }
}

static void text_handler (enum mg_event ev, const mg_ws_message *ws)
{
  char ev_buf [200] = "";

  mg_str event_type = mg_json_get_tok (ws->data, "$.event_type");
  mg_str property   = mg_json_get_tok (ws->data, "$.property");
  mg_str resp       = mg_json_get_tok (ws->data, "$.get_property_response");

  snprintf (ev_buf, sizeof(ev_buf),
            ", ev: %s, event_type: %.*s, property: %.*s",
            net_ev_name(ev),
            (int)event_type.len, event_type.buf,
            (int)property.len,   property.buf);

  HEX_DUMP (ws->data.buf, ws->data.len, ev_buf);

  if (resp.len)
  {
    mg_str value = mg_json_get_tok (ws->data, "$.value");

     TRACE ("get_property_response: %.*s, value: %.*s\n",
            (int)resp.len,  resp.buf,
            (int)value.len, value.buf);
  }

  if (!done_init)
  {
    get_api_version();
    set_frequency ("device_center_frequency", Modes.freq);
    set_frequency ("device_vfo_frequency", Modes.freq);
    done_init = true;
  }
  else if (!stream_init)
  {
    stream_init = true;
    stream_enable ("true");
    spectrum_enable ("false");
  }
}

/**
 * Call-back from net_io.c
 */
static void ws_handler (enum mg_event ev, const mg_ws_message *ws)
{
  char ev_buf [200] = "";
  bool is_text   = ((ws->flags & 15) == WEBSOCKET_OP_TEXT);
  bool is_binary = ((ws->flags & 15) == WEBSOCKET_OP_BINARY) && (ws->data.len > 2);

  num_ws_events++;

  TRACE ("num_ws_events: %llu, op: %s, len %zd\n",
          num_ws_events, ws_opcode(ws), ws->data.len);

  if (is_binary)
  {
    if (stream_init)
       binary_handler (ws);
    return;
  }

  if (is_text)
     text_handler (ev, ws);
}

void sdrconnect_init (void)
{
//Modes.net = true;
  net_ws_handler = ws_handler;
}

void sdrconnect_exit (void)
{
//if (Modes.websock_in)
     stream_enable ("false");
}

void sdrconnect_stats (void)
{
  if (net_stat_common(MODES_NET_SERVICE_WEBSOCK))
  {
    LOG_STDOUT ("    %8llu bytes sent.\n", Modes.stat.bytes_sent [MODES_NET_SERVICE_WEBSOCK]);
    LOG_STDOUT ("    %8llu IQ-samples recv.\n", stats_iq_samples);
  }
}

/*
 * If no samples received, no statistics to show.
 */
void sdrconnect_no_stats (intptr_t service)
{
  if (service == MODES_NET_SERVICE_WEBSOCK && Modes.stat.samples_recv_sdrconnect == 0)
     Modes.no_stats = true;
}

void sdrconnect_tests (void)
{
  TRACE ("%s() called.\7\n", __FUNCTION__);

  sdrconnect_init();
  if (!net_init())
     return;

  while (!Modes.exit)
  {
    /*
     * Poll and handle network events
     */
    background_tasks();
    if (num_ws_events > 20)
       Modes.exit = true;
  }
  sdrconnect_exit();
  sdrconnect_stats();
}

static void set_property (const char *what, const char *value)
{
  char buf [100];

  mg_snprintf (buf, sizeof(buf),
               "{%m:%m,%m:%m"}",
               MG_ESC("event_type"), MG_ESC("set_property"),
               MG_ESC("property"),   MG_ESC(what),
               MG_ESC("value"),      MG_ESC(value));

  mg_ws_printf (Modes.websock_in, WEBSOCKET_OP_TEXT, buf);
  TRACE ("%s\n", buf);
}

static void get_property (const char *what)
{
  char buf [100];

  mg_snprintf (buf, sizeof(buf),
               "{%m:%m,%m:%m"}",
               MG_ESC("event_type"), MG_ESC("get_property"),
               MG_ESC("property"),   MG_ESC(what));

  mg_ws_printf (Modes.websock_in, WEBSOCKET_OP_TEXT, buf);
  TRACE ("%s\n", buf);
}

static void get_api_version (void)
{
  get_property ("get_api_version");
}

static void set_frequency (const char *which, uint64_t freq)
{
  char str [20];
  set_property (which, _ui64toa(freq, str, 10));
}

static void set_sample_rate (uint32_t rate)
{
  char str [10];
  set_property ("device_sample_rate", _ultoa(rate, str, 10));
}

static void stream_enable (const char *enable)
{
  char buf [100];

  mg_snprintf (buf, sizeof(buf),
               "{%m:%m,%m:%m,%m:%m}",
               MG_ESC("event_type"), MG_ESC("iq_stream_enable"),
               MG_ESC("property"),   MG_ESC(""),
               MG_ESC("value"),      MG_ESC(enable));
  mg_ws_printf (Modes.websock_in, WEBSOCKET_OP_TEXT, buf);
  TRACE ("%s\n", buf);
}

static void spectrum_enable (const char *enable)
{
  char buf [100];

  mg_snprintf (buf, sizeof(buf),
               "{%m:%m,%m:%m,%m:%m}",
               MG_ESC("event_type"), MG_ESC("spectrum_enable"),
               MG_ESC("property"),   MG_ESC(""),
               MG_ESC("value"),      MG_ESC(enable));

  mg_ws_printf (Modes.websock_in, WEBSOCKET_OP_TEXT, buf);
  TRACE ("%s\n", buf);
}

static const char *ws_opcode (const mg_ws_message *ws)
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
 * Use this local hex-dump function; not `mg_hexdump()`.
 */
static void hex_dump (const uint8_t *buf, size_t len,  unsigned line, const char *what)
{
  static char hex_digits[] = "0123456789ABCDEF";
//const uint8_t *buf = (const uint8_t*) _buf;
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

