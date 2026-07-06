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

#define USE_DUMP_FILES     0
#define FIRMWARE_TIMER     10000

#define GPS_HAVE_FIX_LOW   6
#define GPS_HAVE_FIX_HIGH  15
#define GPS_HDOP_WORST     20

GNS_priv g_data;

static void        set_defaults (void);
static bool        send_option (const uint8_t *msg, int msg_sz, const char *what);
static void        send_beast_options (void);
static void        send_firmware_req (void *unused);
static bool        pkt_enqueue (const RX_packet *pkt);
static uint32_t    pkt_list_len (void);
static void        pkt_free (void);
static double      binary_angle (int32_t angle);
static char       *get_name_space (uint16_t port);
static const char *ch_str (int ch);
static void        hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what);
static void        hex_dump_csv (FILE *f, const uint8_t *buf, size_t len, const char *what, bool rc);

static char hex_digits [] = "0123456789ABCDEF";

static void state_get_sync (uint8_t ch, int idx);
static void state_put_ch (uint8_t ch, int idx);
static void state_got_1A (uint8_t ch, int idx);

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

  if (!COM_init(hnd))
  {
    LOG_STDERR ("COM_init (\"%s\") failed.\n", g_data.COM.dev_name);
    gns_hulc_exit (hnd);
    return (NULL);
  }

  LOG_STDERR ("Running GNS %s on `%s' (%s)\n",
              g_data.Beast.enable ? "Beast" : "HULC",
              g_data.COM.dev_name, g_data.COM.name_space);

  Modes.gns_hulc.handle = hnd;

  if (g_data.Beast.enable)
     send_beast_options();

  g_data.pkt_current.msg_len = 0;
  g_data.got_x1A = false;

  DEBUG2 ("ch: --, %s() -> state_get_sync()\n", __FUNCTION__);
  g_data.state = state_get_sync;

  mg_timer_add (&Modes.mgr, FIRMWARE_TIMER, MG_TIMER_REPEAT, send_firmware_req, NULL);

#if USE_DUMP_FILES
  char *hex_file = mg_mprintf ("%s\\gns-hex.txt", Modes.tmp_dir);
  char *gps_file = mg_mprintf ("%s\\gns-gps.txt", Modes.tmp_dir);

  g_data.hex_file = fopen (hex_file, "w+");
  g_data.gps_file = fopen (gps_file, "w+");

  free (hex_file);
  free (gps_file);
#endif

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
    COM_exit (hnd);
    Sleep (GNS_HULC_SLEEP);
    CloseHandle (hnd);
  }

  free (Modes.gns_hulc.name);
  free (g_data.sio_buf);
  pkt_free();

  DeleteCriticalSection (&g_data.crit);
  Modes.gns_hulc.name = NULL;
  g_data.sio_buf = NULL;

  if (g_data.hex_file)
     fclose (g_data.hex_file);

  if (g_data.gps_file)
     fclose (g_data.gps_file);

  g_data.hex_file = g_data.gps_file = NULL;
}

/**
 * Show statistics. Called from main() before exit.
 */
void gns_hulc_stats (void)
{
  uint64_t sum = g_data.stat.rx_packets_32 + g_data.stat.rx_packets_33 +
                 g_data.stat.rx_packets_34 + g_data.stat.rx_packets_48 +
                 g_data.stat.rx_errors;

  if (sum == 0ULL)
     return;

  LOG_STDOUT ("! \n");
  LOG_STDOUT ("GNS-HULC statistics:\n");

  LOG_STDOUT (" %8llu RX-packets-32 (unstuffed: %llu).\n",
              g_data.stat.rx_packets_32, g_data.stat.rx_unstuffed_32);

  LOG_STDOUT (" %8llu RX-packets-33 (unstuffed: %llu).\n",
              g_data.stat.rx_packets_33, g_data.stat.rx_unstuffed_33);

  LOG_STDOUT (" %8llu RX-packets-34 (unstuffed: %llu).\n",
              g_data.stat.rx_packets_34, g_data.stat.rx_unstuffed_34);

  LOG_STDOUT (" %8llu RX-packets-48 (unstuffed: %llu).\n",
              g_data.stat.rx_packets_48, g_data.stat.rx_unstuffed_48);

  LOG_STDOUT (" %8llu RX-unknown.\n",   g_data.stat.rx_packets_unknown);
  LOG_STDOUT (" %8llu TX-packets.\n",   g_data.stat.tx_packets);
  LOG_STDOUT (" %8llu RX-junk-data.\n", g_data.stat.pkt_junk);

  double percent = (100.0 * g_data.stat.pkt_too_short) /
                   (g_data.stat.rx_packets_33 + g_data.stat.rx_packets_34 +
                    g_data.stat.rx_packets_48 + g_data.stat.rx_packets_unknown);

  LOG_STDOUT (" %8llu Too short packets (%.2f%%).\n", g_data.stat.pkt_too_short, percent);
  LOG_STDOUT (" %8llu Dequeued packets\n", g_data.stat.pkt_dequeued);
  LOG_STDOUT (" %8llu mode_S errors.\n", g_data.stat.mode_S_errors);
}

uint64_t gns_hulc_junk (void)
{
  return (g_data.stat.pkt_junk);
}

uint64_t gns_hulc_too_short (void)
{
  return (g_data.stat.pkt_too_short);
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
    *ts_msec = timestamp;
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

  DEBUG2 ("%s(): timestamp: %s, RSSI: %d / %s dBFS%s\n\n",
          func, ts_buf, hdr->RSSI, dbFS, junk ? junk : "");

  return (junk == NULL);
}

static void decode_common (const RX_packet *pkt, int valid_bits, const char *func)
{
  const header_32_33 *hdr;
  const uint8_t      *msg;
  modeS_message       mm;
  uint64_t            ts;
  int                 DF, rc;

  if (pkt->msg_len < sizeof(*hdr))
     g_data.pkt_junk = ", junk-data";

  msg = pkt->msg;

  DEBUG2 ("%s(): msg[0]: 0x%02X, msg[1]: 0x%02X, msg_len: %u%s\n",
          func, msg[0], msg[1], pkt->msg_len,
          g_data.pkt_junk ? g_data.pkt_junk : "");

  HEX_DUMP2 (msg, min(pkt->msg_len, sizeof(pkt->msg)), NULL);

  hdr = (const header_32_33*) &pkt->msg [2];
  decoder_header (hdr, &ts, func);

  msg = (const uint8_t*) (hdr + 1);
  DF  = msg [0] >> 3;    /* Downlink Format */

  rc = modeS_message_score (msg, valid_bits);
  DEBUG2 ("%s(), DF: %2d, modeS_message_score(): %d\n", func, DF, rc);

  mm.timestamp_msg     = ts;
  mm.sys_timestamp_msg = g_data.GPS.detected ? ts : MSEC_TIME();

  rc = decode_mode_S_message (&mm, msg);
  if (rc == 0)
  {
    modeS_user_message (&mm);
  }
  else
  {
    g_data.stat.mode_S_errors++;
    DEBUG1 ("%s(): DF: %2d, decode_mode_S_message(): %d\n", func, DF, rc);
  }
}

/**
 * Decode msg-type 0x32; "Mode-S Short Squitter" raw data at `pkt->msg + 2`
 *
 * Comparable to `readBeast()` in readsb.
 */
static void decode_msg_32 (const RX_packet *pkt, uint32_t msg_len)
{
  assert (pkt->msg_len >= MODES_SHORT_MSG_BYTES);   /* >= 7 */
  decode_common (pkt, MODES_SHORT_MSG_BITS, __FUNCTION__);
}

/**
 * Decode msg-type 0x33; "Mode-S Extended Squitter" raw data at `pkt->msg + 2`
 */
static void decode_msg_33 (const RX_packet *pkt, uint32_t msg_len)
{
  assert (pkt->msg_len >= MODES_LONG_MSG_BYTES);  /* >= 14 */
  decode_common (pkt, MODES_LONG_MSG_BITS, __FUNCTION__);
}

/**
 * Decode msg-type 0x34; No idea what this is. Show it raw.
 */
static void decode_msg_34 (const RX_packet *pkt, uint32_t msg_len)
{
  const uint8_t *msg = pkt->msg;

  DEBUG1 ("%s(): msg[0]: 0x%02X, msg[1]: 0x%02X, msg_len: %u\n",
          __FUNCTION__, msg[0], msg[1], msg_len);
  HEX_DUMP1 (msg, msg_len, NULL);
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
static void show_firmware_resp (const uint8_t *cmd, uint32_t cmd_len)
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
  char             *comma;
  uint16_t          flags;
  time_t            xTime;
  uint8_t           ID, len;
  bool              gps_valid, gps_fix;
  pos_t             gps_pos = { 360.0, 360.0 };   /* not a VALID_POS() */
  const char       *junk1 = NULL;             /* local junk checks */
  const char       *junk2 = NULL;
  const char       *junk3 = NULL;
  const uint8_t    *msg   = pkt->msg;
  const uint8_t    *firmware = msg + 4;
  const status_msg *hsm = (const status_msg*) &pkt->msg [4];

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
    show_firmware_resp (firmware, len);
    return;
  }

  /* ID == 0x01 */

  if (len < sizeof(*hsm))
  {
    junk3 = ", junk-data-3!";
    g_data.stat.pkt_junk++;
    return;
  }

  flags = mg_ntohs (hsm->flags);
  xTime = mg_ntohl (hsm->xTime);

  DEBUG2 ("%s(): ID: %u, len: %u, msg[0]: 0x%02X, msg[1]: 0x%02X, msg[2]: 0x%02X, msg[3]: 0x%02X, msg_len: %u"
          "%s%s%s\n", __FUNCTION__, ID, len, msg[0], msg[1], msg[2], msg[3], msg_len,
          junk1 ? junk1 : "",
          junk2 ? junk2 : "",
          junk3 ? junk3 : "");

  HEX_DUMP2 (msg, msg_len, NULL);

  gps_valid = gps_fix = false;
  g_data.GPS.detected = false;

  if ((flags & 0x8000) == 0x8000)
  {
    strcat_s (flags_str, sizeof(flags_str), "GPS detected, ");
    g_data.GPS.detected = true;
  }

  if ((flags & 0x4000) == 0x4000)
  {
    strcat_s (flags_str, sizeof(flags_str),  "GPS valid, ");
    gps_valid = true;
  }

  if ((flags & 0x2000) == 0x2000)
  {
    strcat_s (flags_str, sizeof(flags_str),  "GPS fix, ");
    gps_fix = true;
  }

  if ((flags & 0x1000) == 0x1000)
     strcat_s (flags_str, sizeof(flags_str),  "PPS time, ");

  /* 4 unused bits */

  if ((flags & 0x0080) == 0x0080)
     strcat_s (flags_str, sizeof(flags_str),  "Tx-queue overflow since start-up, ");

  if ((flags & 0x0040) == 0x0040)
     strcat_s (flags_str, sizeof(flags_str),  "Tx-queue overflow during last second, ");

  if ((flags & 0x0020) == 0x0020)
     strcat_s (flags_str, sizeof(flags_str),  "Excessive NMEA, ");

  DEBUG2 ("msg_len:        %u, sizeof(*hsm): %zd, len: %u\n", msg_len, sizeof(*hsm), len);
  DEBUG2 ("hsm.serial:     %u\n", mg_ntohl (hsm->serial));

  comma = strrchr (flags_str, ',');   /* remove last ", " */
  if (comma)
     *comma = '\0';

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
    gps_pos.lat = binary_angle (hsm->latitude);
    gps_pos.lon = binary_angle (hsm->longitude);

    g_data.GPS.satellites = hsm->satellites;   /* no swap; only 8-bit */
    g_data.GPS.altitude   = mg_ntohs (hsm->altitude);
    g_data.GPS.HDOP       = (double)hsm->hdop / 10;

    DEBUG2 ("gps_valid:      true\n");
    DEBUG2 ("hsm.latitude:   %+10.08f\n", g_data.GPS.pos.lat);
    DEBUG2 ("hsm.longitude:  %+10.08f\n", g_data.GPS.pos.lon);
    DEBUG2 ("hsm.altitude:   %d\n", g_data.GPS.altitude);
    DEBUG2 ("hsm.satellites: %d\n", g_data.GPS.satellites);
    DEBUG2 ("hsm.hdop:       %.2f\n", g_data.GPS.HDOP);   /* HDOP; Horizontal Dilution of Precision */

    if (gps_fix && g_data.GPS.detected &&
        !junk1 && !junk2 && VALID_POS(gps_pos) &&
        g_data.GPS.HDOP < GPS_HDOP_WORST)                 /* lower number is better */
    {
      g_data.GPS.pos = gps_pos;

      if (hsm->satellites <= GPS_HAVE_FIX_LOW && g_data.GPS.have_fix)
      {
        g_data.GPS.have_fix = false;
        g_data.stat.GPS_fix_lost++;
        DEBUG1 ("Lost GPS-fix\n");
      }
      else if (hsm->satellites >= GPS_HAVE_FIX_HIGH && !g_data.GPS.have_fix)
      {
        g_data.GPS.have_fix = true;
        g_data.stat.GPS_fix_regained++;
        DEBUG1 ("Regained GPS-fix\n");
      }
    }
  }

  if (g_data.gps_file)
  {
    FILETIME  now;
    ULONGLONG elapsed_s;
    double    delta_gc_distance = 0.0;
    static pos_t _home_pos = { 60.3053642, 5.3041353 };
    static bool done_header = false;

    if (!done_header)
    {
      fprintf (g_data.gps_file, "# seconds, valid,  latitude,       longitude,    altitude,     "
               "delta great circle distance, my pos: %+.08f, %+.08f\n",
               _home_pos.lat, _home_pos.lon);
      done_header = true;
    }

    get_FILETIME_now (&now);
    elapsed_s = *(ULONGLONG*) &now - *(ULONGLONG*) &Modes.start_FILETIME; /* in 100 nsec units */

    gps_pos.lat = binary_angle (hsm->latitude);
    gps_pos.lon = binary_angle (hsm->longitude);

    delta_gc_distance = geo_great_circle_dist (&gps_pos, &_home_pos);

    fprintf (g_data.gps_file, "%4.0f:      %d,      %+13.08f, %+13.08f, %5u,        %.3f\n",
             (double)elapsed_s / 1E7, gps_valid && !junk1 && !junk2, gps_pos.lat, gps_pos.lon,
             mg_ntohs(hsm->altitude), delta_gc_distance);
  }
}

/**
 * Check the packet for too short or too big `pkt->msg_len`.
 */
static bool pkt_check (const RX_packet *pkt)
{
  if (pkt->msg_len < RX_MIN_SIZE)
  {
    g_data.stat.pkt_too_short++;
    g_data.stat.pkt_too_short_bytes += pkt->msg_len;
    DEBUG2 ("msg_len: %u, msg_type: 0x%02X, too short. Min-size: %zd\n",
            pkt->msg_len, pkt->msg_type, RX_MIN_SIZE);
    HEX_DUMP1 (pkt->msg, pkt->msg_len, ", too short");
    return (false);
  }

  if (pkt->msg_len >= RX_MAX_SIZE)
  {
    g_data.stat.pkt_too_big++;
    DEBUG2 ("msg_len: %u, msg_type: 0x%02X, too big. Max-size: %zd\n", pkt->msg_len, pkt->msg_type, RX_MAX_SIZE);
    HEX_DUMP2 (pkt->msg, min(pkt->msg_len, 200), ", too big");
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
  RX_packet *copy = NULL;
  bool  rc;

  EnterCriticalSection (&g_data.crit);

  g_data.stat.pkt_enqueued_bytes += pkt->msg_len;

  copy = malloc (sizeof(*pkt));
  if (!copy)
  {
    g_data.stat.pkt_OOM++;
    DEBUG1 ("malloc() failed\n");
    rc =  false;
    goto failed;
  }

  assert (g_data.got_x1A);
  assert (pkt->msg_len > 0);
  assert (pkt->msg [0] == 0x1A);  /* SOP */

  copy->msg [0]  = pkt->msg [0];
  copy->msg [1]  = pkt->msg [1];
  copy->msg_type = pkt->msg [1];
  copy->usec     = get_usec_now();
  copy->next     = NULL;

  const uint8_t *src = pkt->msg + 2;
  uint8_t       *dst = copy->msg + 2;
  uint32_t       i, unstuffed;

  for (i = unstuffed = 0; i < pkt->msg_len - 2; i++)
  {
    if (*src == 0x1A)
    {
      unstuffed++;  /* unstuff; skip the extra x1A */
      *src++;
    }
    *dst++ = *src++;
  }

  copy->msg_len = 2 + i - unstuffed;

  rc = pkt_check (copy);

  if (g_data.hex_file)
  {
    hex_dump_csv (g_data.hex_file, pkt->msg,  pkt->msg_len,  "before:", rc);
    hex_dump_csv (g_data.hex_file, copy->msg, copy->msg_len, "after: ", rc);
    fputs ("\n", g_data.hex_file);
  }

  if (!rc)
     goto failed;

  switch (copy->msg_type)
  {
    case MODES_SHORT_SQ:       /* 0x32 */
         g_data.stat.rx_packets_32++;
         g_data.stat.rx_unstuffed_32 += unstuffed;
         break;
    case MODES_EXT_SQ:         /* 0x33 */
         g_data.stat.rx_packets_33++;
         g_data.stat.rx_unstuffed_33 += unstuffed;
         break;
    case HULC_MSG_34:          /* 0x34 */
         g_data.stat.rx_packets_34++;
         g_data.stat.rx_unstuffed_34 += unstuffed;
         break;
    case HULC_STATUS:          /* 0x48 */
         g_data.stat.rx_packets_48++;
         g_data.stat.rx_unstuffed_48 += unstuffed;
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

failed:
  if (!rc && copy)
     free (copy);

  LeaveCriticalSection (&g_data.crit);
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
 * Return the name of a state-function. Handy for tracing.
 */
const char *state_name (state_func f)
{
  return (f == state_get_sync ? "state_get_sync" :
          f == state_put_ch   ? "state_put_ch  " :
          f == state_got_1A   ? "state_got_1A  " : "?");
}

/*
 * Wait for a single `ch == 0x1A` to mark the *END* of a packet.
 * Then enter `state_put_ch()`. Otherwise discard this `ch`.
 *
 * In the extremely seldom case, that the FSM starts with 0x1A,0x1A
 * then wait for another single 0x1A marking the packet.
 */
static void state_get_sync (uint8_t ch, int idx)
{
#if 0
  if (g_data.old_ch == 0x1A && ch != 0x1A)
#else
  if (ch == 0x1A)
#endif
  {
    g_data.pkt_current.msg [0] = 0x1A;
    g_data.pkt_current.msg_len = 1;
    g_data.old_ch  = -1;      /* we do not know or care */
    g_data.got_x1A = true;

    DEBUG1 ("got x1A sync\n");
    g_data.state = state_put_ch;
  }
}

/*
 * If `ch == 0x1A` enter `state_got_1A()` to possibly unstuff.
 * Otherwise append `ch` byte to `g_data.pkt_current.msg []`.
 */
static void state_put_ch (uint8_t ch, int idx)
{
  if (ch == 0x1A)
  {
//  g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;
    g_data.state = state_got_1A;
  }
  else
  {
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = ch;
  }
}

/*
 * Got 0xx1A, check if this `ch` is a 0x1A too.
 * If so, do the unstuffing is done in `pkt_enqueue()`.
 * Otherwise, call `pkt_enqueue()` for current packet and start a new.
 */
static void state_got_1A (uint8_t ch, int idx)
{
  if (ch == 0x1A)
  {
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;
  }
  else
  {
    DEBUG2 ("ch: %s, old_ch: %s,                                   "
            "idx: %4d, old_idx: %4d, pkt_current.msg_len: %u, calling pkt_enqueue()\n",
            ch_str(ch), ch_str(g_data.old_ch),
            idx, g_data.old_idx,
            g_data.pkt_current.msg_len);

    pkt_enqueue (&g_data.pkt_current);

    /* Restart `g_data.pkt_current`
     */
    memset (&g_data.pkt_current, '\0', sizeof(g_data.pkt_current));

    g_data.pkt_current.msg [0] = 0x1A;
    g_data.pkt_current.msg [1] = ch;    /* First `ch` in next `pkt_current.msg` */
    g_data.pkt_current.msg_len = 2;
  }

  g_data.state = state_put_ch;
}

/**
 * \def DEBUG_CH()
 * \li Trace current `ch` and `g_data.old_ch`.
 * \li Show the state transition; old-state -> current state.
 * \li Show the `g_data.sio_buf[]` indices.
 */
#define DEBUG_CH(idx)                                                     \
        DEBUG2 ("ch: %s, old_ch: %s, %s -> %s, idx: %4d, old_idx: %4d\n", \
                ch_str(ch), ch_str(g_data.old_ch),                        \
                state_name(old_state), state_name(g_data.state),          \
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
 * Possibly fill up `g_data.sio_buf[]` with fresh data.
 * Return the max-index for new or old data.
 *
 * \li If `g_data.old_data == true`, continue reading from old-index.
 * \li If `g_data.old_data == false`, read from `[ 0 -- *idx_max >`.
 */
static bool get_COM_data (int *idx_max)
{
  if (g_data.old_data)   /* process old data */
  {
    *idx_max = min (g_data.sio_len, sizeof(g_data.pkt_current.msg)) + g_data.old_idx;

    DEBUG_IDX ("old data", g_data.old_idx, *idx_max);
    return (true);
  }

  /* Fill up with fresh data
   */
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
      return (false);
    }
    return (true);
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
  DEBUG_IDX ("fresh data", g_data.old_idx, *idx_max);

  return (true);
}

/**
 * The detail of it all is here.
 */
static int hulc_read (void)
{
  int idx, delta_idx, start_idx, idx_max = -1;
  int old_enqueued = g_data.stat.pkt_enqueued;

  state_func old_state;

  if (!get_COM_data(&idx_max))
     return (-1);

  if (!g_data.old_data && g_data.sio_len == 0)
  {
    DEBUG2 ("Nothing to do\n");
    return (0);
  }

  start_idx = g_data.old_idx;

  for (idx = start_idx; idx < idx_max; idx++)
  {
    int ch = g_data.sio_buf [idx];

    old_state = g_data.state;

    (*g_data.state) (ch, idx);

    DEBUG_CH (idx);

    g_data.old_ch = ch;
  }

  delta_idx = idx - start_idx;

  if (delta_idx < g_data.sio_len)
  {
    g_data.old_data = true;
    g_data.sio_len -= delta_idx;
    g_data.old_idx += delta_idx;

    DEBUG2 ("Processed: [%d - %d>, remaining sio_len: %d, new old_idx: %d\n", start_idx, idx, g_data.sio_len, g_data.old_idx);
    assert (g_data.sio_len > 0);

    /* Process the rest of `g_data.sio_buf` on next call;
     * from current `idx`.
     */
  }
  else
  {
    g_data.old_data = false;
    g_data.old_idx  = 0;
    g_data.sio_len -= delta_idx;
    DEBUG2 ("Processed all: [%d - %d>, sio_len: %d, new old_idx: 0\n", start_idx, idx, g_data.sio_len);
    assert (g_data.sio_len == 0);

    /* On next iteration `g_data.sio_len = COM_read()`
     */
  }
  return (g_data.stat.pkt_enqueued - old_enqueued);
}

/**
 * Stop debug output if we reached max-messages
 */
static void check_max_messages (void)
{
  if (Modes.max_messages > 0 && Modes.stat.messages_shown >= Modes.max_messages)
     Modes.debug &= ~(DEBUG_GNS_HULC | DEBUG_GNS_HULC2);
}

/**
 * Called from `data_thread_fn()`.
 *
 * In an infinite loop, read (buffered) serial data and fill `g_data.pktg_list`
 * with RX_packet (g_data.pkt_current). These packets are then polled by the below
 * `gns_hulc_poll()`.
 */
void gns_hulc_read_loop (void)
{
//mg_timer_add (&Modes.mgr, FIRMWARE_TIMER, MG_TIMER_REPEAT, send_firmware_req, NULL);

  while (!Modes.exit)
  {
    if (hulc_read() < 0)
       break;

    Sleep (Modes.gns_hulc.poll_ms);   /* default 50 msec */
//  check_max_messages();
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

    DEBUG2 ("pkt->msg_type: 0x%02X, pkt->msg_len: %u, diff1: %.1f, diff2: %.1f\n",
            pkt->msg_type, pkt->msg_len, diff1_ms, diff2_ms);

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
           DEBUG2 ("Rx-unknown: msg[0]: 0x%02X, msg[1]: 0x%02X, msg[2]: 0x%02X, msg_len: %u\n",
                   msg[0], msg[1], msg[2], pkt->msg_len);
           HEX_DUMP2 (msg, pkt->msg_len, NULL);
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
  double ret = (double) mg_ntohl (angle) * STEP_SIZE;

  if (ret > 180.0)
     ret -= 360.0;
  assert (ret >= -180.0 && ret < 180.0);
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
  static char buf [2][3];
  static int  idx = 0;
  char       *ret = buf [idx];

  if (ch == -1)
     return ("-1");

  ret [0] = hex_digits [(uint8_t)ch >> 4];
  ret [1] = hex_digits [(uint8_t)ch & 15];
  ret [2] = '\0';

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

  EnterCriticalSection (&Modes.print_mutex);

  LOG_STDOUT (GNS_FILE "(%u): len: %zd%s\n", line, len, what ? what : "");

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

  LeaveCriticalSection (&Modes.print_mutex);
}

static void hex_dump_csv (FILE *f, const uint8_t *buf, size_t len, const char *what, bool rc)
{
  fprintf (f, "%s rc: %d: ", what, rc);

  for (size_t i = 0; i < len; i++)
  {
    fprintf (f, "%02X", buf[i]);
    if (i < len - 1)
       fputc (' ', f);
  }
  fputs ("\n", f);
}

