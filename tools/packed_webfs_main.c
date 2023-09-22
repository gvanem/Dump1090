/**
 * For 'packed_test.exe'.
 */
#include "misc.h"

typedef const char *(*spec_func) (void);
typedef const char *(*unlist_func) (size_t i);
typedef const char *(*unpack_func) (const char *name, size_t *size, time_t *mtime);

extern const char *mg_unlist_1 (size_t i);
extern const char *mg_unpack_1 (const char *name, size_t *size, time_t *mtime);
extern const char *mg_spec_1 (void);

/*
 * The '--minify' version of the above data.
 */
extern const char *mg_unlist_2 (size_t i);
extern const char *mg_unpack_2 (const char *name, size_t *size, time_t *mtime);
extern const char *mg_spec_2 (void);

static int          rc = 0;
static packed_file *lookup_table = NULL;
static size_t       lookup_table_sz = 0;

static void check_specs (spec_func spec_1, spec_func spec_2)
{
  if (strcmp((*spec_1)(), (*spec_2)()))
  {
    fprintf (stderr, "'spec_1()' -> '%s'\n", (*spec_1)());
    fprintf (stderr, "'spec_2()' -> '%s'\n", (*spec_2)());
    rc++;
  }
}

static void check_numbers (unlist_func unlist_1, unlist_func unlist_2)
{
  size_t num_1, num_2;

  for (num_1 = 0; (*unlist_1)(num_1); num_1++)
      ;
  for (num_2 = 0; (*unlist_2)(num_2); num_2++)
      ;

  if (num_1 == num_2)
  {
    fprintf (stderr, "Both 'mg_unlist_1()' and 'mg_unlist_2()' has %zu files.\n", num_1);
    lookup_table_sz = num_1;
  }
  else
  {
    rc++;
    fprintf (stderr, "'unlist_1()' gave %zu files. But 'unlist_2()' gave %zu files.\n", num_1, num_2);
  }
}

static void check_sizes (unlist_func unlist_1, unlist_func unlist_2,
                         unpack_func unpack_1, unpack_func unpack_2)
{
  size_t      ftotal_1 = 0;
  size_t      ftotal_2 = 0;
  size_t      num;
  const char *data;
  const char *fname;
  size_t      fsize;
  time_t      mtime;

  for (num = 0; (fname = (*unlist_1)(num)) != NULL; num++)
  {
    data = (*unpack_1) (fname, &fsize, &mtime);
    assert (data);
    assert (mtime > 0);
    ftotal_1 += fsize;
  }
  for (num = 0; (fname = (*unlist_2)(num)) != NULL; num++)
  {
    data = (*unpack_2) (fname, &fsize, &mtime);
    assert (data);
    assert (mtime > 0);
    ftotal_2 += fsize;
  }

  if (ftotal_2 >= ftotal_1)
  {
    fprintf (stderr, "'unpack_2()' showed no '--minify' benefit.\n");
    rc++;
  }
  else
  {
    fprintf (stderr, "'unlist_1()' returned %s bytes.\n", dword_str(ftotal_1));
    fprintf (stderr, "'unlist_2()' returned %s bytes.\n", dword_str(ftotal_2));
  }
}

/*
 * Check that both lists returns the same files with the time-stamps.
 */
static void check_listing (unlist_func unlist_1, unlist_func unlist_2,
                           unpack_func unpack_1, unpack_func unpack_2)
{
  size_t num;
  int    _rc = rc;

  for (num = 0;; num++)
  {
    const char *fname_1 = (*unlist_1) (num);
    const char *fname_2 = (*unlist_2) (num);
    time_t      mtime_1, mtime_2;

    if (!fname_1 || !fname_2)
       break;

    (*unpack_1) (fname_1, NULL, &mtime_1);
    (*unpack_2) (fname_2, NULL, &mtime_2);

    if (strcmp(fname_1, fname_2) || mtime_1 != mtime_2)
       _rc++;
  }
  fprintf (stderr, "'unpack_1()' and 'unpack_2()' returned %s files.\n", _rc == rc ? "the same" : "different");
}

static int compare_on_name (const void *_a, const void *_b)
{
  const packed_file *a = (const packed_file*) _a;
  const packed_file *b = (const packed_file*) _b;
  return strcmp (a->name, b->name);
}

static void create_lookup_table (unlist_func unlist, unpack_func unpack)
{
  const char *fname;
  size_t      num;

  lookup_table = malloc (sizeof(*lookup_table) * lookup_table_sz);
  for (num = 0; (fname = (*unlist)(num)) != NULL; num++)
  {
    lookup_table[num].name = fname;
    lookup_table[num].data = (const unsigned char*) (*unpack) (fname, &lookup_table[num].size, &lookup_table[num].mtime);
  }
  qsort (lookup_table, num, sizeof(*lookup_table), compare_on_name);
}

/*
 * Test the speed of a normal 'unpack()'.
 */
static double normal_test (const char *fname, unpack_func unpack)
{
  double      now = get_usec_now();
  const char *data = (*unpack) (fname, NULL, NULL);

  assert (data);
  return (get_usec_now() - now);
}

/*
 * Test the speed of a 'bsearch()' lookup.
 */
static double bsearch_test (const char *fname)
{
  packed_file key;
  double      now = get_usec_now();
  const char *data;

  key.name = fname;
  data = bsearch (&key, lookup_table, lookup_table_sz, sizeof(*lookup_table), compare_on_name);
  assert (data);
  return (get_usec_now() - now);
}

/*
 * Check the lookup speed of an normal 'unpack()' vs. a 'bsearch()' based lookup.
 */
static void check_speed (unlist_func unlist_1, unlist_func unlist_2,
                         unpack_func unpack_1, unpack_func unpack_2)
{
  const char *fname_1, *fname_2;
  double      time_normal  = 0.0;
  double      time_bsearch = 0.0;
  size_t      loops, idx;

  create_lookup_table (unlist_1, unpack_1);

  for (loops = 0; loops < 1000; loops++)
  {
    idx = random_range (0, lookup_table_sz - 1);
    fname_1 = (*unlist_1) (idx);
    fname_2 = (*unlist_2) (idx);
    assert (fname_1);
    assert (fname_2);
    assert (strcmp(fname_1, fname_2) == 0);

    time_bsearch += bsearch_test (fname_1);
    time_normal  += normal_test (fname_2, unpack_2);
  }
  fprintf (stderr, "bsearch: %3.2f usec/lookup, %7llu lookups/sec.\n", time_bsearch / loops, (uint64_t) (loops * 1E6 / time_bsearch));
  fprintf (stderr, "normal:  %3.2f usec/lookup, %7llu lookups/sec.\n", time_normal / loops,  (uint64_t) (loops * 1E6 / time_normal));
  free (lookup_table);
}

int main (void)
{
  check_specs   (mg_spec_1, mg_spec_2);
  check_numbers (mg_unlist_1, mg_unlist_2);
  check_listing (mg_unlist_1, mg_unlist_2, mg_unpack_1, mg_unpack_2);
  check_sizes   (mg_unlist_1, mg_unlist_2, mg_unpack_1, mg_unpack_2);
  check_speed   (mg_unlist_1, mg_unlist_2, mg_unpack_1, mg_unpack_2);
  return (rc);
}

/*
 * To allow 'misc.obj' to link in.
 */
#ifdef __clang__
  #pragma clang diagnostic ignored "-Wunused-parameter"
#else
  #pragma warning (disable: 4100)
#endif

global_data Modes;

#define DEAD_CODE(ret, rv, func, args)  ret func args { return (rv); }

DEAD_CODE (const char *, "?", mz_version, (void))
DEAD_CODE (const char *, "?", sqlite3_libversion, (void))
DEAD_CODE (const char *, "?", sqlite3_compileoption_get, (int N))
DEAD_CODE (const char *, "?", trace_strerror, (DWORD err))
DEAD_CODE (const char *, "?", mg_unpack, (const char *name, size_t *size, time_t *mtime))
DEAD_CODE (const char *, "?", mg_unlist, (size_t i))
DEAD_CODE (uint32_t,     0,   rtlsdr_last_error, (void))
