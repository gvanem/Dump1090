/**\file    raw-sbs.c
 * \ingroup Decoder
 * \brief   Functions for RAW-IN, RAW-OUT, SBS-IN and SBS-OUT
 */
#include "aircraft.h"
#include "net_io.h"
#include "raw-sbs.h"

/**
 * \def LOG_GOOD_RAW()
 *      if `--debug g` is active, log a good RAW message.
 */
#define LOG_GOOD_RAW(fmt, ...)  TRACE2 ("RAW(%d): " fmt, \
                                        loop_cnt, ## __VA_ARGS__)

/**
 * \def LOG_BOGUS_RAW()
 *      if `--debug g` is active, log a bad / bogus RAW message.
 */
#define LOG_BOGUS_RAW(num, fmt, ...)  TRACE ("RAW(%d), Bogus msg %d: " fmt, \
                                             loop_cnt, num, ## __VA_ARGS__)

/**
 * \def LOG_GOOD_SBS()
 *      if `--debug g` is active, log a good SBS message.
 */
#define LOG_GOOD_SBS(fmt, ...)  TRACE ("SBS(%d): " fmt, loop_cnt, __VA_ARGS__)

/**
 * \def LOG_BOGUS_SBS()
 *      if `--debug g` is active, log a bad / bogus SBS message.
 */
#define LOG_BOGUS_SBS(fmt, ...)  TRACE ("SBS(%d), Bogus msg: " fmt, loop_cnt, __VA_ARGS__)

/**
 * The `readsb` program will send 5 heart-beats like this
 * in RAW mode.
 */
#define RAW_HEART_BEAT  "*0000;\n*0000;\n*0000;\n*0000;\n*0000;\n"

/**
 * \typedef SBS_msg_t
 *
 * The 6 different SBS message types. The important data is in `"MSG,.."`.
 * Details at: http://woodair.net/sbs/article/barebones42_socket_data.htm
 */
typedef enum SBS_msg_t {
        SBS_UNKNOWN = 0,
        SBS_MSG,
        SBS_SEL,
        SBS_ID,
        SBS_AIR,
        SBS_STA,
        SBS_CLK,
      } SBS_msg_t;

static const search_list SBS_types[] = {
                       { SBS_MSG, "MSG," },
                       { SBS_AIR, "AIR," },
                       { SBS_STA, "STA," },
                       { SBS_SEL, "SEL," },
                       { SBS_CLK, "CLK," },
                       { SBS_ID,  "ID," }
                     };

static SBS_msg_t   SBS_message_type (const mg_iobuf *msg);
static bool        SBS_recv_input (char *msg);
static const char *SBS_set_timestamp (char *ts);
static uint64_t    SBS_get_timestamp (const char *ts);
static int         SBS_decode_msg (const char *fields[], modeS_message *mm);

/**
 * Write raw output to TCP clients.
 */
void raw_out_send (const modeS_message *mm)
{
  char  msg [10 + 2*MODES_LONG_MSG_BYTES];
  char *p = msg;

  if (!net_handler_sending(MODES_NET_SERVICE_RAW_OUT))
     return;

  *p++ = '*';
  mg_hex_lower (&mm->msg, mm->msg_bits/8, p);
  p = strchr (p, '\0');
  *p++ = ';';
  *p++ = '\n';
  net_handler_send (MODES_NET_SERVICE_RAW_OUT, msg, p - msg);
}

/**
 * Common for all config-parser callbacks.
 *
 * Parses a "host-X-Y = [tcp|udp]://host:port" and sets
 * `serv->host` and serv->port`.
 */
#define SET_HOST_PORT(arg, serv, def_port) net_set_host_port (arg, serv, def_port)

/**
 * Parses "host-raw-in = tcp://host:port"
 */
bool raw_in_set_host_port (const char *arg)
{
  return SET_HOST_PORT (arg, &modeS_net_services [MODES_NET_SERVICE_RAW_IN], MODES_NET_PORT_RAW_IN);
}

/**
 * Parses "host-raw-out = tcp://host:port"
 */
bool raw_out_set_host_port (const char *arg)
{
  return SET_HOST_PORT (arg, &modeS_net_services [MODES_NET_SERVICE_RAW_OUT], MODES_NET_PORT_RAW_OUT);
}

/**
 * Parses "host-sbs-in = tcp://host:port"
 */
bool sbs_in_set_host_port (const char *arg)
{
  return SET_HOST_PORT (arg, &modeS_net_services [MODES_NET_SERVICE_SBS_IN], MODES_NET_PORT_SBS);
}

/**
 * Parses "net-ro-port = port"
 */
bool raw_out_set_port (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_RAW_OUT].port = (uint16_t) atoi (arg);
  return (true);
}

/**
 * Parses "net-ri-port = port"
 */
bool raw_in_set_port (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_RAW_IN].port = (uint16_t) atoi (arg);
  return (true);
}

/**
 * Parses "net-sbs-port = port"
 */
bool sbs_out_set_port (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_SBS_OUT].port = (uint16_t) atoi (arg);
  return (true);
}

/*
 * Show decoder statistics for a RAW_IN service.
 * Only if we had a connection with such a server.
 *
 * Called at program exit from `net_show_stats()` in `net_io.c`.
 */
void raw_in_stats (void)
{
  if (net_stat_common(MODES_NET_SERVICE_RAW_IN))
  {
    LOG_STDOUT ("  %8llu good messages.\n", Modes.stat.RAW_good);
    LOG_STDOUT ("  %8llu empty messages.\n", Modes.stat.RAW_empty);
    LOG_STDOUT ("  %8llu unrecognized messages.\n", Modes.stat.RAW_unrecognized);
  }
}

void sbs_in_stats (void)
{
  if (net_stat_common(MODES_NET_SERVICE_SBS_IN))
  {
    LOG_STDOUT ("  %8llu good messages.\n", Modes.stat.SBS_good);
    LOG_STDOUT ("  %8llu AIR messages.\n", Modes.stat.SBS_AIR_msg);
    LOG_STDOUT ("  %8llu CLK messages.\n", Modes.stat.SBS_CLK_msg);
    LOG_STDOUT ("  %8llu ID  messages.\n", Modes.stat.SBS_ID_msg);
    LOG_STDOUT ("  %8llu MSG messages.\n", Modes.stat.SBS_MSG_msg);
    LOG_STDOUT ("  %8llu SEL messages.\n", Modes.stat.SBS_SEL_msg);
    LOG_STDOUT ("  %8llu STA messages.\n", Modes.stat.SBS_STA_msg);
    LOG_STDOUT ("  %8llu unrecognized messages.\n", Modes.stat.SBS_unrecognized);
  }
}

/**
 * This is a `net_msg_handler` function.
 *
 * Called from `net_io.c` to decode a RAW-IN message.
 * If OK, calls `decode_mode_S_message()` to fill `&mm` and then
 * calls `modeS_user_message (&mm)` for further handling.
 */
bool raw_decode_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t       bin_msg [MODES_LONG_MSG_BYTES];
  int           len, j;
  uint8_t      *hex, *end;

  if (Modes.exit)
  {
    mg_iobuf_del (msg, 0, msg->len);  /* Quit `net_connection_recv()` loop ASAP */
    return (false);
  }

  if (msg->len == 0)  /* all was consumed */
     return (false);

  end = memchr (msg->buf, '\n', msg->len);
  if (!end)
  {
    mg_iobuf_del (msg, 0, msg->len);
    return (false);
  }

  *end++ = '\0';
  hex = msg->buf;
  len = end - msg->buf - 1;

  if (msg->len >= 2 && end[-2] == '\r')
  {
    end [-2] = '\0';
    len--;
  }

  /* Check for Heart-beat signal
   */
  if (!strcmp((const char*)hex, RAW_HEART_BEAT))
  {
    LOG_GOOD_RAW ("Got heart-beat signal\n");
    Modes.stat.RAW_good++;
    mg_iobuf_del (msg, 0, msg->len);
    return (true);
  }

  /* Remove spaces on the left and on the right.
   */
  while (len && isspace(hex[len-1]))
  {
    hex [len-1] = '\0';
    len--;
  }
  while (isspace(*hex))
  {
    hex++;
    len--;
  }

  /* Check it's format.
   */
  if (len < 2)
  {
    Modes.stat.RAW_empty++;
    Modes.stat.RAW_unrecognized++;
    LOG_BOGUS_RAW (1, "'%.*s'\n", (int)msg->len, msg->buf);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  if (hex[0] != '*' || !memchr(msg->buf, ';', len))
  {
    Modes.stat.RAW_unrecognized++;
    LOG_BOGUS_RAW (2, "hex[0]: '%c', '%.*s'\n", hex[0], (int)msg->len, msg->buf);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  /* Turn the message into binary.
   */
  hex++;     /* Skip `*` and `;` */
  len -= 2;

  if (len > 2 * MODES_LONG_MSG_BYTES)   /* Too long message (> 28 bytes)... broken. */
  {
    Modes.stat.RAW_unrecognized++;
    LOG_BOGUS_RAW (3, "len=%d, '%.*s'\n", len, len, hex);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  for (j = 0; j < len; j += 2)
  {
    int high = hex_digit_val (hex[j]);
    int low  = hex_digit_val (hex[j+1]);

    if (high == -1 || low == -1)
    {
      Modes.stat.RAW_unrecognized++;
      LOG_BOGUS_RAW (4, "high='%c', low='%c'\n", hex[j], hex[j+1]);
      mg_iobuf_del (msg, 0, end - msg->buf);
      return (false);
    }
    bin_msg [j/2] = (high << 4) | low;
  }

  LOG_GOOD_RAW ("'%.*s'\n", len, hex);
  Modes.stat.RAW_good++;

  mg_iobuf_del (msg, 0, end - msg->buf);

  decode_mode_S_message (&mm, bin_msg);
  if (mm.CRC_ok)
     modeS_user_message (&mm);
  return (true);
}

/**
 * The decoder for SBS input of `MSG,x` messages.
 * Details at: http://woodair.net/sbs/article/barebones42_socket_data.htm
 */
static bool SBS_recv_input (char *msg)
{
  modeS_message mm;
  char  *fields [23]; /* leave 0 indexed entry empty, place 22 tokens into array */
  char  *p = msg;
  size_t i;

  fields [0] = "?";
  fields [1] = "MSG";

  /* E.g.:
   *   MSG,5,111,11111,45D068,111111,2024/03/16,18:53:45.000,2024/03/16,18:53:45.000,,7125,,,,,,,,,,0
   *   ^
   *   |__ at index 1 (not 0)
   */
  for (i = 2; i < DIM(fields); i++)
  {
    fields [i] = str_sep (&p, ",");
    if (!p && i < DIM(fields) - 1)
    {
      TRACE ("Missing field %zd\n", i);
      return (false);
    }
  }

  int rc = SBS_decode_msg (fields + 1, &mm);
  if (rc == 0)
       modeS_user_message (&mm);
  else TRACE ("field-error %d\n", rc);
  return (true);
}

/**
 * This is a `net_msg_handler` function similar to `raw_decode_message()`.
 *
 * It is called from `net_io.c` on a `MG_EV_READ` event from Mongoose.
 * Potentially multiple times until all lines in the event-chunk gets
 * consumes; `loop_cnt` is at-least 0.
 *
 * This function shall decode a string representing a Mode S message
 * in SBS format (Base Station) like:
 * ```
 * MSG,5,1,1,4CC52B,1,2021/09/20,23:30:43.897,2021/09/20,23:30:43.901,,38000,,,,,,,0,,0,
 * ```
 *
 * It accepts both `\n` and `\r\n` terminated records.
 * It checks for all 6 valid Message Types, but it handles only `"MSG"` records.
 *
 * \todo Move the handling of SBS-IN data to the `data_thread_fn()`.
 *       Add a `struct mg_queue *sbs_in_data` to `Modes.sbs_in::fn_data`?
 */
bool sbs_decode_message (mg_iobuf *msg, int loop_cnt)
{
  uint8_t *end;

  if (Modes.exit)
  {
    mg_iobuf_del (msg, 0, msg->len);  /* Quit `net_connection_recv()` loop ASAP */
    return (false);
  }

  if (Modes.debug & DEBUG_NET2)
  {
    /* with '--debug N', the trace will look like this
     *   net_io.c(938): MG_EV_READ: 703 bytes from 127.0.0.1:30003 (service "SBS TCP input")
     *   0000   4d 53 47 2c 35 2c 31 31 31 2c 31 31 31 31 31 2c   MSG,5,111,11111,
     *   0010   33 43 36 35 38 36 2c 31 31 31 31 31 31 2c 32 30   3C6586,111111,20
     *   0020   32 34 2f 30 33 2f 31 36 2c 30 36 3a 31 35 3a 33   24/03/16,06:15:3
     *   0030   39 2e 30 30 30 2c 32 30 32 34 2f 30 33 2f 31 36   9.000,2024/03/16
     *   0040   2c 30 36 3a 31 35 3a 33 39 2e 30 30 30 2c 2c 34   ,06:15:39.000,,4
     *   0050   31 37 35 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 30 0d 0a   175,,,,,,,,,,0..
     *                                                    ^_ end of this line
     *   0060   4d 53 47 2c 36 2c 31 31 31 2c 31 31 31 31 31 2c   MSG,6,111,11111,
     */
    mg_hexdump (msg->buf, msg->len);
  }

  if (msg->len == 0)  /* all was consumed */
     return (false);

  end = memchr (msg->buf, '\n', msg->len);
  if (!end)
     return (true);   /* The end-of-line could come in next message */

  *end++ = '\0';
  if ((end - 2 > msg->buf) && end [-2] == '\r')
     end [-2] = '\0';

  switch (SBS_message_type(msg))
  {
    case SBS_UNKNOWN:
         Modes.stat.SBS_unrecognized++;
         LOG_BOGUS_SBS ("'%.*s'\n", (int)(end - msg->buf), msg->buf);
         mg_iobuf_del (msg, 0, msg->len);  /* recover by deleting the complete msg */
         return (false);

    case SBS_MSG:
         Modes.stat.SBS_MSG_msg++;
         LOG_GOOD_SBS ("'%.*s'\n", (int)(end - msg->buf), msg->buf);
         if (SBS_recv_input((char*)msg->buf))
              Modes.stat.SBS_good++;
         else Modes.stat.SBS_unrecognized++;
         break;

    case SBS_AIR:
         Modes.stat.SBS_AIR_msg++;
         Modes.stat.SBS_good++;
//       LOG_GOOD_SBS ("'%.*s'\n", (int)(end - msg->buf), msg->buf);
         break;

    case SBS_STA:
         Modes.stat.SBS_STA_msg++;
         Modes.stat.SBS_good++;
//       LOG_GOOD_SBS ("'%.*s'\n", (int)(end - msg->buf), msg->buf);
         break;

    case SBS_ID:
         Modes.stat.SBS_ID_msg++;
         Modes.stat.SBS_good++;
         break;

    case SBS_SEL:
         Modes.stat.SBS_SEL_msg++;
         Modes.stat.SBS_good++;
         break;

    case SBS_CLK:
         Modes.stat.SBS_CLK_msg++;
         Modes.stat.SBS_good++;
         break;
  }
  mg_iobuf_del (msg, 0, end - msg->buf);
  return (true);
}

/**
 * Write SBS output to TCP clients (Base Station format).
 */
void sbs_out_send (const modeS_message *mm)
{
  char  msg [MODES_MAX_SBS_SIZE], *p = msg;
  int   emergency = 0, ground = 0, alert = 0, spi = 0;
  char  timestamp [60];
  const char *date_str;

  if (mm->msg_type == 4 || mm->msg_type == 5 || mm->msg_type == 21)
  {
    /**\note
     * identity is calculated/kept in base10 but is actually
     * octal (07500 is represented as 7500)
     */
    if (mm->identity == 7500 || mm->identity == 7600 || mm->identity == 7700)
       emergency = -1;
    if (mm->flight_status == 1 || mm->flight_status == 3)
       ground = -1;
    if (mm->flight_status == 2 || mm->flight_status == 3 || mm->flight_status == 4)
       alert = -1;
    if (mm->flight_status == 4 || mm->flight_status == 5)
       spi = -1;
  }

  /* Field 11 could contain the call-sign we can get from `aircraft_find_or_create()::reg_num`.
   *
   * Need to output the current date and time into the SBS output
   *          1   2 3 4 5      6 7          8            9          10           11 12   13 14  15       16       17 18 19 20 21 22
   * example: MSG,3,1,1,4CA7B6,1,2023/10/20,22:33:49.364,2023/10/20,22:33:49.403,  ,7250,  ,   ,53.26917,-2.17755,  ,  ,  ,  ,  ,0
   */
  date_str = SBS_set_timestamp (timestamp);

  if (mm->msg_type == 0)
  {
    p += sprintf (p, "MSG,5,1,1,%06X,1,%s,,%d,,,,,,,,,,",
                  mm->addr, date_str, mm->altitude);
  }
  else if (mm->msg_type == 4)
  {
    p += sprintf (p, "MSG,5,1,1,%06X,1,%s,,%d,,,,,,,%d,%d,%d,%d",
                  mm->addr, date_str, mm->altitude, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 5)
  {
    p += sprintf (p, "MSG,6,1,1,%06X,1,%s,,,,,,,,%04X,%d,%d,%d,%d",
                  mm->addr, date_str, mm->identity, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 11)
  {
    p += sprintf (p, "MSG,8,1,1,%06X,1,%s,,,,,,,,,,,,%d",
                  mm->addr, date_str, ground);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 4)
  {
    p += sprintf (p, "MSG,1,1,1,%06X,1,%s,%s,,,,,,,,,,,",
                  mm->addr, date_str, mm->flight);
  }
  else if (mm->msg_type == 17 && mm->ME_type >= 9 && mm->ME_type <= 18)
  {
    if (mm->AC_flags & MODES_ACFLAGS_LATLON_VALID)
         p += sprintf (p, "MSG,3,1,1,%06X,1,%s,,%d,,,%1.5f,%1.5f,,,%d,%d,%d,%d",
                       mm->addr, date_str, mm->altitude, mm->position.lat, mm->position.lon, alert, emergency, spi, ground);
    else p += sprintf (p, "MSG,3,1,1,%06X,1,%s,,%d,,,,,,,%d,%d,%d,%d",
                       mm->addr, date_str, mm->altitude, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 19 && mm->ME_subtype == 1)
  {
    int vr = (mm->vert_rate_sign == 0 ? 1 : -1) * mm->vert_rate;

    p += sprintf (p, "MSG,4,1,1,%06X,1,%s,,,%d,%d,,,%i,,,,,",
                  mm->addr, date_str, (int)round(mm->velocity), (int)round(mm->heading), vr);
  }
  else if (mm->msg_type == 21)
  {
    p += sprintf (p, "MSG,6,1,1,%06X,1,%s,,,,,,,,%04X,%d,%d,%d,%d",
                  mm->addr, date_str, mm->identity, alert, emergency, spi, ground);
  }
  else
    return;

  *p++ = '\n';
  net_handler_send (MODES_NET_SERVICE_SBS_OUT, msg, p - msg);
}

/**
 * Decode `fields[1..22]` and fill `*mm`.
 *
 * This is rather incomplete at the moment.
 *
 * \todo
 *  Use `decodeSbsLine()` from readsb's `net_io.c` as guidance.
 *
 * \param in  fields  array of field to be parsed.
 * \param out mm      the modeS-message.
 * \retval 0  on success.
 * \retval N  failure in field N.
 */
static int SBS_decode_msg (const char *fields[], modeS_message *mm)
{
  const char *p;
  char  *end;
  bool   got_lat, got_lon;
  int    i_val;
  double f_val;

  memset (mm, '\0', sizeof(*mm));

  /*
   * Ignore these:
   *   fields [3] == Session ID -- Database Session record number.
   *   fields [4] == AircraftID -- Database Aircraft record number.
   *   fields [6] == FlightID   -- Database Flight record number.
   *
   * Tested with JetVision's 'rtl1090.beta3.exe' where these are
   * always "111", "1111" and "11111".
   */

  p = fields [2];    /* MSG sub types 1 to 8 */
  i_val = strtod (p, &end);
  if (strlen(p) != 1 || end == p || *end != '\0' || i_val < 1 || i_val > 8)
     return (2);

  mm->msg_type = i_val;

  p = fields [5];
  if (strlen(p) != 6)   /* HexIdent -- Aircraft Mode S hexadecimal code */
     return (5);

  if (*p == '~')
       mm->addr = mg_unhexn (p+1, 6) | MODES_NON_ICAO_ADDRESS;
  else mm->addr = mg_unhexn (p, 6);

  p = fields [10];
  if (*p)
     mm->sys_timestamp_msg = SBS_get_timestamp (p);

  p = fields [11];      /* Callsign -- An eight digit flight ID */
  if (*p)
  {
    strncpy (mm->flight, p, sizeof(mm->flight));
    mm->AC_flags |= MODES_ACFLAGS_CALLSIGN_VALID;
  }

  p = fields [12];
  if (*p)
  {
    i_val = strtod (p, &end);
    if (end == p || *end != '\0')
       return (12);

    mm->altitude = i_val;
    mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;
  }

  p = fields [13];
  if (*p)
  {
    f_val = strtof (p, &end);
    if (end == p || *end != '\0')
       return (13);
    mm->velocity = f_val;
    mm->AC_flags |= MODES_ACFLAGS_SPEED_VALID;
  }

  p = fields [14];      /* Track -- Track of aircraft (not heading). Derived from the velocity E/W and velocity N/S */
  if (*p)
  {
    f_val = strtof (p, &end);
    if (end == p || *end != '\0')
       return (14);

    mm->heading = f_val;
    mm->AC_flags |= MODES_ACFLAGS_HEADING_VALID;
  }

  got_lat = false;
  p = fields [15];
  if (*p)
  {
    f_val = strtof (p, &end);
    if (end == p || *end != '\0')
       return (15);

    mm->position.lat = f_val;
//  mm->raw_latitude = mm->position.lat;
    got_lat = true;
  }

  got_lon = false;
  p = fields [16];
  if (*p)
  {
    f_val = strtof (p, &end);
    if (end == p || *end != '\0')
       return (16);
    mm->position.lon = f_val;
//  mm->raw_longitude = mm->position.lon;
    got_lon = true;
  }

  if (got_lat && got_lon)
     mm->AC_flags |= MODES_ACFLAGS_LATLON_VALID;

  p = fields [17];
  if (*p)
  {
    i_val = strtod (p, &end);
    if (end == p || *end != '\0')
       return (17);

    if (i_val < 0)
       mm->vert_rate_sign = -1;
    mm->vert_rate = abs (i_val);
    mm->AC_flags |= MODES_ACFLAGS_VERTRATE_VALID;
  }

  p = fields [18];
  if (*p)
  {
    mm->identity = mg_unhex (p);
    mm->AC_flags |= MODES_ACFLAGS_SQUAWK_VALID;
  }

#if 0  // \todo
  mm->msg_bits = ...;
  snprintf (p, left, "%02x", mm->msg[i])

  mm->CRC = crc_checksum (msg, mm->msg_bits);
#endif

  mm->msg_bits = 0;
  mm->source   = SOURCE_MODE_S;
  mm->CRC_ok   = mm->SBS_in = true;
  return (0);
}

/**
 * Return a date-time for the SBS output.
 */
static const char *SBS_set_timestamp (char *ts)
{
  char       ts_buf [30];
  FILETIME   ft;
  SYSTEMTIME st;

  get_FILETIME_now (&ft);
  FileTimeToSystemTime (&ft, &st);
  snprintf (ts_buf, sizeof(ts_buf), "%04u/%02u/%02u,%02u:%02u:%02u.%03u,",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

  /* Since the date,time,date,time is just a repeat, build the whole string once and
   * then add it to each MSG output.
   */
  strcpy (ts, ts_buf);
  strcat (ts, ts_buf);

  ts [strlen(ts) - 1] = '\0';    /* remove last ',' */
  return (ts);
}

/*
 * Parse time spec in `*buf` according to `*format` and return a `struct tm *tmp`.
 */
static char *strptime (const char *buf, const char *format, struct tm *tm)
{
  MODES_NOTUSED (buf);
  MODES_NOTUSED (format);
  MODES_NOTUSED (tm);
  return (NULL);
}

/**
 * Parse a date-time from the SBS input.
 * The inverse of `SBS_set_timestamp()`.
 */
static uint64_t SBS_get_timestamp (const char *ts)
{
  struct tm   tm;
  const char *t;

  memset (&tm, '\0', sizeof(tm));
  t = strptime (ts, "%Y/%m/%d %H:%M:%S", &tm);   /* ignore milli-sec in timestamp; it is always ".000" */
  if (t)
     return (1000LL * timegm (&tm));   /* to milli-sec */
  return MSEC_TIME();                  /* failed; use now */
}

/**
 * Validate and return tge SBS-message type in `*msg`.
 */
static SBS_msg_t SBS_message_type (const mg_iobuf *msg)
{
  const char *m = (const char*) msg->buf;
  const char *p = strchr (m, ',');
  char  name [10];
  DWORD type;

  if (msg->len < 4 || !p || p - m >= sizeof(name))
     return (SBS_UNKNOWN);

  strncpy (name, m, p - m + 1);
  name [p - m + 1] = '\0';
  type = search_list_value (name, SBS_types, DIM(SBS_types));
  if (type == (DWORD)-1)
     return (SBS_UNKNOWN);
  return (SBS_msg_t) type;
}


