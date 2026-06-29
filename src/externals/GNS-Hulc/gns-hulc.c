/**\file    gns-hulc.c
 * \ingroup GNS-HULC
 *
 * Decoder for GNS / HULC protocol.
 *
 * The tricky part of this is to turn the serial data into Packets
 * or \ref RX_packet. The Start-of-Packet (SOP=0x1A) must be found
 * before processing a RX-packet further. So a 0x1A,0x1A in the middle
 * of a packet MUST be converted to a single 0x1A; i.e. unstuffing.
 *
 * All this resembles \ref HDLC or \ref SLIP
 *
 * where a SOP is the frame delimiter.
 *
 * But unlike HDLC, this GNS-HULC protocol does *NOT* end with a frame delimiter
 * and have variable length of packets.
 *
 * \link     https://www.gns-electronics.de/flight-information-for-avionics
 * \ref HDLC https://en.wikipedia.org/wiki/High-Level_Data_Link_Control
 * \ref SLIP https://en.wikipedia.org/wiki/Serial_Line_Internet_Protocol
 */
#define GNS_FILE  "gns-hulc.c"

#include "GNS-Hulc/gns-private.h"
#include "GNS-Hulc/gns-hulc.h"

#define IOBUF_SIZE         2048

#define GPS_HAVE_FIX_LOW   6
#define GPS_HAVE_FIX_HIGH  15
#define GPS_HDOP_WORST     20

GNS_priv g_data;

static void        set_defaults (void);
static bool        send_option (const uint8_t *msg, int msg_sz, const char *what);
static void        send_beast_options (void);
static void        send_firmware_req (void *unused);

static void        decode_msg_32 (const RX_packet *pkt, uint32_t msg_len);
static void        decode_msg_33 (const RX_packet *pkt, uint32_t msg_len);
static void        decode_msg_34 (const RX_packet *pkt, uint32_t msg_len);
static void        decode_msg_48 (const RX_packet *pkt, uint32_t msg_len);
static bool        pkt_enqueue (const RX_packet *pkt);
static uint32_t    pkt_list_len (void);
static void        pkt_free (void);
static double      binary_angle (int32_t angle);
static char       *get_name_space (uint16_t port);
static const char *ch_str (int ch);
static void        hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what);

static char hex_digits [] = "0123456789ABCDEF";

static void state_get_sync (uint8_t ch);
static void state_put_ch (uint8_t ch);
static void state_got_1A (uint8_t ch);

/**
 * \def HEX_DUMP1()
 * \def HEX_DUMP2()
 *
 * For development; dumping of various buffers.
 */
#define HEX_DUMP1(buf, len, what)                 \
        do {                                      \
          if (Modes.debug & DEBUG_GNS_HULC)       \
             hex_dump (buf, len, __LINE__, what); \
        } while (0)

#define HEX_DUMP2(buf, len, what)                 \
        do {                                      \
          if (Modes.debug & DEBUG_GNS_HULC2)      \
             hex_dump (buf, len, __LINE__, what); \
        } while (0)


/*
 * Config-callback for "hulc-baud = x".
 */
bool gns_hulc_set_baud (const char *arg)
{
  char    *end;
  uint64_t val = strtoull (arg, &end, 10);

  set_defaults();

  if (end > arg)
     g_data.COM.baud_rate = (uint32_t) val;

  /* Not fatal, but give a warning since this rate will probably not work
   */
  if (g_data.COM.baud_rate != COM_BAUD_RATE)
     cfg_illegal_val ("hulc-baud", arg);
  return (true);
}

/*
 * Config-callback for "hulc-beastmode = yes|no".
 */
bool gns_hulc_set_beast (const char *arg)
{
  set_defaults();

  g_data.Beast.enable = cfg_true (arg);
  return (true);
}

/**
 * Config-callback for "hulc-port = x".
 * Assigns `Modes.gns_hulc.port` only once.
 */
bool gns_hulc_set_port (const char *arg)
{
  char    *end;
  uint64_t val;

  set_defaults();

  if (g_data.COM.port_set)
  {
    DEBUG1 ("Already have Modes.gns_hulc.port: %d\n", Modes.gns_hulc.port);
    return (true);
  }

  /* We were called from `parse_cmd_line()` with `--device gns-hulcN`.
   * Allow `N > 9` here.
   */
  if (str_startswith(arg, "gns-hulc"))
  {
    if (strlen(arg) > 8)
    {
      Modes.gns_hulc.port = atoi (arg + 8);
      g_data.COM.port_set = true;
    }
    else
      Modes.gns_hulc.port = GNS_HULC_DEFAULT_COMPORT;
  }
  else
  {
    if (!strnicmp(arg, "COM", 3))
       return gns_hulc_set_port (arg + 3);

    val = strtoull (arg, &end, 10);
    if (end > arg)
         Modes.gns_hulc.port = (uint32_t) val;
    else cfg_illegal_val ("hulc-port", arg);
  }

  free (Modes.gns_hulc.name);
  Modes.gns_hulc.name = mg_mprintf ("HULC-%d", Modes.gns_hulc.port);
  return (true);
}

/**
 * Config-callback for "hulc-gps-enable = yes|no".
 */
bool gns_hulc_gps_enable (const char *arg)
{
  set_defaults();
  g_data.GPS.enable = cfg_true (arg);
  return (true);
}

/**
 * Config-callback for "hulc-bufsize = <uint32_t value>".
 * Lowest minimum is `IOBUF_SIZE`.
 */
bool gns_hulc_set_buf_size (const char *arg)
{
  char   *end;
  int64_t val = strtoll (arg, &end, 10);

  set_defaults();

  if (end > arg && val > 0 && val < UINT_MAX)
  {
    COM_RX_SIZE = max (val, IOBUF_SIZE);
    DEBUG1 ("sio_buf_size: %d\n", COM_RX_SIZE);
  }
  else
    cfg_illegal_val ("hulc-bufsize", arg);
  return (true);
}

/**
 * Config-callback for "hulc-poll-ms = <millisec>".
 */
bool gns_hulc_set_poll_ms (const char *arg)
{
  char   *end;
  int64_t val = strtoll (arg, &end, 10);

  set_defaults();

  if (end > arg && val > 0 && val < UINT_MAX)
  {
    Modes.gns_hulc.poll_ms = val;
    DEBUG1 ("Modes.gns_hulc.poll_ms: %u\n", Modes.gns_hulc.poll_ms);
  }
  else
    cfg_illegal_val ("hulc-poll-ms", arg);
  return (true);
}

/**
 * Returns last good GPS position / altitude / number-of-sat / HDOP
 * from HULC status message.
 */
bool gns_hulc_gps_info (pos_t *pos, int *altitude, int *num, double *hdop)
{
  if (!g_data.GPS.have_fix)
     return (false);

  if (pos)
  {
    pos->lat = g_data.GPS.pos.lat;
    pos->lon = g_data.GPS.pos.lon;
  }

  if (altitude)
     *altitude  = g_data.GPS.altitude;

  if (num)
     *num  = g_data.GPS.satellites;

  if (hdop)
     *hdop = g_data.GPS.HDOP;
  return (true);
}

/**
 * Initialise all GNS-HULC stuff once.
 */
HANDLE gns_hulc_init (uint16_t port)
{
  HANDLE hnd;

  set_defaults();

  g_data.sio_buf = calloc (COM_RX_SIZE+1, 1);

  InitializeCriticalSection (&g_data.crit);

  strncpy (g_data.COM.name_space, get_name_space(port), sizeof(g_data.COM.name_space)-1);

  /* "\\.\COMx".
   */
  snprintf (g_data.COM.dev_name, sizeof(g_data.COM.dev_name), "\\\\.\\COM%d", port);

  DEBUG2 ("Modes.gns_hulc.port:  %u\n", Modes.gns_hulc.port);
  DEBUG2 ("Modes.gns_hulc.name:  %s\n", Modes.gns_hulc.name ? Modes.gns_hulc.name : "<none>");
  DEBUG2 ("Modes.selected_dev:   '%s'\n", Modes.selected_dev ? Modes.selected_dev  : "<none>");
  DEBUG2 ("name_space:           '%s'\n", g_data.COM.name_space);

  hnd = CreateFileA (g_data.COM.dev_name, GENERIC_READ | GENERIC_WRITE,
                     0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  if (hnd == INVALID_HANDLE_VALUE)
  {
    LOG_STDERR ("Error opening %s: %s\n", g_data.COM.dev_name, win_strerror(GetLastError()));
    gns_hulc_exit (hnd);
    return (NULL);
  }

  if (!COM_setup(hnd))
  {
    LOG_STDERR ("COM_setup (\"%s\") failed.\n", g_data.COM.dev_name);
    gns_hulc_exit (hnd);
    return (NULL);
  }

  LOG_STDERR ("Running GNS %s on `%s' (%s)\n",
              g_data.Beast.enable ? "Beast" : "HULC",
              g_data.COM.dev_name, g_data.COM.name_space);


  Modes.gns_hulc.handle = hnd;

  if (g_data.Beast.enable)
       send_beast_options();
//else send_firmware_req (NULL);

  g_data.pkt_current.msg_len = 0;
  g_data.pkt_current.msg_marker = MARKER_MAGIC;
  g_data.got_x1A = false;
  g_data.old_ch  = -1;

  DEBUG1 ("ch: --, %s() -> state_get_sync()\n", __FUNCTION__);
  g_data.state = state_get_sync;

  return (hnd);
}

/**
 * Exit function called from main() if gns_hulc_succeded.
 * Otherwise it's called from above.
 */
void gns_hulc_exit (HANDLE hnd)
{
  if (hnd)
  {
    Sleep (GNS_HULC_SLEEP);
    CloseHandle (hnd);
  }

  free (Modes.gns_hulc.name);
  free (g_data.sio_buf);
  pkt_free();

  DeleteCriticalSection (&g_data.crit);
  Modes.gns_hulc.name = NULL;
  g_data.sio_buf = NULL;
}

/**
 * Show statistics. Called from main() before exit.
 */
void gns_hulc_stats (void)
{
  uint64_t sum = g_data.stat.rx_packets_32 + g_data.stat.rx_packets_33 +
                 g_data.stat.rx_packets_34 + g_data.stat.rx_packets_48 +
                 g_data.stat.tx_bytes   + g_data.stat.rx_errors +
                 g_data.stat.tx_packets + g_data.stat.tx_errors;

  if (sum > 0ULL)
  {
    LOG_STDOUT ("! \n");
    LOG_STDOUT ("GNS-HULC statistics:\n");
    LOG_STDOUT (" %8llu RX-packets-32.\n",     g_data.stat.rx_packets_32);     /* Mode-S Short Squitter packets */
    LOG_STDOUT (" %8llu RX-packets-33.\n",     g_data.stat.rx_packets_33);     /* Mode-S Extender Squitter packets */
    LOG_STDOUT (" %8llu RX-packets-34.\n",     g_data.stat.rx_packets_34);     /* Unknown 33 */
    LOG_STDOUT (" %8llu RX-packets-48.\n",     g_data.stat.rx_packets_48);     /* HULC Status packets */
    LOG_STDOUT (" %8llu RX-unknown.\n",        g_data.stat.rx_packets_unknown);
//  LOG_STDOUT (" %8llu RX-errors.\n",         g_data.stat.rx_errors);
    LOG_STDOUT (" %8llu TX-packets.\n",        g_data.stat.tx_packets);
//  LOG_STDOUT (" %8llu TX-errors.\n",         g_data.stat.tx_errors);
//  LOG_STDOUT (" %8llu Enqueued packets.\n",  g_data.stat.pkt_enqueued);
    LOG_STDOUT (" %8llu RX-junk-data.\n",      g_data.stat.pkt_junk);
//  LOG_STDOUT (" %8llu Sleep()-poll.\n",      g_data.stat.pkt_list_sleep);
//  LOG_STDOUT (" %8llu Out-of-memory.\n",     g_data.stat.pkt_OOM);
//  LOG_STDOUT (" %8llu Unstuffed packets.\n", g_data.stat.pkt_unstuffed);

    double percent = 100.0 * g_data.stat.pkt_too_short /
                     (g_data.stat.rx_packets_33 + g_data.stat.rx_packets_34 +
                      g_data.stat.rx_packets_48 + g_data.stat.rx_packets_unknown);

    LOG_STDOUT (" %8llu Too short packets (%.1f%%).\n", g_data.stat.pkt_too_short, percent);
//  LOG_STDOUT (" %8llu Too big packets.\n",  g_data.stat.pkt_too_big);
//  LOG_STDOUT (" %8llu Old data packets.\n", g_data.stat.old_data_cnt);
//  LOG_STDOUT (" %8llu Marker destroyed.\n", g_data.stat.pkt_bad_marker);
    LOG_STDOUT (" %8u mode_S errors.\n",      g_data.stat.mode_S_errors);
    LOG_STDOUT (" %8u pkt_max_len.\n",        g_data.stat.pkt_max_len);
    LOG_STDOUT (" %8llu Dequeued packets.\n", g_data.stat.pkt_dequeued);
    LOG_STDOUT ("          Enqueued bytes %llu, RX-bytes: %llu\n", g_data.stat.pkt_enqueued_bytes, g_data.stat.rx_bytes);
  }
}

uint64_t gns_hulc_junk (void)
{
  return (g_data.stat.pkt_junk);
}

uint64_t gns_hulc_too_short (void)
{
  return (g_data.stat.pkt_too_short);
}

uint64_t gns_hulc_too_big (void)
{
  return (g_data.stat.pkt_too_big);
}

uint64_t gns_hulc_unknown (void)
{
  return (g_data.stat.rx_packets_unknown);
}

bool gns_hulc_gps_detected (void)
{
  return (g_data.GPS.detected);
}

/**
 * Can we use GNS-Hulc GPS position to replace `Modes.home_pos`?
 */
bool gns_hulc_gps_enabled (void)
{
  return (g_data.GPS.detected && g_data.GPS.enable && g_data.GPS.have_fix);
}

void gns_hulc_tests (void)
{
  LOG_STDERR ("%s() does nothing yet\n", __FUNCTION__);
}

/**
 * Return the name of a state-function. Handy for tracing.
 */
const char *state_name (state_func f)
{
  return (f == state_get_sync ? "state_get_sync" :
          f == state_put_ch   ? "state_put_ch  " :
          f == state_got_1A   ? "state_got_1A  " : "?");
}

/*
 * Wait for a single ch == x1A to mark the *END* of a packet.
 * Then enter state_put_ch(). Otherwise discard ch.
 *
 * In the extremely seldom case, that the FSM starts with x1A,x1A
 * then wait for another single x1A marking the packet.
 */
static void state_get_sync (uint8_t ch)
{
  if (ch == 0x1A)
  {
    g_data.pkt_current.msg [0] = 0x1A;
    g_data.pkt_current.msg_len = 1;
    g_data.old_ch  = -1;      /* we do not know */
    g_data.got_x1A = true;

    DEBUG1 ("got x1A sync\n");
    g_data.state = state_put_ch;
  }
}

/*
 * Unstuff or append a byte to `g_data.pkt_current.msg []`
 */
static void state_put_ch (uint8_t ch)
{
  if (ch == 0x1A)
  {
    g_data.state = state_got_1A;   /* possibly an stuffed / ESCaped byte or a SOP ahead */
  }
  else
  {
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = ch;

    if (g_data.pkt_current.msg_len >= sizeof(g_data.pkt_current.msg) - 1)
       DEBUG2 ("Reached limit (1)\n");
    if (g_data.pkt_current.msg_marker != MARKER_MAGIC)
       DEBUG2 ("Marker destroyed (1), msg_len: %u\n", g_data.pkt_current.msg_len);
  }
}

/*
 * Got x1A, check if this ch == x1A.
 * If not, enqueue current and start a new packet.
 */
static void state_got_1A (uint8_t ch)
{
  if (ch == 0x1A && g_data.old_ch == 0x1A)
  {
    if (g_data.pkt_current.msg_len < 3)
       DEBUG2 ("Too short (1)\n");

    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;

    if (g_data.pkt_current.msg_len >= sizeof(g_data.pkt_current.msg) - 1)
       DEBUG2 ("Reached limit (2)\n");
    if (g_data.pkt_current.msg_marker != MARKER_MAGIC)
       DEBUG2 ("Marker destroyed (2), msg_len: %u\n", g_data.pkt_current.msg_len);

    assert (g_data.pkt_current.msg_len < sizeof(g_data.pkt_current.msg));

    g_data.state = state_put_ch;
  }
  else
  {
    DEBUG2 (" ch: %s old_ch: %s pkt_current.msg_len: %u, calling pkt_enqueue()\n",
            ch_str(ch), ch_str(g_data.old_ch), g_data.pkt_current.msg_len);

    pkt_enqueue (&g_data.pkt_current);

    g_data.pkt_current.msg [0] = 0x1A;
    g_data.pkt_current.msg [1] = ch;    /* First `ch` in next `pkt_current.msg` */
    g_data.pkt_current.msg_len = 2;
    g_data.pkt_current.msg_marker = MARKER_MAGIC;

    g_data.state = state_put_ch;  /* Or state_skip_2 ?? */
  }
}

/**
 * Set all defaults once
 */
static void set_defaults (void)
{
  static bool done = false;

  if (done)
     return;

  done = true;

  Modes.gns_hulc.port    = GNS_HULC_DEFAULT_COMPORT;
  Modes.gns_hulc.poll_ms = GNS_HULC_SLEEP;

  g_data.sio_buf_size   = IOBUF_SIZE;   /* 2 kByte */
  g_data.GPS.have_fix   = false;
  g_data.GPS.enable     = false;
  g_data.GPS.satellites = 0;

  g_data.COM.RTS_ctrl   = RTS_CONTROL_ENABLE;
  g_data.COM.DTR_ctrl   = DTR_CONTROL_ENABLE;
  g_data.COM.baud_rate  = COM_BAUD_RATE;

  g_data.Beast.enable         = false;
  g_data.Beast.filter_DF045   = false;
  g_data.Beast.filter_DF1117  = false;
  g_data.Beast.mode_AC        = Modes.mode_AC = false;
  g_data.Beast.mlat_timestamp = true;
  g_data.Beast.FEC            = true;
  g_data.Beast.CRC            = true;
}

static bool decoder_header (const header_32_33 *hdr, uint64_t *ts_msec, const char *func)
{
  static bool done = false;
  static LONG timezone = 0;    /* in minutes */
  static LONG dst_adjust = 0;  /* in minutes */
  static LONG UTC_adjust = 0;  /* in minutes */
  const char *junk = NULL;     /* got local junk data? */
  char        ts_buf [100];    /* timestamp info */

  *ts_msec = 0ULL;

  if  (!done)
  {
    TIME_ZONE_INFORMATION tz_info;
    DWORD rc = GetTimeZoneInformation (&tz_info);

    if (rc == TIME_ZONE_ID_UNKNOWN || rc == TIME_ZONE_ID_STANDARD || rc == TIME_ZONE_ID_DAYLIGHT)
    {
      timezone   = tz_info.Bias + tz_info.StandardBias;
      dst_adjust = tz_info.StandardBias - tz_info.DaylightBias;
      UTC_adjust = dst_adjust - timezone;
    }
    DEBUG1 ("rc: %lu, timezone: %ld, dst_adjust: %ld, UTC_adjust: %ld min, sizeof(*hdr): %zd\n",
            rc, timezone, dst_adjust, UTC_adjust, sizeof(*hdr));
    done = true;
  }

  /* With GPS detected, the upper 18 bits are seconds since last midnight 00:00:00 UTC
   * With no GPS, the timestamp is a 48-bit unsigned number counting at 12 MHz.
   */
  uint64_t timestamp;

  if (!g_data.GPS.detected)
  {
    timestamp = (mg_ntohl (hdr->ts1) << 16) + mg_ntohs (hdr->ts2);
    snprintf (ts_buf, sizeof(ts_buf), "relative %.0f", timestamp / 12*1E6);
  }
  else
  {
    timestamp = mg_ntohl (hdr->ts1) >> (32 - 18); /* get upper 18 bits */

    if (timestamp > 24*3600)
    {
      junk = ", junk-data!";
      g_data.stat.pkt_junk++;
    }

    /* Lower 30 bits are nanoseconds of current second
     */
    uint32_t nanosec = (mg_ntohl (hdr->ts1) >> 30) + mg_ntohl (hdr->ts2);
    uint16_t microsec = nanosec / 1000;

    /* Calculate local time.
     */
    uint32_t local_sec = (timestamp + 60 * UTC_adjust) % (24 * 3600);
    uint16_t hours     = (uint16_t) local_sec / 3600;
    uint16_t minutes   = (timestamp / 60) % 60;
    uint16_t seconds   = (uint16_t) local_sec % 60;

    *ts_msec = 1000 * local_sec + seconds + microsec / 1000;
    snprintf (ts_buf, sizeof(ts_buf), "absolute %llu, nanosec: %u: %02u:%02u:%02u:%06u",
              timestamp, nanosec, hours, minutes, seconds, microsec);
  }

  char dbFS [10] = "-";

  if (hdr->RSSI > 0)
     snprintf (dbFS, sizeof(dbFS), "%.1f", 10.0 * log10((double)hdr->RSSI / 255.0));  /* is this right? */

  if (junk)
       DEBUG1 ("%s(): timestamp: %s, RSSI: %d / %s dBFS%s\n\n", func, ts_buf, hdr->RSSI, dbFS, junk);
  else DEBUG2 ("%s(): timestamp: %s, RSSI: %d / %s dBFS\n\n", func, ts_buf, hdr->RSSI, dbFS);

  return (junk == NULL);
}

static void decode_common (const RX_packet *pkt, const char *func)
{
  const header_32_33 *hdr = (const header_32_33*) &pkt->msg [2];
  const uint8_t      *msg = pkt->msg;
  modeS_message       mm;
  uint64_t            ts;
  int                 rc;

  if (pkt->msg_len < sizeof(*hdr))
     g_data.pkt_junk = ", junk-data";

  DEBUG2 ("%s(): msg[0]: 0x%02X, msg[1]: 0x%02X, msg_len: %u%s\n",
          func, msg[0], msg[1], pkt->msg_len,
          g_data.pkt_junk ? g_data.pkt_junk : "");

  HEX_DUMP2 (msg, min(pkt->msg_len, sizeof(pkt->msg)), NULL);

  decoder_header (hdr, &ts, func);

  rc = decode_mode_S_message (&mm, (const uint8_t*)(hdr + 1));
  if (rc == 0)
  {
#if 1
    mm.timestamp_msg     = ts;
    mm.sys_timestamp_msg = g_data.GPS.detected ? ts : MSEC_TIME();
#endif
    modeS_user_message (&mm);
  }
  else
  {
    g_data.stat.mode_S_errors++;
    DEBUG1 ("%s(): decode_mode_S_message(): %d, \n", func, rc);
  }

}

/**
 * Decode msg-type 0x32; "Mode-S Short Squitter" raw data at `pkt->msg + 2`
 *
 * Comparable to `readBeast()` in readsb.
 */
static void decode_msg_32 (const RX_packet *pkt, uint32_t msg_len)
{
  const uint8_t *msg = pkt->msg;

  assert (msg[0] == 0x1A);
  assert (pkt->msg_type == MODES_SHORT_SQ);

  decode_common (pkt, __FUNCTION__);
}

/**
 * Decode msg-type 0x33; "Mode-S Extended Squitter" raw data at `pkt->msg + 2`
 */
static void decode_msg_33 (const RX_packet *pkt, uint32_t msg_len)
{
  const uint8_t      *msg = pkt->msg;

  assert (msg[0] == 0x1A);
  assert (pkt->msg_type == MODES_EXT_SQ);

  decode_common (pkt, __FUNCTION__);
}

/**
 * Decode msg-type 0x34; No idea what this is. Show it raw.
 */
static void decode_msg_34 (const RX_packet *pkt, uint32_t msg_len)
{
  const uint8_t *msg = pkt->msg;

  assert (msg[0] == 0x1A);
  assert (pkt->msg_type == HULC_MSG_34);

  DEBUG2 ("%s(): msg[0]: 0x%02X, msg[1]: 0x%02X, msg_len: %u%s\n",
          __FUNCTION__, msg[0], msg[1], msg_len,
          g_data.pkt_junk ? g_data.pkt_junk : "");
  HEX_DUMP2 (msg, min(msg_len, sizeof(pkt->msg)), NULL);
}

/*
 * Decode msg-type 0x48 and ID=24:
 * ```
 * 0x1A 0x48 0x24 0x10 CMD P00, P01, ... P14
 *                     ^
 *                     |__ *cmd is here
 * ```
 *
 * 0x10 denotes length of data; CMD P00 P01 ... P14.
 * And should always be x10.
 *
 * This is a the response to the Firmware request command; `"#00\r\n"`
 * sent in `gns_hulc_init()` and `send_option()`.
 */
static void show_firmware (const uint8_t *cmd, uint32_t cmd_len)
{
  const uint8_t *param = cmd + 1;
  uint32_t       i;
  char           fw_buf [200];
  char          *p = fw_buf;
  char          *end = p + sizeof(fw_buf);

  if (cmd_len != 16)
     g_data.pkt_junk = ", Firmware-junk";

  HEX_DUMP1 (cmd, cmd_len, ", Firmware-resp:");

  p += snprintf (p, end - p, "CMD: %02X: ", *cmd);
  for (i = 0; i < cmd_len - 1; i++)
      p += snprintf (p, end - p, "P[%d]: %02X ", i, param[i]);
  DEBUG1 ("%s\n", fw_buf);
}

static void send_firmware_req (void *unused)
{
  DEBUG1 ("%s()\n", __FUNCTION__);
  send_option ((const uint8_t*)"#00\r\n", 5, ", Firmware request:");
  (void) unused;
}

/**
 * Decode msg x40 where ID = 0x01 or 0x24: <br>
 *  \li ID = 0x01: Periodic HULC Status Message
 *  \li ID = 0x24: Reply to command; i.e the "GNS-HULC Firmware" response
 *
 * The message layout:
 *
 * ```
 *    ID at msg[2]   len at msg[3]
 *              |         |
 *              |         |
 * 0x1A, 0x48, 0x01/0x24, <len>, <data>
 *                               |__ hsm or firmware
 * ```
 */
static void decode_msg_48 (const RX_packet *pkt, uint32_t msg_len)
{
  char              flags_str [200] = "";
  uint16_t          flags;
  time_t            xTime;
  uint8_t           ID, len;
  bool              gps_valid;
  const char       *junk1 = "";
  const char       *junk2 = "";
  const char       *junk3 = "";
  const uint8_t    *msg   = pkt->msg;
  const uint8_t    *firmware = msg + 4;
  const status_msg *hsm = (const status_msg*) &pkt->msg [4];

  assert (msg[0] == 0x1A);
  assert (msg[1] == HULC_STATUS);

  ID = msg [2];

  /**
   * Command lengths:
   * ID = 0x01, should be 24
   * ID = 0x24, should be 16.
   */
  len = msg [3];

  if (ID != 0x01 && ID != 0x24)
  {
    junk1 = ", junk-data-1!";
    g_data.stat.pkt_junk++;
  }

  if (msg_len < sizeof(*hsm))
  {
    junk2 = ", junk-data-2!";
    g_data.stat.pkt_junk++;
  }

  if (ID == 0x24)
  {
    show_firmware (firmware, len);
    return;
  }

  if (len < sizeof(*hsm))
  {
    junk3 = ", junk-data-3!";
    g_data.stat.pkt_junk++;
    return;
  }

  flags = mg_ntohs (hsm->flags);
  xTime = mg_ntohl (hsm->xTime);

  DEBUG2 ("%s(): ID: %u, len: %u, msg[0]: 0x%02X, msg[1]: 0x%02X, msg[2]: 0x%02X, msg[3]: 0x%02X, msg_len: %u%s%s%s\n",
          __FUNCTION__, ID, len, msg[0], msg[1], msg[2], msg[3], msg_len, junk1, junk2, junk3);

  HEX_DUMP2 (msg, msg_len, NULL);

  if ((flags & 0x8000) == 0x8000)
  {
    strcat_s (flags_str, sizeof(flags_str), "GPS detected, ");
    g_data.GPS.detected = true;
  }
  else
    g_data.GPS.detected = false;

  if ((flags & 0x4000) == 0x4000)
  {
    strcat_s (flags_str, sizeof(flags_str),  "GPS valid, ");
    gps_valid = true;
  }
  else
    gps_valid = false;

  if ((flags & 0x2000) == 0x2000)
     strcat_s (flags_str, sizeof(flags_str),  "GPS fix, ");

  if ((flags & 0x1000) == 0x1000)
     strcat_s (flags_str, sizeof(flags_str),  "PPS time, ");

  /* 4 unused bits */

  if ((flags & 0x0080) == 0x0080)
     strcat_s (flags_str, sizeof(flags_str),  "Tx-queue overflow since start-up, ");

  if ((flags & 0x0040) == 0x0040)
     strcat_s (flags_str, sizeof(flags_str),  "Tx-queue overflow during last second, ");

  if ((flags & 0x0020) == 0x0020)
     strcat_s (flags_str, sizeof(flags_str),  "Excessive NMEA, ");

  pos_t pos = { 360.0, 360.0 }; /* not a VALID_POS() */

  if (gps_valid)
  {
    pos.lat = binary_angle (hsm->latitude);
    pos.lon = binary_angle (hsm->longitude);

    if (VALID_POS(pos))
       g_data.GPS.pos = pos;

    g_data.GPS.satellites = hsm->satellites;   /* no swap; only 8-bit */
    g_data.GPS.altitude   = mg_ntohs (hsm->altitude);
    g_data.GPS.HDOP       = (double)hsm->hdop / 10;
  }

  char *comma = strrchr (flags_str, ',');

  if (comma)
     *comma = '\0';   /* remove last ", " */

  DEBUG2 ("msg_len:        %u, sizeof(*hsm): %zd, len: %u\n", msg_len, sizeof(*hsm), len);
  DEBUG2 ("hsm.serial:     %u\n", mg_ntohl (hsm->serial));
  DEBUG2 ("hsm.flags:      0x%04X: %s\n", flags, flags_str);
  DEBUG2 ("hsm.xTime:      %llu, %.24s\n", xTime, ctime(&xTime));

  if (!gps_valid)
  {
    DEBUG2 ("gps_valid:      false\n");
    DEBUG2 ("hsm.latitude:   %+10.08f\n", binary_angle (hsm->latitude));
    DEBUG2 ("hsm.longitude:  %+10.08f\n", binary_angle (hsm->longitude));
    DEBUG2 ("hsm.altitude:   %d\n", mg_ntohs (hsm->altitude));
    DEBUG2 ("hsm.satellites: %d\n", hsm->satellites);
    DEBUG2 ("hsm.hdop:       %.2f\n", (double)hsm->hdop / 10);
  }
  else
  {
    DEBUG2 ("gps_valid:      true\n");
    DEBUG2 ("hsm.latitude:   %+10.08f\n", g_data.GPS.pos.lat);
    DEBUG2 ("hsm.longitude:  %+10.08f\n", g_data.GPS.pos.lon);
    DEBUG2 ("hsm.altitude:   %d\n", g_data.GPS.altitude);
    DEBUG2 ("hsm.satellites: %d\n", g_data.GPS.satellites);
    DEBUG2 ("hsm.hdop:       %.2f\n", g_data.GPS.HDOP);      /* HDOP; Horizontal Dilution of Precision */

    if (*junk1 == '\0' && *junk2 == '\0' &&
        VALID_POS(pos) && g_data.GPS.HDOP < GPS_HDOP_WORST)  /* lower number is better */
    {
      if (hsm->satellites <= GPS_HAVE_FIX_LOW && g_data.GPS.have_fix)
      {
        g_data.GPS.have_fix = false;
        DEBUG2 ("Lost GPS-fix\n");
      }
      else if (hsm->satellites >= GPS_HAVE_FIX_HIGH && !g_data.GPS.have_fix)
      {
        g_data.GPS.have_fix = true;
        DEBUG2 ("Regained GPS-fix\n");
      }
    }
  }
  DEBUG2 ("\n");
}

/**
 * Check the packet for a valid `MARKER_MAGIC`, too short or too big `pkt->msg_len`.
 */
static bool pkt_check (const RX_packet *pkt)
{
  if (pkt->msg_marker != MARKER_MAGIC)
  {
    g_data.stat.pkt_bad_marker++;
    DEBUG2 ("Marker destroyed (3), pkt->msg_len: %u\n", pkt->msg_len);
    return (false);
  }

  if (pkt->msg_len <= sizeof(header_32_33) /* + MODES_SHORT_SQ_SZ */)
  {
    g_data.stat.pkt_too_short++;
    DEBUG2 ("msg_len: %u, msg_type: 0x%02X, too short (2). Min-size: %zd\n",
            pkt->msg_len, pkt->msg_type, sizeof(header_32_33) + MODES_SHORT_SQ_SZ);
    HEX_DUMP1 (pkt->msg, pkt->msg_len, ", too short");
    return (false);
  }

  if (pkt->msg_len >= sizeof(pkt->msg))
  {
    g_data.stat.pkt_too_big++;
    DEBUG2 ("msg_len: %u, msg_type: 0x%02X, too big. Max-size: %zd\n", pkt->msg_len, pkt->msg_type, sizeof(pkt->msg) - 1);
    HEX_DUMP1 (pkt->msg, min(pkt->msg_len, 200), ", too big");
    return (false);
  }

  return (true);
}

/**
 * Add a parsed packet `== g_data.pkt_current` to the end of the
 * packet queue.
 */
static bool pkt_enqueue (const RX_packet *pkt)
{
  RX_packet *copy;
  bool  rc;

  g_data.stat.pkt_enqueued_bytes += pkt->msg_len;

  copy = malloc (sizeof(*pkt));
  if (!copy)
  {
    g_data.stat.pkt_OOM++;
    DEBUG1 ("calloc() failed\n");
    rc =  false;
    goto failed;
  }

  assert (g_data.got_x1A);
  assert (pkt->msg_len > 0);
  assert (pkt->msg [0] == 0x1A);  /* SOP */
  assert (pkt->msg [1] != 0x1A);  /* msg_type */

  copy->msg [0]    = pkt->msg [0];
  copy->msg [1]    = pkt->msg [1];
  copy->msg_type   = pkt->msg [1];
  copy->usec       = get_usec_now();
  copy->next       = NULL;
  copy->unstuffed  = 0;
  copy->msg_marker = MARKER_MAGIC;

  const uint8_t *src = pkt->msg + 2;
  uint8_t       *dst = copy->msg + 2;
  uint32_t       i;

  for (i = 0; i < pkt->msg_len - 2; i++)
  {
    if (*src == 0x1A)
    {
      copy->unstuffed++;  /* unstuff; skip the extra x1A */
      *src++;
    }
    *dst++ = *src++;
  }

  copy->msg_len = 2 + i;

  rc = pkt_check (copy);
  if (!rc)
  {
    free (copy);
    goto failed;
  }

  EnterCriticalSection (&g_data.crit);

  if (copy->unstuffed > 0)
     g_data.stat.pkt_unstuffed++;  /* Update unstuffed packets counter */

  switch (copy->msg_type)
  {
    case MODES_SHORT_SQ:       /* 0x32 */
         g_data.stat.rx_packets_32++;
         break;
    case MODES_EXT_SQ:         /* 0x33 */
         g_data.stat.rx_packets_33++;
         break;
    case HULC_MSG_34:          /* 0x34 */
         g_data.stat.rx_packets_34++;
         break;
    case HULC_STATUS:          /* 0x48 */
         g_data.stat.rx_packets_48++;
         break;
    default:
         g_data.stat.rx_packets_unknown++;
         DEBUG1 ("Rx-unknown, copy->msg_len: %u, copy->msg_type: 0x%02X\n",
                 copy->msg_len, copy->msg_type);
         HEX_DUMP1 (copy->msg, min(copy->msg_len, sizeof(copy->msg)), NULL);
         rc = false;
         goto failed;
  }

  LIST_ADD_TAIL (RX_packet, &g_data.pkt_list, copy);

  g_data.stat.pkt_enqueued++;

  uint32_t len = pkt_list_len();

  if (len > g_data.stat.pkt_max_len)
  {
    g_data.stat.pkt_max_len = len;
    DEBUG1 ("pkt_max_len: %u\n", g_data.stat.pkt_max_len);
  }

  LeaveCriticalSection (&g_data.crit);

failed:

  /* Restart `g_data.pkt_current`
   */
  memset (&g_data.pkt_current, '\0', sizeof(g_data.pkt_current));
  DEBUG2 ("\n");
  return (rc);
}

/**
 * Return length of `g_data.pkt_list` now.
 */
static uint32_t pkt_list_len (void)
{
  const RX_packet *pkt;
  uint32_t num = 0;

  for (pkt = g_data.pkt_list; pkt; pkt = pkt->next)
      num++;
  return (num);
}

/**
 * Free all elements in `g_data.pkt_list`.
 */
static void pkt_free (void)
{
  RX_packet *pkt, *pkt_next;

  EnterCriticalSection (&g_data.crit);

  for (pkt = g_data.pkt_list; pkt; pkt = pkt_next)
  {
    LIST_DELETE (RX_packet, &g_data.pkt_list, pkt);
    pkt_next = pkt->next;
    free (pkt);
  }
  LeaveCriticalSection (&g_data.crit);
}

/**
 * \def DEBUG_CH()
 * \li Trace current `ch` and `g_data.old_ch`.
 * \li Show the state transition; old-state -> current state.
 * \li Show the `g_data.sio_buf[]` indices.
 */
#define DEBUG_CH()                                                      \
        DEBUG2 ("ch: %s old_ch: %s %s -> %s, idx: %4d, old_idx: %4d\n", \
                ch_str(ch), ch_str(g_data.old_ch),                      \
                state_name(old_state), state_name(g_data.state),        \
                idx, g_data.old_idx)

/**
 * \def DEBUG_IDX()
 * Trace the legal range of `g_data.sio_buf[]` indices.
 * and whether we are processing "fresh-data" or "old-data".
 */
#define DEBUG_IDX(what, first, last)                                         \
        DEBUG2 ("Processing %s: sio_len: %d, idx: [%d - %d>, old_idx: %d\n", \
                what, g_data.sio_len, first, last, g_data.old_idx)

/**
 * Possibly fill up `g_data.sio_buf[]` with fresh data; `g_data.old_idx = 0`.
 * And return the max-index for new or old data.
 */
static bool get_idx_max (int *idx_max)
{
  *idx_max = -1;

  if (!g_data.old_data)    /* fill up with fresh data */
  {
    g_data.old_idx = 0;
    g_data.sio_len = COM_read (Modes.gns_hulc.handle, g_data.sio_buf, COM_RX_SIZE);
    if (g_data.sio_len == 0)
    {
      DEBUG2 ("sio_len: 0\n");

      /**
       * \todo
       * This dead_count should depend on `Sleep()` time
       * in `data_thread_fn()` and port baud-rate.
       */
      if (++g_data.COM.dead_count >= COM_DEAD_COUNT)
      {
        LOG_STDERR ("Port `%s' seems dead. No data %u times in a row.\7\n", g_data.COM.dev_name, COM_DEAD_COUNT);
        Modes.exit = Modes.no_stats = true;
      }
      return (false);
    }

    if (g_data.sio_len < 0)
    {
      LOG_STDERR ("Modes.exit: COM-port problem; %s\n", win_strerror(GetLastError()));
      Modes.exit = true;
      return (false);
    }

    if (g_data.sio_len > 0)
       g_data.COM.dead_count = 0;

    *idx_max = min (g_data.sio_len, sizeof(g_data.pkt_current.msg));
    DEBUG_IDX ("fresh-data", g_data.old_idx, *idx_max);
//  g_data.old_ch = -1;
  }
  else
  {
    *idx_max = min (g_data.sio_len, sizeof(g_data.pkt_current.msg)) + g_data.old_idx;
    DEBUG_IDX ("old-data", g_data.old_idx, *idx_max);
//  g_data.old_ch = -1;
  }
  return (true);
}

/**
 * The detail of it all is here.
 */
static int hulc_read (void)
{
  int ch = -1;
  int idx, start_idx, idx_max;
  int old_enqueued = g_data.stat.pkt_enqueued;

  state_func old_state;

  if (!get_idx_max(&idx_max))
     return (0);

  /*
   * Comparable to `readBeastCommand()` in readsb.
   */
  start_idx = g_data.old_idx;

  for (idx = start_idx; idx < idx_max; idx++)
  {
    ch = g_data.sio_buf [idx];
    old_state = g_data.state;

    (*g_data.state) (ch);

    DEBUG_CH();

    g_data.old_ch = ch;
  }

  /* Process the rest of `g_data.sio_buf` on next call;
   * from current `idx`.
   */
  if (idx < g_data.sio_len)
  {
    g_data.sio_len -= idx;
    g_data.old_idx += idx;
    g_data.old_data = true;
    g_data.stat.old_data_cnt++;

    DEBUG2 ("Processed: [%d - %d>, sio_len remaining: %d\n", start_idx, idx, g_data.sio_len);
    assert (g_data.sio_len > 0);
  }
  else
  {
    DEBUG2 ("Processed all: [%d - %d>, sio_len: %d\n", start_idx, idx, g_data.sio_len);
    g_data.old_idx  = 0;
    g_data.sio_len  = 0;
    g_data.old_data = false;

#if 0
//  g_data.old_ch = -1;
    old_state    = g_data.state;
    g_data.state = state_get_sync;
    DEBUG_CH();
#endif
  }
  return (g_data.stat.pkt_enqueued - old_enqueued);
}

/**
 * Called from `data_thread_fn()`.
 *
 * In an infinite loop, read (buffered) serial data and fill `g_data.pktg_list`
 * with RX_packet (g_data.pkt_current). These packets are then polled by the below
 * `gns_hulc_poll()`.
 */
#define USE_MG_TIMER_ADD 0
#define FW_TIMER         60000

void gns_hulc_read_loop (void)
{
#if USE_MG_TIMER_ADD
  mg_timer_add (&Modes.mgr, FW_TIMER, MG_TIMER_REPEAT, send_firmware_req, NULL);
#else
  static uint64_t last_fw_request = 0ULL;
  uint64_t now;
#endif

  while (!Modes.exit)
  {
    hulc_read();
    Sleep (Modes.gns_hulc.poll_ms);   /* default 50 msec */

#if (USE_MG_TIMER_ADD == 0)
    now = MSEC_TIME();
    if (last_fw_request == 0ULL)
       last_fw_request = now;
    if ((now - last_fw_request) >= FW_TIMER)
       send_firmware_req (NULL);
    last_fw_request = now;
#endif

    /* Stop debug output if we reached max-messages
     */
    if (Modes.max_messages > 0 && Modes.stat.messages_shown >= Modes.max_messages)
       Modes.debug &= ~(DEBUG_GNS_HULC | DEBUG_GNS_HULC2);
  }
}

/**
 * Called from `background_tasks()` (in main-thread) to poll and process
 * the `g_data.pkt_list`.
 *
 * Since `LIST_ADD_TAIL()` was used in `pkt_enqueue()`, this will process
 * the oldest packets first.
 *
 * Try to acquire the critical-section. If this fails or the packet-list
 * is empty, Sleep() for `Modes.gns_hulc.poll_ms` (50 msec by default).
 *
 * Then traverse the packet-list unless it's empty.
 * After processing each, free the RX-packet.
 */
void gns_hulc_poll (void)
{
  RX_packet    *pkt, *pkt_next;
  uint8_t      *msg;
  double        now;
  static double prev_usec = 0.0;

  /* If something bad happend or we were told to stop, return zero.
   */
  if (Modes.exit)
     return;

  if (!TryEnterCriticalSection(&g_data.crit) || /* Try to lock */
      !g_data.pkt_list)     /* if list empty, wait `Modes.gns_hulc.poll_ms` for a packet */
  {
    Sleep (Modes.gns_hulc.poll_ms);
    g_data.stat.pkt_list_sleep++;
  }

  now = get_usec_now();

  for (pkt = g_data.pkt_list; pkt; pkt = pkt_next)
  {
    double diff1_ms, diff2_ms;

    pkt_next = pkt->next;

    diff1_ms = (now -  pkt->usec) / 1E3;
    diff2_ms = (prev_usec > 0.0) ? (pkt->usec - prev_usec) / 1E3 : 0.0;
    prev_usec = pkt->usec;

    DEBUG2 ("pkt->msg_type: 0x%02X, pkt->msg_len: %u, diff1: %.1f, diff2: %.1f, old_data_cnt: %llu\n",
            pkt->msg_type, pkt->msg_len, diff1_ms, diff2_ms, g_data.stat.old_data_cnt);

    HEX_DUMP2 (pkt->msg, pkt->msg_len, NULL);

    g_data.pkt_junk = NULL;

    switch (pkt->msg_type)
    {
      case MODES_SHORT_SQ:
           decode_msg_32 (pkt, pkt->msg_len);
           break;
      case MODES_EXT_SQ:
           decode_msg_33 (pkt, pkt->msg_len);
           break;
      case HULC_MSG_34:
           decode_msg_34 (pkt, pkt->msg_len);
           break;
      case HULC_STATUS:
           decode_msg_48 (pkt, pkt->msg_len);
           break;
      default:          /* cannot happen */
           msg = pkt->msg;
           DEBUG1 ("Rx-unknown: msg[0]: 0x%02X, msg[1]: 0x%02X, msg[2]: 0x%02X, msg_len: %u\n",
                   msg[0], msg[1], msg[2], pkt->msg_len);
           HEX_DUMP1 (msg, pkt->msg_len, NULL);
           break;
    }

    g_data.stat.pkt_dequeued++;

    if (g_data.pkt_junk)
       g_data.stat.pkt_junk++;

    LIST_DELETE (RX_packet, &g_data.pkt_list, pkt);
    free (pkt);
  }

  LeaveCriticalSection (&g_data.crit);
}

/**
 * Send an option message to GNS-Hulc.
 *
 * If `g_data.Beast.enable` send a Beast option.
 * Otherwise send a Hulc "Firmware request".
 */
static bool send_option (const uint8_t *msg, int msg_sz, const char *what)
{
  int rc = COM_write (Modes.gns_hulc.handle, msg, msg_sz);

  if (rc == msg_sz)
  {
    g_data.stat.tx_packets++;
    HEX_DUMP1 (msg, msg_sz, what);
  }
  return (rc == msg_sz);
}

/**
 * Send a specific Beast option message to GNS-Hulc.
 */
static bool send_beast_option (uint8_t opt)
{
  uint8_t msg[3] = { 0x1A, '1', opt };
  char    what [30];

  snprintf (what, sizeof(what), ", Beast option %u/%c:", opt, opt);
  return send_option (msg, sizeof(msg), what);
}

/**
 * Send all Beast options
 */
static void send_beast_options (void)
{
  if (g_data.Beast.enable)
  {
    send_beast_option ('B');         /* set classic beast mode */
    send_beast_option ('C');         /* use binary format */
    send_beast_option ('H');         /* RTS enabled */

    if (g_data.Beast.filter_DF1117)
         send_beast_option ('D');    /* enable DF11/17-only filter*/
    else send_beast_option ('d');    /* disable DF11/17-only filter, deliver all messages */

    if (g_data.Beast.mlat_timestamp)
         send_beast_option ('E');    /* enable mlat timestamps */
    else send_beast_option ('e');    /* disable mlat timestamps */

    if (g_data.Beast.CRC)
         send_beast_option ('f');    /* enable CRC checks */
    else send_beast_option ('F');    /* disable CRC checks */

    if (g_data.Beast.filter_DF045)
         send_beast_option ('G');    /* enable DF0/4/5 filter */
    else send_beast_option ('g');    /* disable DF0/4/5 filter, deliver all messages */

    if (g_data.Beast.FEC)
         send_beast_option ('i');    /* FEC enabled */
    else send_beast_option ('I');    /* FEC disbled */

    if (g_data.Beast.mode_AC)
         send_beast_option ('J');    /* Mode A/C enabled */
    else send_beast_option ('j');    /* Mode A/C disabled */
  }
}

/*
 * 360 deg / 2^32
 */
#define STEP_SIZE 8.38190317153931E-08

/**
 * Convert 32-bit signed binary angular measure to double degree.
 * See https://www.globalspec.com/reference/14722/160210/Chapter-7-5-3-Binary-Angular-Measure
 *
 * \param angle  Data buffer start (MSB first)
 * \retval       Angular degree
 */
static double binary_angle (int32_t angle)
{
  double ret;

  angle = mg_ntohl (angle);
  ret = (double) (angle * STEP_SIZE);

  if (ret > 180.0)
     ret -= 360.0;
  assert (ret >= -180 && ret < 180);
  return (ret);
}

/**
 * Look in the Registry COM-port mapping at `COM_KEY_NAME` for
 * a NT-namespace name for `COMx`.
 *
 * \eg if `look_for == "COM4"` and `RegEnumValue()` returns
 *     `value == "\Device\VCP0"` and `data == "COM4"`,
 *     then return `"\Device\VCP0"` for the name-space name.
 *     (the first Virtual COM-port).
 */
static char *get_name_space (uint16_t port)
{
  static char ret [100];
  char   look_for [100];
  HKEY   key;
  DWORD  rc, num = 0;

  strcpy (ret, "?");
  snprintf (look_for, sizeof(look_for), "COM%u", port);

  DEBUG2 ("look_for: '%s'\n", look_for);

  rc = RegOpenKeyExA (HKEY_LOCAL_MACHINE, COM_KEY_NAME, 0, KEY_READ, &key);
  if (rc != ERROR_SUCCESS)
  {
    DEBUG2 ("RegOpenKeyExA (\"HKLM\\%s\") failed; %s\n", COM_KEY_NAME, win_strerror(rc));
    return (ret);
  }

  while (1)
  {
    char  value [100] = { '\0' };
    char  data [100]  = { '\0' };
    DWORD value_size  = sizeof(value);
    DWORD data_size   = sizeof(data);
    DWORD type        = REG_NONE;
    const char *err = "No more items";

    rc = RegEnumValue (key, num++, value, &value_size, NULL, &type, (BYTE*)&data, &data_size);
    if (rc != ERROR_NO_MORE_ITEMS)
       err = win_strerror (GetLastError());

    DEBUG2 ("RegEnumValue(): %s\n", err);
    if (rc == ERROR_NO_MORE_ITEMS)
       goto quit;

    if (type != REG_SZ)
       continue;

    if (!strnicmp(data, look_for, strlen(look_for)))
    {
      strncpy (ret, value, sizeof(ret)-1);
      goto quit;
    }
  }
quit:
  RegCloseKey (key);
  DEBUG2 ("ret: '%s'\n", ret);
  return (ret);
}

static const char *ch_str (int ch)
{
  static char buf [2][5];
  static int  idx = 0;
  char       *ret = buf [idx];

  if (ch == -1)
     return ("-1 ,");

  ret [0] = hex_digits [(uint8_t)ch >> 4];
  ret [1] = hex_digits [(uint8_t)ch & 15];
  ret [2] = (ch == 0x1A) ? '!' : ' ';
  ret [3] = ',';
  ret [4] = '\0';

  idx ^= 1;
  return (ret);
}

/**
 * Use this local hex-dump function; not `mg_hexdump()`.
 */
static void hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what)
{
  size_t i, idx, count = 0;
  char   lbuf [200];
  int    lbuf_idx;

#if 1
  EnterCriticalSection (&Modes.print_mutex);
#else
  EnterCriticalSection (&g_data.crit);
#endif

  LOG_STDOUT (GNS_FILE "(%u): len: %zd%s\n", line, len, what ? what : "");

  for (idx = 0; len > 0; len -= count)
  {
    count = (len > 16) ? 16 : len;
    snprintf (lbuf, sizeof(lbuf), "%4.4X  ", (int)idx);

    lbuf_idx = 6;

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
       lbuf [lbuf_idx++] = iscntrl (buf[i]) ? '.' : buf [i];

    for ( ; i < 16; i++)
        lbuf [lbuf_idx++] = ' ';

    lbuf [lbuf_idx++] = '|';
    lbuf [lbuf_idx++] = '\0';

    LOG_STDOUT ("!%s\n", lbuf);
    buf += count;
    idx += count;
  }

  LOG_STDOUT ("! \n");

#if 1
  LeaveCriticalSection (&Modes.print_mutex);
#else
  LeaveCriticalSection (&g_data.crit);
#endif

}
