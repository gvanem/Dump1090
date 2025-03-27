/**
 * \file    infile.c
 * \ingroup Input
 * \brief   Read binary data or CSV records
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demod-2000.h"
#include "misc.h"
#include "infile.h"

/**
 * Support for reading a ready-made 2 column CSV files with
 * lines like this:
 * \code
 *   1698140962.119813, 1a33000023d2653d24903907dbc1c50fca1ad77f538d33
 *                                      ^
 *                                      |__ save from ofs 18
 * \endcode
 *
 * column 1 == timestamp of recorded message
 * column 2 == the raw SBS message without the '*' start and ';' termination
 */
#define CSV_RAW_OFS       18           /**< The SBS-data starts at ofs 18 */
#define CSV_REC_INCREMENT (1024*1024)  /**< Increment for `realloc()` */

/**
 * \typedef csv_record
 */
typedef struct csv_record {
        double        timestamp;       /**< Timestamp of recorded message. With fractional usec */
        double        delta_us;        /**< Micro-sec since 1st message; `g_data.reference_time` */
        unsigned char raw_msg [32];    /**< Raw message stored as `*249 ... 33;\n` */
      } csv_record;

typedef struct csv_globals {
        CSV_context  ctx;
        csv_record  *records;
        size_t       rec_allocated;
        uint32_t     num_records;
        double       reference_time;
      } csv_globals;

static csv_globals g_data;

static bool csv_parse_file (void);
static int  csv_read (void);

/**
 * Open and initialize the file set in `infile_set()` below
 */
bool infile_init (void)
{
  const char *file = Modes.infile;
  bool        rc = true;

  assert (file[0]);

  if (g_data.ctx.file_name)
  {
    Modes.infile_fd = 0;   /* fake it for `any_device` in dump1090.c */
    return csv_parse_file();
  }

  if (file[0] == '-' && file[1] == '\0')
  {
    Modes.infile_fd = STDIN_FILENO;
    SETMODE (Modes.infile_fd, O_BINARY);
    return (true);
  }

  Modes.infile_fd = _open (file, O_RDONLY | O_BINARY);
  if (Modes.infile_fd == -1)
  {
    LOG_STDERR ("Error opening `%s`: %s\n", file, strerror(errno));
    rc = false;
  }
  return (rc);
}

/**
 * This is used when `--infile` is specified in order to read data
 * from a binary file instead of using a RTLSDR / SDRplay device.
 */
static int bin_read (void)
{
  uint32_t rc = 0;

  if (Modes.loops > 0 && Modes.infile_fd == STDIN_FILENO)
  {
    LOG_STDERR ("Option `--loops <N>' not supported for `stdin'.\n");
    Modes.loops = 0;
  }

  do
  {
     int      nread, toread;
     uint8_t *data;

     if (Modes.interactive)
     {
       /* When --infile and --interactive are used together, slow down
        * mimicking the real RTLSDR / SDRplay rate.
        */
       Sleep (1000);
     }

     /* Move the last part of the previous buffer, that was not processed,
      * on the start of the new buffer.
      */
     memcpy (Modes.data, Modes.data + MODES_ASYNC_BUF_SIZE, 4*(MODES_FULL_LEN-1));
     toread = MODES_ASYNC_BUF_SIZE;
     data   = Modes.data + 4*(MODES_FULL_LEN-1);

     while (toread)
     {
       nread = _read (Modes.infile_fd, data, toread);
       if (nread <= 0)
          break;
       data   += nread;
       toread -= nread;
     }

     if (toread)
     {
       /* Not enough data on file to fill the buffer? Pad with
        * no signal.
        */
       memset (data, 127, toread);
     }

     compute_magnitude_vector (Modes.data);
     rc += demodulate_2000 (Modes.magnitude, Modes.data_len/2);
     background_tasks();

     if (Modes.exit || Modes.infile_fd == STDIN_FILENO)
        break;

     /* seek the file again from the start
      * and re-play it if --loops was given.
      */
     if (Modes.loops > 0)
        Modes.loops--;
     if (Modes.loops == 0 || _lseek(Modes.infile_fd, 0, SEEK_SET) == -1)
        break;
  }
  while (1);
  return (rc);
}

/**
 * Process the `--infile file`.
 * A binary of CSV-file.
 */
int infile_read (void)
{
  if (g_data.ctx.file_name)
     return csv_read();
  return bin_read();
}

/**
 * Free memory and close the `--infile file` handle.
 */
void infile_exit (void)
{
  if (g_data.records)
  {
    free (g_data.records);
    g_data.records = NULL;
    Modes.infile_fd = -1;
  }
  else if (Modes.infile_fd == STDIN_FILENO)
  {
    SETMODE (STDIN_FILENO, O_TEXT);
    return;
  }
  if (Modes.infile_fd > -1)
  {
    _close (Modes.infile_fd);
    Modes.infile_fd = -1;
  }
}

bool infile_set (const char *arg)
{
  mg_file_path copy;

  strcpy_s (Modes.infile, sizeof(Modes.infile), arg);
  strcpy_s (copy, sizeof(copy), Modes.infile);
  strlwr (copy);

  if (str_endswith(copy, ".csv"))
     g_data.ctx.file_name = Modes.infile;
  return (true);
}

static bool csv_add_record (double timestamp, const char *msg, double delta_us)
{
  csv_record    *rec, *more;
  size_t         len, ofs = CSV_RAW_OFS;
  unsigned char *p;

  if (g_data.num_records + sizeof(*rec) >= g_data.rec_allocated)
  {
    g_data.rec_allocated += sizeof(*rec) * CSV_REC_INCREMENT;
    more = realloc (g_data.records, g_data.rec_allocated);
    if (!more)
       return (false);

    g_data.records = more;
  }

  rec = g_data.records + g_data.num_records++;
  rec->timestamp = timestamp;
  rec->delta_us  = delta_us;

  len = strlen (msg + ofs);
  assert (len < sizeof(rec->raw_msg) - 3);
  memcpy ((char*)rec->raw_msg + 1, msg + ofs, len);

  rec->raw_msg [0] = '*';
  p = rec->raw_msg + len + 1;
  *p++ = ';';
  *p++ = '\n';
  *p   = '\0';
  return (true);
}

/*
 * Parsing of a .CSV-file used as `--infile`.
 * Handy for testing the `decode_RAW_message()` and higher level functions.
 */
static int csv_callback (struct CSV_context *ctx, const char *value)
{
  static double timestamp = 0.0;
  static double delta_us  = 0.0;
  int    rc = 0;

  if (Modes.exit)
     return (0);

  if (ctx->field_num == 0)
  {
    timestamp = atof (value);
    if (g_data.reference_time == 0.0)
         g_data.reference_time = timestamp;
    else delta_us = timestamp - g_data.reference_time;

    rc = ((time_t)timestamp > 0);
  }
  else if (ctx->field_num == 1)
  {
    if (csv_add_record(timestamp, value, delta_us))
       rc = 1;
    timestamp = 0.0;   /* ready for next record */
  }
  return (rc);
}

static bool csv_parse_file (void)
{
  double start_t = get_usec_now();

  g_data.ctx.delimiter  = ',';
  g_data.ctx.callback   = csv_callback;
  g_data.ctx.num_fields = 2;
  g_data.ctx.rec_max    = Modes.max_messages;
  g_data.ctx.line_size  = 0;

  printf ("Parsing '%s' ...", g_data.ctx.file_name);
  fflush (stdout);

  if (!CSV_open_and_parse_file(&g_data.ctx))
  {
    printf ("Parsing failed: %s\n", strerror(errno));
    return (false);
  }

  puts ("");
  TRACE ("Parsed %u records in %.3f msec from: \"%s\"",
         g_data.ctx.rec_num, (get_usec_now() - start_t) / 1E3, g_data.ctx.file_name);
  return (true);
}

/*
 * For testing a limited (e.g. option `--max-messages 1000`)
 * or the full data-set.
 */
static void csv_read_test (void)
{
  const csv_record *rec = g_data.records;
  time_t ref, now = time (NULL);
  int    i;

  assert (rec);

  ref = (time_t) g_data.reference_time;
  printf ("%s():\n"
          "  Dumping '%s'.\n"
          "  Reference time: %.24s\n",
          __FUNCTION__, Modes.infile, ctime(&ref));

  puts ("  TS        fraction  delta-uS  Raw message\n"
        "  --------------------------------------------------------------");

  for (i = 0; rec && i < (int)g_data.num_records; rec++, i++)
  {
    double fraction = rec->timestamp - (time_t)rec->timestamp;
    char   hms [10];
    time_t ts = (time_t) (rec->timestamp - g_data.reference_time) + now;

    strftime (hms, sizeof(hms), "%H:%M:%S", localtime(&ts));
    printf ("  %s +%.06f  %.06f  %s", hms, fraction, rec->delta_us, rec->raw_msg);
  }

  printf ("  Added %u records\n\n", g_data.num_records);
  assert (i == (int)g_data.num_records);
}

/**
 * This is used when `--infile file.csv` is used
 */
static int csv_read (void)
{
  const csv_record *rec = g_data.records;
  double            elapsed, start_us = get_usec_now();
  uint32_t          num = 0;
  int               ret = 0;

  assert (rec);

  if (Modes.exit)
     return (0);

  if (Modes.debug & DEBUG_GENERAL2)
     csv_read_test();

  do
  {
    mg_iobuf msg;
    int      rc;

    background_tasks();

    /* When to fire off the next raw message?
     */
    elapsed = (get_usec_now() - start_us) / 1E6;
    if (elapsed >= rec->delta_us)
    {
      msg.buf = (unsigned char*) rec->raw_msg;
      msg.len = strlen ((const char*)rec->raw_msg);

      rc = (int) decode_RAW_message (&msg, 0);
      TRACE ("  msg: %3d, rc: %d, Modes.stat.RAW_good: %llu",
             num, rc, Modes.stat.RAW_good);

      num++;
      rec++;

      if (rc)
      {
        Modes.stat.good_CRC++;
        ret++;
      }

      if (Modes.max_messages > 0 && --Modes.max_messages == 0)
      {
        LOG_STDOUT ("'Modes.max_messages' reached 0.\n");
        Modes.exit = true;
      }
      if (num >= g_data.num_records)
      {
        LOG_STDOUT ("No more CSV records.\n");
        Modes.exit = true;
      }
    }
  }
  while (!Modes.exit);

  return (ret);
}
