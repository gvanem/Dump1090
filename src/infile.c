/**
 * \file    infile.c
 * \ingroup SDR input functions
 * \brief   Read binary data or CSV records
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "demod.h"
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
    _setmode (Modes.infile_fd, O_BINARY);
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
 * This function currently runs in the main-thread.
 * \todo call it from the `data_thread_fn()` instead
 */
static int bin_read (void)
{
  uint64_t bytes_read = 0;
  bool     eof = false;
  size_t   readbuf_sz = MODES_MAG_BUF_SAMPLES * Modes.bytes_per_sample;
  void    *readbuf = calloc (readbuf_sz, 1);

  if (!readbuf)
  {
    LOG_STDERR ("failed to allocate read buffer\n");
    return (0);
  }

  while (!Modes.exit && !eof)
  {
    int      nread = 0;
    int      toread = 0;
    char    *rdata;
    uint32_t samples_read;
    struct   mag_buf *out_buf;

    TRACE ("%s(): Modes.sample_counter: %llu\n", __FUNCTION__, Modes.sample_counter);

    out_buf = fifo_acquire (100);
    if (!out_buf)
       continue;   /* no space for output , maybe we halted */

    out_buf->sample_timestamp = (Modes.sample_counter * 12E6) / Modes.sample_rate;
    out_buf->sys_timestamp    = MSEC_TIME();

    toread = readbuf_sz;

    rdata = readbuf;
    while (toread)
    {
      nread = _read (Modes.infile_fd, rdata, toread);
      if (nread <= 0)
      {
        eof = true;
        break;
      }
      rdata      += nread;
      bytes_read += nread;
      toread     -= nread;
      TRACE ("  nread: %d, bytes_read: %llu, eof: %d\n", nread, bytes_read, eof);
    }

    samples_read = nread / Modes.bytes_per_sample;

    /* Convert the new data
     */
    (*Modes.converter_func) (readbuf, &out_buf->data [Modes.trailing_samples],
                             samples_read, Modes.converter_state, &out_buf->mean_power);

    out_buf->valid_length = out_buf->overlap + samples_read;
    out_buf->flags = 0;

    Modes.sample_counter += samples_read;

    /* Push the new data to the FIFO.
     * And dequeue it immediately. There should be something to process
     */
    fifo_enqueue (out_buf);
    out_buf = fifo_dequeue (100);
    if (out_buf)
    {
      (*Modes.demod_func) (out_buf);
      fifo_release (out_buf);        /* Return the buffer to the FIFO freelist for reuse */
    }

    /* seek the file again from the start
     * and re-play it if --loops was given.
     */
    if (Modes.loops > 0)
    {
      Modes.loops--;
      eof = false;
      if (_lseek(Modes.infile_fd, 0, SEEK_SET) == -1)
         Modes.exit = true;
    }
  }
  free (readbuf);
  fifo_halt();
  return ((int) Modes.sample_counter);
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
    _setmode (STDIN_FILENO, O_TEXT);
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

bool informat_set (const char *arg)
{
  convert_format f = INPUT_ILLEGAL;

  if (!strcmp(arg, "uc8"))
     f = INPUT_UC8;
  else if (!strcmp(arg, "sc16"))
     f = INPUT_SC16;
  else if (!strcmp(arg, "sc16q11"))
     f = INPUT_SC16Q11;

  if (f != INPUT_ILLEGAL)
  {
    Modes.input_format = f;
    return (true);
  }
  return (false);
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
  TRACE ("Parsed %u records in %.3f msec from: \"%s\"\n",
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
      TRACE ("  msg: %3d, rc: %d, Modes.stat.RAW_good: %llu\n",
             num, rc, Modes.stat.RAW_good);

      num++;
      rec++;

      if (rc)
         ret++;

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
