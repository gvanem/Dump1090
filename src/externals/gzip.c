/*
 * Scraped from:
 *   https://github.com/wkoszek/mini_gzip/blob/master/mini_gzip.c
 *   https://github.com/wkoszek/mini_gzip/blob/master/mini_gzip_testprog.c (`mini_gz_decompress()` function)
 *
 * and simplified; no compression.
 * Only decompression as needed in `aircraft.c`.
 *
 * BSD 2-clause license
 * Copyright (c) 2013 Wojciech A. Koszek <wkoszek@FreeBSD.org>
 *
 * Based on:
 *
 * https://github.com/strake/gzip.git
 */

/**\file    gzip.c
 * \ingroup Misc
 * \brief   GZIP decompression
 */

#ifdef MINIZ_NO_INFLATE_APIS
#error "Cannot define 'MINIZ_NO_INFLATE_APIS'"
#endif

#ifdef MINIZ_NO_ZLIB_APIS
#error "Cannot define 'MINIZ_NO_ZLIB_APIS'"
#endif

/* To avoid duplicate symbols from 'externals/zip.c' and here.
 */
#define MINIZ_HEADER_FILE_ONLY 1

#include "misc.h"
#include "miniz.h"
#include "gzip.h"

#define ERROR_START  (MZ_PARAM_ERROR - 1)
#define CHUNK_SIZE   (10*1024)

typedef struct gzip {
        size_t    data_len;
        char     *fname_ptr;
        uint8_t  *data_ptr;
        mz_ulong  out_len;
      } gzip;

typedef enum gzip_err {
        GZ_OK = 0,
        GZ_UNKNOWN     = ERROR_START,
        GZ_INVALID_HDR = ERROR_START - 1
      } gzip_err;

/**
 * Check the GZIP header read in at `mem`.
 * \retval  GZ_OK (0) if okay
 * \retval  < 0       on error
 *
 * \ref https://en.wikipedia.org/wiki/Gzip
 */
static int gzip_init (gzip *gz, void *mem, size_t mem_len)
{
  char *mem8_ptr = (char*) mem;
  char *hptr     = mem8_ptr + 0;    /* .gz header */
  char *hauxptr  = mem8_ptr + 10;   /* auxillary header */

  memset (gz, '\0', sizeof(*gz));

  if (hptr[0] != 0x1F || hptr[1] != (char)0x8B)  /* wrong signature */
  {
    TRACE ("Invalid GZIP header: %04X\n", (hptr[1] << 8) + hptr[0]);
    return (GZ_INVALID_HDR);
  }

  if (hptr[2] != 8)       /* not Deflate algorithm */
  {
    TRACE ("Unknown algorithm: %02X\n", hptr[2]);
    return (GZ_INVALID_HDR);
  }

  if (hptr[3] & 0x04)     /* FEXTRA bit */
     hauxptr += 2;

  if (hptr[3] & 0x08)     /* FNAME bit */
  {
    gz->fname_ptr = (char*) hauxptr;
    while (*hauxptr)
       hauxptr++;
    hauxptr++;
  }

  if (hptr[3] & 0x10)     /* FHCOMMENT bit */
  {
    while (*hauxptr)
       hauxptr++;
    hauxptr++;
  }

  if (hptr[3] & 0x2)     /* FHCRC bit */
     hauxptr += 2;

  gz->data_ptr = (uint8_t*) hauxptr;
  gz->data_len = mem_len - (hauxptr - hptr);
  return (GZ_OK);
}

static int gzip_deflate (gzip *gz, void *mem_out, size_t mem_out_len)
{
  z_stream zs;
  mz_ulong ret;
  int      rc;

  memset (&zs, '\0', sizeof(zs));

  /* negative for raw deflate
   */
  rc = mz_inflateInit2 (&zs, -MZ_DEFAULT_WINDOW_BITS);
  if (rc != MZ_OK)
     return (rc);

  zs.avail_out = mem_out_len;
  zs.next_in   = gz->data_ptr;
  zs.next_out  = mem_out;

  while (1)
  {
    size_t bytes_to_read = min (CHUNK_SIZE, gz->data_len);
    int    rc;

    zs.avail_in += bytes_to_read;
    rc = mz_inflate (&zs, MZ_SYNC_FLUSH);
    gz->data_len -= bytes_to_read;

    if (zs.avail_out == 0 && gz->data_len != 0)
    {
      TRACE ("MZ_DATA_ERROR\n");
      return (MZ_DATA_ERROR);
    }
    if (rc > MZ_STREAM_END || rc < 0)
    {
      TRACE ("rc: %d\n", rc);
      return (rc);
    }
    if (rc == MZ_STREAM_END)
    {
      TRACE ("MZ_STREAM_END\n");
      break;
    }
  }
  mz_inflateEnd (&zs);
  gz->out_len = zs.total_out;
  return (GZ_OK);
}

/**
 * The only public function here.
 *
 * Similar to `zip_extract()`;
 * Extracts a gzip archive file into directory.
 *
 * If extract_cb is not NULL, the callback will be called after
 * successfully extracting the whole gzip file.
 *
 * @param gzip_name   gzip archive file.
 * @param dir         output directory.
 * @param extract_cb  extract callback.
 * @param cb_arg      callback argument.
 *
 * @return the return code - 0 on success, negative number (< 0) on error.
 */
int gzip_extract (const char *gzip_name, const char *dir,
                  gzip_cb extract_cb, void *cb_arg)
{
  mz_ulong  in_size;
  mz_ulong  out_size;
  mz_ulong  written;
  gzip_err  ret = GZ_UNKNOWN;
  gzip      gz;
  char     *mem_in  = NULL;
  char     *mem_out = NULL;
  char      out_filename [MAX_PATH];
  FILE     *out_file = NULL;
  FILE     *in_file  = fopen (gzip_name, "rb");

  if (!in_file)
     goto quit;

  in_size = filelength (fileno(in_file));

  /* Assume decompressed file will be max 6 times bigger
   */
  out_size = 6 * in_size;
  mem_in  = malloc (in_size);
  mem_out = malloc (out_size);
  if (!mem_in || !mem_out)
     goto quit;

  if (fread(mem_in, 1, in_size, in_file) != in_size)
     goto quit;

  if (gzip_init(&gz, mem_in, in_size) < 0)
  {
    TRACE ("gzip_init() failed\n");
    goto quit;
  }

  if (gzip_deflate(&gz, mem_out, out_size) < 0 || gz.out_len == 0)
  {
    TRACE ("gzip_deflate() failed\n");
    goto quit;
  }

  TRACE ("gz.out_len: %lu, out_size: %lu, in_size: %lu, gz.fname_ptr: '%s'\n",
         gz.out_len, out_size, in_size, gz.fname_ptr);

  snprintf (out_filename, sizeof(out_filename), "%s\\%s",
            dir, gz.fname_ptr ? gz.fname_ptr : "gzip.tmp");

  TRACE ("Decompressing '%s' to '%s'\n", gzip_name, out_filename);

  out_file = fopen (out_filename, "w+b");
  if (!out_file)
     goto quit;

  written = fwrite (mem_out, 1, gz.out_len, out_file);
  if (written != gz.out_len)
  {
    TRACE ("fwrite() wrote only %lu bytes\n", written);
    goto quit;
  }
  ret = GZ_OK;

quit:
  FREE (mem_in);
  FREE (mem_out);
  FCLOSE (in_file);
  FCLOSE (out_file);
  TRACE ("ret = %d\n", ret);

  if (ret == GZ_OK && extract_cb)
    (*extract_cb) (out_filename, cb_arg);
  return (ret);
}

