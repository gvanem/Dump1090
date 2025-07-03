/*
 * Part of dump1090, a Mode S message decoder for RTLSDR devices.
 *
 * Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
 */

/**\file    crc.c
 * \ingroup Misc
 * \brief   Mode S CRC calculation and error correction.
 */
#include "misc.h"
#include "crc.h"

/* Errorinfo for "no errors"
 */
static errorinfo NO_ERRORS = { 0,0, { 0,0 }};

/* Generator polynomial for the Mode S CRC:
 */
#define MODES_GENERATOR_POLY 0xFFF409U

/*
 * CRC values for all single-byte messages;
 * used to speed up CRC calculation.
 */
static uint32_t crc_table [256];

static errorinfo *short_errors,   *long_errors;
static int        short_errors_sz, long_errors_sz;

/*
 * Syndrome values for all single-bit errors;
 * used to speed up construction of error-correction tables.
 */
static uint32_t single_bit_syndrome [MODES_LONG_MSG_BITS];

static void init_tables (void)
{
  size_t  i;
  int     j;
  uint8_t msg [DIM(single_bit_syndrome) / 8];

  for (i = 0; i < DIM(crc_table); i++)
  {
    uint32_t c = i << 16;

    for (j = 0; j < 8; ++j)
    {
      if (c & 0x800000)
           c = (c << 1) ^ MODES_GENERATOR_POLY;
      else c = (c << 1);
    }
    crc_table [i] = c & 0x00FFFFFF;
  }
  memset (msg, '\0', sizeof(msg));
  for (i = 0; i < DIM(single_bit_syndrome); i++)
  {
    msg [i / 8] ^= 1 << (7 - (i & 7));
    single_bit_syndrome [i] = crc_checksum (msg, DIM(single_bit_syndrome));
    msg [i / 8] ^= 1 << (7 - (i & 7));
  }
}

/**
 * Do the CRC check of `msg`.
 */
uint32_t crc_checksum (const uint8_t *msg, int bits)
{
  uint32_t rem = 0;
  int      i, n = bits / 8;

  assert (bits % 8 == 0);
  assert (n >= 3);

  for (i = 0; i < n - 3; i++)
  {
    rem = (rem << 8) ^ crc_table [msg[i] ^ ((rem & 0xFF0000) >> 16)];
    rem &= 0xFFFFFF;
  }
  rem = rem ^ (msg[n-3] << 16) ^ (msg[n-2] << 8) ^ (msg[n-1]);
  return (rem);
}

/**
 * Compare two errorinfo structures
 */
static int syndrome_compare (const void *x, const void *y)
{
  errorinfo *ex = (errorinfo*) x;
  errorinfo *ey = (errorinfo*) y;

  return (int)ex->syndrome - (int)ey->syndrome;
}

/*
 * (n k), the number of ways of selecting k distinct items from a set of n items
 */
static int combinations (int n, int k)
{
  int result = 1, i;

  if (k == 0 || k == n)
     return (1);

  if (k > n)
     return (0);

  for (i = 1; i <= k; ++i)
  {
    result = result * n / i;
    n = n - 1;
  }
  return (result);
}

/**
 * Recursively populates an errorinfo table with error syndromes.
 *
 * \param in,out table      the table to fill
 * \param in     n          first entry to fill
 * \param in     maxSize    max size of table
 * \param in     offset     start bit offset for checksum calculation
 * \param in     startbit   first bit to introduce errors into
 * \param in     endbit     (one past) last bit to introduce errors info
 * \param in     base_entry template entry to start from
 * \param in     error_bit  how many error bits have already been set
 * \param in     max_errors maximum total error bits to set
 * \retval       the next free entry in the table
 *
 * On output, `table` has been populated between `[n, return value)`
 */
static int prepare_sub_table (errorinfo *table, int n, int maxsize, int offset, int startbit, int endbit,
                              errorinfo *base_entry, int error_bit, int max_errors)
{
  int i = 0;

  if (error_bit >= max_errors)
     return (n);

  for (i = startbit; i < endbit; ++i)
  {
    assert (n < maxsize);

    table[n] = *base_entry;
    table[n].syndrome ^= single_bit_syndrome[i + offset];
    table[n].errors = error_bit + 1;
    table[n].bit [error_bit] = i;

    ++n;
    n = prepare_sub_table (table, n, maxsize, offset, i + 1, endbit, &table[n-1], error_bit + 1, max_errors);
  }
  return (n);
}

static int flag_collisions (const errorinfo *table, int tablesize, int offset, int startbit, int endbit,
                            uint32_t base_syndrome, int error_bit, int first_error, int last_error)
{
  int i = 0;
  int count = 0;

  if (error_bit > last_error)
     return (0);

  for (i = startbit; i < endbit; ++i)
  {
    errorinfo ei;

    ei.syndrome = base_syndrome ^ single_bit_syndrome[i + offset];

    if (error_bit >= first_error)
    {
      errorinfo *collision = bsearch(&ei, table, tablesize, sizeof(errorinfo), syndrome_compare);
      if (collision != NULL && collision->errors != -1)
      {
        count++;
        collision->errors = -1;
      }
    }
    count += flag_collisions (table, tablesize, offset, i+1, endbit,
                              ei.syndrome, error_bit + 1, first_error, last_error);
  }
  return (count);
}

/**
 * Allocate and build an error table for messages of length "bits" (max `MODES_LONG_MSG_BITS`).
 * Returns a pointer to the new table and sets `*size_out` to the table length
 */
static errorinfo *prepare_error_table (int bits, int max_correct, int max_detect, int *size_out)
{
  int        i, j, maxsize, usedsize;
  errorinfo *table, base_entry;

  assert (bits >= 0 && bits <= MODES_LONG_MSG_BITS);
  assert (max_correct >=0 && max_correct <= MODES_MAX_BITERRORS);
  assert (max_detect >= max_correct);

  if (!max_correct)
  {
    *size_out = 0;
    return (NULL);
  }

  maxsize = 0;
  for (i = 1; i <= max_correct; ++i)
      maxsize += combinations (bits - 5, i);  /* space needed for all i-bit errors */

  table = malloc (maxsize * sizeof(errorinfo));
  base_entry.syndrome = 0;
  base_entry.errors = 0;
  for (i = 0; i < MODES_MAX_BITERRORS; ++i)
      base_entry.bit[i] = -1;

  /* ignore the first 5 bits (DF type)
   */
  usedsize = prepare_sub_table (table, 0, maxsize, MODES_LONG_MSG_BITS - bits, 5, bits, &base_entry, 0, max_correct);

  qsort (table, usedsize, sizeof(errorinfo), syndrome_compare);

  /* Handle ambiguous cases, where there is more than one possible error pattern
   * that produces a given syndrome (this happens with >2 bit errors).
   */
  for (i = 0, j = 0; i < usedsize; ++i)
  {
    if (i < usedsize-1 && table[i+1].syndrome == table[i].syndrome)
    {
      /* Skip over this entry and all collisions
       */
      while (i < usedsize && table[i+1].syndrome == table[i].syndrome)
          i++;
      continue;  /* now table[i] is the last duplicate */
    }

    if (i != j)
       table[j] = table[i];
    j++;
  }

  if (j < usedsize)
     usedsize = j;

  /* Flag collisions we want to detect but not correct
   */
  if (max_detect > max_correct)
  {
    int flagged = flag_collisions (table, usedsize, MODES_LONG_MSG_BITS - bits, 5, bits, 0, 1, max_correct+1, max_detect);

    if (flagged > 0)
    {
      for (i = 0, j = 0; i < usedsize; ++i)
      {
        if (table[i].errors != -1)
        {
          if (i != j)
             table [j] = table [i];
          j++;
        }
      }
      usedsize = j;
    }
  }
  *size_out = usedsize;
  return (table);
}

void crc_exit (void)
{
  free (short_errors);
  free (long_errors);
}

/**
 * Precompute syndrome tables for 56- and 112-bit messages.
 */
void crc_init (int fix_bits)
{
  init_tables();

  switch (fix_bits)
  {
    case 0:
         short_errors = long_errors = NULL;
         short_errors_sz = long_errors_sz = 0;
         break;

    case 1:
         /* For 1 bit correction, we have 100% coverage up to 4 bit detection, so don't bother
          * with flagging collisions there.
          */
         short_errors = prepare_error_table (MODES_SHORT_MSG_BITS, 1, 1, &short_errors_sz);
         long_errors  = prepare_error_table (MODES_LONG_MSG_BITS, 1, 1, &long_errors_sz);
         break;

    default:
         /* Detect up to 4 bit errors; this reduces our 2-bit coverage to about 65%.
          * This can take a little while - tell the user.
          */
         short_errors = prepare_error_table (MODES_SHORT_MSG_BITS, 2, 4, &short_errors_sz);
         long_errors  = prepare_error_table (MODES_LONG_MSG_BITS, 2, 4, &long_errors_sz);
         break;
  }
}

/**
 * Given an error syndrome and message length, return
 * an error-correction descriptor, or NULL if the
 * syndrome is uncorrectable
 */
errorinfo *crc_checksum_diagnose (uint32_t syndrome, int bitlen)
{
  errorinfo *table, ei;
  int        tablesize;

  if (syndrome == 0)
     return (&NO_ERRORS);

  assert (bitlen == MODES_SHORT_MSG_BITS || bitlen == MODES_LONG_MSG_BITS);
  if (bitlen == MODES_SHORT_MSG_BITS)
  {
    table     = short_errors;
    tablesize = short_errors_sz;
  }
  else
  {
    table     = long_errors;
    tablesize = long_errors_sz;
  }

  if (!table)  /* no CRC checking */
     return (NULL);

  ei.syndrome = syndrome;
  return bsearch (&ei, table, tablesize, sizeof(errorinfo), syndrome_compare);
}

/**
 * Given a message and an error-correction descriptor,
 * apply the error correction to the given message.
 */
void crc_checksum_fix (uint8_t *msg, const errorinfo *info)
{
  int i;

  assert (info);
  for (i = 0; i < info->errors; ++i)
      msg [info->bit[i] >> 3] ^= 1 << (7 - (info->bit[i] & 7));
}

