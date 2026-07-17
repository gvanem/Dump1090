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
#include "aircraft.h"

#define USE_WAITCOMMEVENT  0
#define USE_GPS_FILE       0

#define GPS_HAVE_FIX_LOW   5
#define GPS_HAVE_FIX_HIGH  10
#define GPS_HDOP_WORST     20

GNS_priv g_data;

static void        set_defaults (void);
static void        send_all_options (void);
static void        state_get_sync (uint8_t ch);
static void        state_put_ch (uint8_t ch);
static void        state_got_1A (uint8_t ch);
static void        pkt_enqueue (const RX_packet *pkt);
static void        pkt_free (void);
static void        pkt_add_junk (_Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(1, 2);
static double      binary_angle (int32_t angle);
static const char *hex_ch_str (int ch);
static void        hex_dump (const uint8_t *buf, size_t len, unsigned line, const char *what);

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

  FREE (Modes.gns_hulc.name);
  Modes.gns_hulc.name = mg_mprintf ("HULC-%d", Modes.gns_hulc.port);

  FREE (Modes.selected_dev);
  Modes.selected_dev = mg_mprintf ("gns-hulc%d", Modes.gns_hulc.port);
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
 * Config-callback for "hulc-bufsize = <uint32_t value>".
 * Lowest minimum is `IOBUF_SIZE`.
 */
bool gns_hulc_set_buf_size (const char *arg)
{
  char   *end;
  int64_t val = strtoll (arg, &end, 10);

  set_defaults();

  if (end > arg && val > 0 && val < UINT_MAX)
       COM_RX_SIZE = max (val, IOBUF_SIZE);
  else cfg_illegal_val ("hulc-bufsize", arg);
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
       Modes.gns_hulc.poll_ms = val;
  else cfg_illegal_val ("hulc-poll-ms", arg);
  return (true);
}

/**
 * Initialise all GNS-HULC stuff once.
 */
HANDLE gns_hulc_init (uint16_t port)
{
  HANDLE hnd;
  char  *file = NULL;

  set_defaults();

  g_data.sio_buf = calloc (COM_RX_SIZE, 1);

  InitializeCriticalSection (&g_data.crit);

  hnd = COM_init (port);
  if (!hnd)
  {
    LOG_STDERR ("COM_init (\"%s\") failed.\n", g_data.COM.dev_name);
    gns_hulc_exit (NULL);
    return (NULL);
  }

  LOG_STDERR ("Running GNS %s on `%s' (%s)\n",
              g_data.Beast.enable ? "Beast" : "HULC",
              g_data.COM.dev_name, g_data.COM.name_space);

  g_data.pkt_current.msg_len = 0;
  g_data.old_ch  = -1;
  g_data.got_x1A = false;

  DEBUG1 ("sio_buf_size: %d, state: state_get_sync()\n", COM_RX_SIZE);
  g_data.state = state_get_sync;

#if USE_GPS_FILE
  file = mg_mprintf ("%s\\gns-gps.txt", Modes.tmp_dir);
  g_data.gps_file = fopen (file, "w+");
  free (file);
#endif

  if (!Modes.error_correct_1 && !Modes.error_correct_2)
     g_data.Beast.FEC = false;

  (void) file;
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
    CloseHandle (hnd);
  }

  FREE (Modes.gns_hulc.name);
  FREE (g_data.COM.dev_name);
  FREE (g_data.sio_buf);
  FCLOSE (g_data.gps_file);
  pkt_free();

  DeleteCriticalSection (&g_data.crit);
}

/**
 * Show statistics. Called from main() before exit.
 */
void gns_hulc_stats (void)
{
  uint64_t sum = g_data.stat.rx_packets_32 + g_data.stat.rx_packets_33 +
                 g_data.stat.rx_packets_48 + g_data.stat.rx_packets_unknown;

  if (sum == 0ULL)
     return;

  LOG_STDOUT ("! \n");
  LOG_STDOUT ("GNS-HULC statistics:\n");
  LOG_STDOUT (" %8llu RX-packets-32.\n", g_data.stat.rx_packets_32);
  LOG_STDOUT (" %8llu RX-packets-33.\n", g_data.stat.rx_packets_33);
  LOG_STDOUT (" %8llu RX-packets-48.\n", g_data.stat.rx_packets_48);
  LOG_STDOUT (" %8llu RX-unknown.\n", g_data.stat.rx_packets_unknown);
  LOG_STDOUT (" %8llu RX-junk-data.\n", g_data.stat.pkt_junk);
  LOG_STDOUT (" %8llu RX-overruns.\n", g_data.stat.rx_overruns);
  LOG_STDOUT (" %8llu TX-packets.\n", g_data.stat.tx_packets);
  LOG_STDOUT (" %8llu Too short packets (%.2f%%).\n", g_data.stat.pkt_too_short, (100.0 * g_data.stat.pkt_too_short) / sum);
}

uint64_t gns_hulc_junk (void)
{
  return (g_data.stat.pkt_junk);
}

uint64_t gns_hulc_overrun (void)
{
  return (g_data.stat.rx_overruns);
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

  g_data.COM.baud_rate  = COM_BAUD_RATE;
  g_data.sio_buf_size   = IOBUF_SIZE;   /* 2 kByte */
  g_data.GPS.have_fix   = false;
  g_data.GPS.enable     = false;
  g_data.GPS.satellites = 0;

  g_data.Beast.enable         = false;
  g_data.Beast.filter_DF045   = false;
  g_data.Beast.filter_DF1117  = false;
  g_data.Beast.mode_AC        = Modes.mode_AC = false;
  g_data.Beast.mlat_timestamp = true;
  g_data.Beast.FEC            = true;
  g_data.Beast.CRC            = true;
}

/**
 * Called for both 0x32 and 0x33 message-types.
 *
 * Decode the header from a `header_32_33` header
 * and return a relative/absolute timestamp and signal-level.
 */
static void decoder_header (const header_32_33 *hdr, uint64_t *ts_msec, double *sig_level, const char *func)
{
  static bool done = false;    /* got `tz_info`? */
  static LONG timezone = 0;    /* in minutes */
  static LONG dst_adjust = 0;  /* in minutes */
  static LONG UTC_adjust = 0;  /* in minutes */
  const char *junk = NULL;     /* got local junk data? */
  char        ts_buf [200];    /* time-stamp buffer */
  uint64_t    timestamp;

  *ts_msec   = 0ULL;
  *sig_level = 0.0;

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

  /* With no GPS, the timestamp is a 48-bit unsigned number counting at 12 MHz.
   */
  if (!g_data.GPS.detected)
  {
    timestamp = (mg_ntohl (hdr->ts1) << 16) + mg_ntohs (hdr->ts2);
    snprintf (ts_buf, sizeof(ts_buf), "relative %.0f", timestamp / 12*1E6);
    *ts_msec = timestamp;
  }
  else
  {
    /* With GPS detected, the upper 18 bits are seconds since last midnight 00:00:00 UTC
     */
    timestamp = mg_ntohl (hdr->ts1) >> (32 - 18);

    if (timestamp >= 24*3600)
    {
      junk = "timestamp-junk";
      pkt_add_junk ("%s: %llu", junk, timestamp);
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
    snprintf (ts_buf, sizeof(ts_buf), "absolute %llu, nanosec: %10u: %02u:%02u:%02u:%06u",
              timestamp, nanosec, hours, minutes, seconds, microsec);
  }

  char dbFS [10] = "-";

  if (hdr->RSSI > 0)
  {
    double level = (double) hdr->RSSI / 255.0;   /* is this right? */

    snprintf (dbFS, sizeof(dbFS), "%.1f", 10.0 * log10(level));
    *sig_level = level;
  }

  if (junk)
       DEBUG1 ("%s(): timestamp: %s, RSSI: %d / %s dBFS, %s\n\n",
               func, ts_buf, hdr->RSSI, dbFS, junk);
  else DEBUG2 ("%s(): timestamp: %s, RSSI: %d / %s dBFS\n\n",
               func, ts_buf, hdr->RSSI, dbFS);
}

/**
 * Common decoders both 0x32 and 0x33 message-types.
 *
 * The only variable is number of bits in `msg`.
 * For `decode_msg_32()` it is `MODES_SHORT_MSG_BITS == 56` and
 * For `decode_msg_33()` it is `MODES_LONG_MSG_BITS == 112`.
 */
static void decode_common (const uint8_t *msg, int valid_bits, const char *func)
{
  const header_32_33 *hdr;
  uint64_t            timestamp;
  double              sig_level;
  int                 DF, score, rc;
  bool                DF_handled;
  modeS_message       mm;

  /* The DFs we handle:
   */
  static int valid_DFs[] = {  0,  4,  5, 11, 16, 17, 18, 20,
                             21, 24, 25, 26, 27, 28, 29, 30, 31 };

  /* Extra DFs valid for non-interactive mode.
   * Ref. modeS_message_display()
   */
  static int extra_DFs[] = { 19,    /* Military Extended Squitter */
                             22,    /* Military Use */
                             32 };  /* Special code for Mode A/C */

  hdr = (const header_32_33*) msg;
  decoder_header (hdr, &timestamp, &sig_level, func);

  msg = (const uint8_t*) (hdr + 1);

  /* Check and handle Downlink Format
   */
  DF = msg [0] >> 3;
  DF_handled = (memchr (valid_DFs, DF, sizeof(valid_DFs)) != NULL);
  if (!Modes.interactive)
      DF_handled |= (memchr (extra_DFs, DF, sizeof(extra_DFs)) != NULL);

  if (!DF_handled)
  {
    DEBUG2 ("%s(): DF %-2d unhandled\n", func, DF);
    return;
  }
  score = modeS_message_score (msg, valid_bits);
  if (score < 0)
  {
    DEBUG2 ("%s(): DF %-2d score: %d\n", func, DF, score);
    return;
  }

  mm.timestamp_msg     = timestamp;
  mm.sys_timestamp_msg = g_data.GPS.detected ? timestamp : MSEC_TIME();

  rc = decode_mode_S_message (&mm, msg);
  if (rc == 0)
  {
    aircraft *a;

    modeS_user_message (&mm);
    a = aircraft_find (mm.addr);
    if (a)
    {
      a->sig_levels [a->sig_idx++] = sig_level;
      a->sig_idx &= DIM(a->sig_levels) - 1;
    }
  }
  else
  {
    g_data.stat.mode_S_errors++;
    DEBUG1 ("%s(): DF %-2d, decode_mode_S_message(): %d\n", func, DF, rc);
  }
}

/**
 * Decode msg-type 0x32; "Mode-S Short Squitter" raw data at `pkt->msg + 2`
 */
static void decode_msg_32 (const RX_packet *pkt, int msg_len)
{
  assert (msg_len >= MODES_SHORT_MSG_BYTES);   /* >= 7 */
  decode_common (pkt->msg + 2, MODES_SHORT_MSG_BITS, __FUNCTION__);
}

/**
 * Decode msg-type 0x33; "Mode-S Extended Squitter" raw data at `pkt->msg + 2`
 */
static void decode_msg_33 (const RX_packet *pkt, int msg_len)
{
  assert (msg_len >= MODES_LONG_MSG_BYTES);  /* >= 14 */
  decode_common (pkt->msg + 2, MODES_LONG_MSG_BITS, __FUNCTION__);
}

/**
 * Decode msg-type 0x34; No idea what this is. Show it raw.
 */
static void decode_msg_34 (const RX_packet *pkt, int msg_len)
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
 * sent in ` send_firmware_req()`.
 */
static void show_firmware_resp (const uint8_t *cmd, uint32_t cmd_len)
{
  if (cmd_len != 16)
  {
    pkt_add_junk ("Firmware-junk: %u", cmd_len);
    return;
  }

  /**
   * A 16-byte response is like:
   * ```
   *  00 00 80 04 81           -- Fixed 00-80-04-81 for compatibility reasons
   *  15 0A 01                 -- yy-ww-bb is Version year, week, build-number
   *  01 02 03 01 00 00 00 00  -- Internal Use
   * ```
   */
  DEBUG1 ("len: %u, yy-ww-bb: %02X %02X %02X\n", cmd_len, cmd[5], cmd[6], cmd[7]);
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
static void decode_msg_48 (const RX_packet *pkt, int msg_len)
{
  char              flags_str [200] = "";
  char             *comma;
  uint16_t          flags;
  time_t            xTime;
  uint8_t           ID, len;
  bool              gps_valid, gps_fix;
  const uint8_t    *msg = pkt->msg;
  const status_msg *hsm = (const status_msg*) &pkt->msg [4];

  ID = msg [2];

  /**
   * Command lengths:
   *   for ID = 0x01 it should be 24
   *   for ID = 0x24 it should be 16.
   */
  len = msg [3];

  if (ID == 0x24)
  {
    HEX_DUMP1 (pkt->msg, pkt->msg_len, ", Firmware-resp:");
    show_firmware_resp (msg + 4, len);
    return;
  }
  if (ID != 0x01)
  {
    pkt_add_junk ("ID: %u", ID);
    return;
  }

  if (len < sizeof(*hsm))
  {
    pkt_add_junk ("ID: 1, len: %u", len);
    return;
  }

  flags = mg_ntohs (hsm->flags);
  xTime = mg_ntohl (hsm->xTime);

  DEBUG2 ("%s(): ID: %u, len: %u, msg[0]: 0x%02X, msg[1]: 0x%02X, msg[2]: 0x%02X, msg[3]: 0x%02X, msg_len: %d\n",
          __FUNCTION__, ID, len, msg[0], msg[1], msg[2], msg[3], msg_len);

  HEX_DUMP2 (msg, msg_len, NULL);

  gps_valid = gps_fix = false;

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

  DEBUG2 ("msg_len:        %d, sizeof(*hsm): %zd, len: %u\n", msg_len, sizeof(*hsm), len);
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
    pos_t gps_pos = { 360.0, 360.0 };  /* not a VALID_POS() */

    gps_pos.lat = binary_angle (hsm->latitude);
    gps_pos.lon = binary_angle (hsm->longitude);

    g_data.GPS.satellites = hsm->satellites;   /* no swap; only 8-bit */
    g_data.GPS.altitude   = mg_ntohs (hsm->altitude);
    g_data.GPS.HDOP       = (double)hsm->hdop / 10;

    DEBUG2 ("gps_valid:      true\n");
    DEBUG2 ("hsm.latitude:   %+10.08f\n", gps_pos.lat);
    DEBUG2 ("hsm.longitude:  %+10.08f\n", gps_pos.lon);
    DEBUG2 ("hsm.altitude:   %d\n", g_data.GPS.altitude);
    DEBUG2 ("hsm.satellites: %d\n", g_data.GPS.satellites);
    DEBUG2 ("hsm.hdop:       %.2f\n", g_data.GPS.HDOP);   /* HDOP; Horizontal Dilution of Precision */

    if (gps_fix && g_data.GPS.detected &&
        !g_data.pkt_junk && VALID_POS(gps_pos) &&
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
    pos_t        gps_pos2;
    FILETIME     now;
    double       elapsed_s, delta_gc_distance;
    static pos_t home_pos2 = { 60.3053642, 5.3041353 };
    static bool  done = false;

    if (!done)
    {
      fprintf (g_data.gps_file, "# seconds, valid,  latitude,       longitude,    altitude,    "
               "num-sat,  delta GC distance, my pos: %+.08f, %+.08f\n",
               home_pos2.lat, home_pos2.lon);
      done = true;
    }

    get_FILETIME_now (&now);

    /* Program runtime; in 100 nsec units to seconds as a double
     */
    elapsed_s = (double) (*(ULONGLONG*)&now - *(ULONGLONG*)&Modes.start_FILETIME) / 1E7;

    gps_pos2.lat = binary_angle (hsm->latitude);
    gps_pos2.lon = binary_angle (hsm->longitude);

    delta_gc_distance = geo_great_circle_dist (&gps_pos2, Modes.home_pos_ok ? &Modes.home_pos : &home_pos2);

    if (g_data.pkt_junk)
       gps_valid = false;

    fprintf (g_data.gps_file, "%4.0f:      %d,      %+13.08f, %+13.08f, %5u,       %2d,       %.3f\n",
             elapsed_s, gps_valid, gps_pos2.lat, gps_pos2.lon,
             mg_ntohs(hsm->altitude), g_data.GPS.satellites, delta_gc_distance);
  }
}

/**
 * Check the packet for "too short" or "too large" `pkt->msg_len`.
 * Also check minimum size based on message-type.
 */
#define TOO_SHORT(pkt, min_len)                                                    \
        g_data.stat.pkt_too_short++;                                               \
        snprintf (err_buf, sizeof(err_buf), ", too short. min_len: %zd", min_len); \
        HEX_DUMP1 (pkt->msg, pkt->msg_len, err_buf)

#define TOO_LARGE(pkt, max_len)                                                    \
        g_data.stat.pkt_too_large++;                                               \
        snprintf (err_buf, sizeof(err_buf), ", too large. max_len: %zd", max_len); \
        HEX_DUMP1 (pkt->msg, min(pkt->msg_len, 200), err_buf)

static bool pkt_enqueue_check (const RX_packet *pkt)
{
  size_t min_len;
  char   err_buf [40];

  assert (pkt->msg_len > 0);
  assert (pkt->msg [0] == 0x1A);    /* SOP; Start-of-Packet */

  if (pkt->msg_len < RX_MIN_SIZE)   /* == 16 */
  {
    TOO_SHORT (pkt, RX_MIN_SIZE);
    return (false);
  }

  if (pkt->msg_len >= RX_MAX_SIZE)  /* Impossible */
  {
    TOO_LARGE (pkt, RX_MAX_SIZE);
    return (false);
  }

  switch (MSG_TYPE(pkt))
  {
    case MODES_SHORT_SQ:
         g_data.stat.rx_packets_32++;
         min_len = 2 + /* sizeof(header_32_33) + */ MODES_SHORT_MSG_BYTES;   /* == 2 + 7 + 7 == 16 */
         break;

    case MODES_EXT_SQ:
         g_data.stat.rx_packets_33++;
         min_len = 2 + /* sizeof(header_32_33) + */ MODES_LONG_MSG_BYTES;    /* == 2 + 7 + 14 == 23 */
         break;

    case HULC_MSG_34:
         g_data.stat.rx_packets_34++;
         min_len = 5;
         break;

    case HULC_STATUS:
         g_data.stat.rx_packets_48++;
         if (pkt->msg[2] == 0x01)               /* Periodic HULC Status Message, == 4 + 20  */
              min_len = 4 + sizeof(status_msg);
         else if (pkt->msg[2] == 0x24)          /* Reply to Command, == 20 */
              min_len = 4 + 16;
         else min_len = RX_MIN_SIZE;            /* == 16 */
         break;

    default:
         g_data.stat.rx_packets_unknown++;
         snprintf (err_buf, sizeof(err_buf), ", Unknown, msg_len: %d, msg_type: 0x%02X",
                   pkt->msg_len, MSG_TYPE(pkt));
         DEBUG1 ("%s\n", err_buf + 2);
         HEX_DUMP1 (pkt->msg, min(pkt->msg_len, sizeof(pkt->msg)), err_buf);
         return (false);
  }

  if (pkt->msg_len < min_len)
  {
    TOO_SHORT (pkt, min_len);
    return (false);
  }
  return (true);
}

/**
 * Add a parsed packet `== g_data.pkt_current` to the end of the packet queue.
 *
 * The critical-section `g_data.crit` is held on entry.
 */
static void pkt_enqueue (const RX_packet *pkt)
{
  RX_packet *copy;

  if (!pkt_enqueue_check(pkt))
     return;

  copy = malloc (sizeof(*pkt));
  if (!copy)
  {
    g_data.stat.pkt_OOM++;
    DEBUG1 ("malloc() failed\n");
    return;
  }

  copy->msg [0] = pkt->msg [0];
  copy->msg [1] = pkt->msg [1];
  copy->usec    = get_usec_now();
  copy->next    = NULL;

  const uint8_t *src = pkt->msg + 2;
  uint8_t       *dst = copy->msg + 2;
  int            i, unstuffed;

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

  LIST_ADD_TAIL (RX_packet, (RX_packet**)&g_data.pkt_list, copy);

  g_data.stat.pkt_enqueued++;
}

/**
 * Free all elements in `g_data.pkt_list`.
 */
static void pkt_free (void)
{
  volatile RX_packet *pkt, *pkt_next;

  EnterCriticalSection (&g_data.crit);

  for (pkt = g_data.pkt_list; pkt; pkt = pkt_next)
  {
    LIST_DELETE (RX_packet, (RX_packet**)&g_data.pkt_list, pkt);
    pkt_next = pkt->next;
    free ((void*)pkt);
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

/**
 * Wait for a single `ch == 0x1A` to mark the *END* of a packet.
 * Then enter `state_put_ch()`. Otherwise discard this `ch`.
 *
 * Also send all start-up options; Firmware-request or Beast options.
 */
static void state_get_sync (uint8_t ch)
{
  if (ch == 0x1A)
  {
    g_data.pkt_current.msg [0] = 0x1A;
    g_data.pkt_current.msg_len = 1;
    g_data.old_ch  = -1;      /* we do not know or care */
    g_data.got_x1A = true;

    DEBUG1 ("got x1A sync\n");
    g_data.state = state_put_ch;

    send_all_options();
  }
  else
    g_data.old_ch = ch;
}

/**
 * If `ch == 0x1A` enter `state_got_1A()` to possibly unstuff.
 * Otherwise append `ch` byte to `g_data.pkt_current.msg[]`.
 */
static void state_put_ch (uint8_t ch)
{
  if (ch == 0x1A)
  {
    g_data.state  = state_got_1A;
    g_data.old_ch = 0x1A;
  }
  else
  {
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = ch;
    g_data.old_ch = ch;
    assert (g_data.pkt_current.msg_len <= sizeof(g_data.pkt_current.msg));
  }
}

/**
 * Got 0x1A, check if this `ch` is a 0x1A too.
 * If so, do the unstuffing in `pkt_enqueue()` later.
 *
 * Otherwise, call `pkt_enqueue()` for current packet and start a new.
 */
static void state_got_1A (uint8_t ch)
{
  if (ch == 0x1A && g_data.old_ch == 0x1A)
  {
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;
    g_data.pkt_current.msg [g_data.pkt_current.msg_len++] = 0x1A;
    g_data.old_ch = 0x1A;
    assert (g_data.pkt_current.msg_len <= sizeof(g_data.pkt_current.msg));

    g_data.state = state_put_ch;
  }
  else
  {
    DEBUG2 ("ch: %s, old_ch: %s, pkt_current.msg_len: %d, calling pkt_enqueue()\n",
            hex_ch_str(ch), hex_ch_str(g_data.old_ch), g_data.pkt_current.msg_len);

    EnterCriticalSection (&g_data.crit);

    pkt_enqueue (&g_data.pkt_current);

    LeaveCriticalSection (&g_data.crit);

    /* Restart `g_data.pkt_current`
     */
    memset (&g_data.pkt_current, '\0', sizeof(g_data.pkt_current));
    g_data.old_ch = -1;

    g_data.pkt_current.msg [0]  = 0x1A;
    g_data.pkt_current.msg [1]  = ch;    /* `ch` in next `pkt_current.msg[1]` (msg-type) */
    g_data.pkt_current.msg_len  = 2;

    g_data.state = state_put_ch;
  }
}

/**
 * Trace the legal range of `g_data.sio_buf[]` indices.
 * And if we are processing "old-data" or "fresh-data".
 *
 * Used with `--debug H`.
 */
static void debug_idx (const char *what, int max_idx)
{
  EnterCriticalSection (&Modes.print_mutex);

  DEBUG2 ("Processing %s: sio_len: %d, idx: [%d - %d>, old_idx: %d\n",
          what, g_data.sio_len, g_data.old_idx, max_idx, g_data.old_idx);

  LeaveCriticalSection (&Modes.print_mutex);
}

/**
 * Possibly fill up `g_data.sio_buf[]` with fresh data.
 * Return the max-index for new or old data.
 *
 * \li If `g_data.old_data == true`, continue reading from `g_data.old_idx` until
 *     reaching `g_data.sio_len` or size-of current-packet.
 * \li If `g_data.old_data == false`, read from index 0 up-to minimum of
 *     `g_data.sio_len` and sizeof current-packet.
 */
static bool get_COM_data (int *idx_max)
{
  /* Process old data?
   */
  if (g_data.old_data)
  {
    *idx_max = g_data.old_idx + min (g_data.sio_len, sizeof(g_data.pkt_current.msg));

    if (1 && (Modes.debug & DEBUG_GNS_HULC2))
       debug_idx ("old data", *idx_max);
    return (true);
  }

#if (USE_WAITCOMMEVENT)
  /*
   * Poll for an EV_RXCHAR event and call `COM_read()`.
   */
  if (COM_poll_events())
#endif
  {
    /*
     * Fill up with fresh data
     */
    g_data.sio_len = COM_read (Modes.gns_hulc.handle, g_data.sio_buf, COM_RX_SIZE);
  }

  if (g_data.sio_len < 0)
  {
    LOG_STDERR ("Modes.fatal.exit: COM-port problem; %s\n", win_strerror(GetLastError()));
    Modes.exit = g_data.fatal_exit = true;
    return (false);
  }

  if (g_data.sio_len == 0)
  {
    /**
     * If we are in initial state `state_get_sync()`, check the
     * "dead-counter"
     *
     * \todo
     * This count should depend on port baud-rate.
     */
    if (g_data.state == state_get_sync && ++g_data.COM.dead_count >= COM_DEAD_COUNT)
    {
      LOG_STDERR ("Port `%s' seems dead. No data %u times in a row.\7\n",
                  g_data.COM.dev_name, COM_DEAD_COUNT);
      Modes.exit = Modes.no_stats = true;
      g_data.fatal_exit = true;
      return (false);
    }
    return (true);
  }

  if (g_data.sio_len > 0)
     g_data.COM.dead_count = 0;

  *idx_max = min (g_data.sio_len, sizeof(g_data.pkt_current.msg));
  if (1 && (Modes.debug & DEBUG_GNS_HULC2))
     debug_idx ("fresh data", *idx_max);

  return (true);
}

/**
 * The inner details of `gns_hulc_read_loop()`.
 */
static int hulc_read (void)
{
  int old_enqueued = g_data.stat.pkt_enqueued;
  int idx_max;

  if (!get_COM_data(&idx_max))
     return (-1);

  if (!g_data.old_data && g_data.sio_len == 0)
     return (0);

  int start_idx = g_data.old_idx;
  int idx;

  for (idx = start_idx; idx < idx_max; idx++)
  {
    int        ch        = g_data.sio_buf [idx];
    state_func old_state = g_data.state;

    (*g_data.state) (ch);

    DEBUG2 ("ch: %s, old_ch: %s, %s -> %s, idx: %4d, old_idx: %4d\n",
            hex_ch_str(ch), hex_ch_str(g_data.old_ch),
            state_name(old_state), state_name(g_data.state),
            idx, g_data.old_idx);
  }

  int delta_idx = idx - start_idx;

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

    /* On next iteration sets `g_data.sio_len = COM_read()`
     */
  }

  /* How many packets enqueued in this call?
   */
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
 * Append formatted string to `g_data.pkt_junk`.
 */
static void pkt_add_junk (const char *fmt, ...)
{
  static char  buf [300];
  static char *dst, *end;

  g_data.stat.pkt_junk++;

  if (!g_data.pkt_junk)
  {
    g_data.pkt_junk = buf;
    dst = buf;
    end = dst + sizeof(buf) - 1;
  }

  if (end - dst > 20)
  {
    va_list args;
    va_start (args, fmt);
    dst += vsnprintf (dst, end - dst, fmt, args);
    va_end (args);
  }
}

/**
 * Called from `data_thread_fn()` separate from main-thread.
 *
 * In this infinite loop, read serial data and fill `g_data.pkt_list`
 * with RX_packet (`g_data.pkt_current`).
 */
void gns_hulc_read_loop (void)
{
  while (!Modes.exit)
  {
    if (hulc_read() < 0)
       break;

#if (USE_WAITCOMMEVENT == 0)
    COM_poll_error();
 /* Sleep (10); */
#endif

    check_max_messages();
  }
}

/**
 * Called from `background_tasks()` (in main-thread) to poll and process
 * the `g_data.pkt_list`.
 *
 * Since `LIST_ADD_TAIL()` was used in `pkt_enqueue()`, this will process
 * the oldest packets first. After processing each, free the RX-packet.
 */
void gns_hulc_poll (void)
{
  volatile RX_packet *pkt, *pkt_next;
  uint32_t count = 0;

  /* Something bad happend or we were told to stop.
   */
  if (Modes.exit || g_data.fatal_exit)
     return;

  if (!TryEnterCriticalSection(&g_data.crit))
     return;

  for (pkt = g_data.pkt_list; pkt; pkt = pkt_next, count++)
  {
    DEBUG2 ("count: %u, msg_type: 0x%02X, diff: %.1f msec\n",
            count, MSG_TYPE(pkt), (get_usec_now() -  pkt->usec) / 1E3);

    g_data.pkt_junk = NULL;

    switch (MSG_TYPE(pkt))
    {
      case MODES_SHORT_SQ:
           decode_msg_32 ((const RX_packet*)pkt, pkt->msg_len);
           break;

      case MODES_EXT_SQ:
           decode_msg_33 ((const RX_packet*)pkt, pkt->msg_len);
           break;

      case HULC_MSG_34:
           decode_msg_34 ((const RX_packet*)pkt, pkt->msg_len);
           break;

      case HULC_STATUS:
           decode_msg_48 ((const RX_packet*)pkt, pkt->msg_len);
           break;
    }

    g_data.stat.pkt_dequeued++;

    if (g_data.pkt_junk)
       DEBUG1 ("pkt_junk: %s\n", g_data.pkt_junk);

    LIST_DELETE (RX_packet, (RX_packet**)&g_data.pkt_list, pkt);
    pkt_next = pkt->next;
    free ((void*)pkt);
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

  g_data.stat.tx_packets++;
  HEX_DUMP1 (msg, msg_sz, what);
  return (rc == msg_sz);
}

static void send_firmware_req (void)
{
  DEBUG1 ("%s()\n", __FUNCTION__);
  send_option ((const uint8_t*)"#00\r\n", 5, ", Firmware request:");
}

/**
 * Send a specific Beast option message to GNS-Hulc.
 */
static bool send_beast_option (uint8_t opt)
{
  uint8_t msg [3] = { 0x1A, '1', opt };
  char    what [30];

  snprintf (what, sizeof(what), ", Beast option '%c':", opt);
  return send_option (msg, sizeof(msg), what);
}

/**
 * Send all Beast options
 */
static void send_beast_options (void)
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
  else send_beast_option ('I');    /* FEC disabled */

  if (g_data.Beast.mode_AC)
       send_beast_option ('J');    /* Mode A/C enabled */
  else send_beast_option ('j');    /* Mode A/C disabled */
}

/**
 * Send all Beast / Hulc start-up options
 */
static void send_all_options (void)
{
  if (g_data.Beast.enable)
     send_beast_options();
  send_firmware_req();
}

/*
 * 360 deg / 2^32
 */
#define STEP_SIZE 8.38190317153931E-08

/**
 * Convert 32-bit signed binary angular measure to double degree.
 * See https://www.globalspec.com/reference/14722/160210/Chapter-7-5-3-Binary-Angular-Measure
 *
 * \param in angle on network order
 * \retval Angular degree
 */
static double binary_angle (int32_t angle)
{
  double ret = (double) mg_ntohl (angle) * STEP_SIZE;

  if (ret >= 180.0)
     ret -= 360.0;
  assert (ret >= -180.0 && ret < 180.0);
  return (ret);
}

static char hex_digits[] = "0123456789ABCDEF";

static const char *hex_ch_str (int ch)
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


