/**
 * For '../packed_test.exe'.
 *
 * Disable some Mongoose features not needed here.
 */
#undef  MG_ENABLE_EPOLL
#undef  MG_ENABLE_FILE
#define MG_ENABLE_EPOLL 0
#define MG_ENABLE_FILE  0

#include "misc.h"
#include "mongoose.c"

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

static file_packed *lookup_table = NULL;
static size_t       lookup_table_sz = 0;
static int          g_rc = 0;

static void check_specs (spec_func spec_1, spec_func spec_2)
{
  if (strcmp((*spec_1)(), (*spec_2)()))
  {
    fprintf (stderr, "'spec_1()' -> '%s'\n", (*spec_1)());
    fprintf (stderr, "'spec_2()' -> '%s'\n", (*spec_2)());
    g_rc++;
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
    g_rc++;
    fprintf (stderr, "'unlist_1()' gave %zu files. But 'unlist_2()' gave %zu files.\n", num_1, num_2);
    lookup_table_sz = min (num_1, num_2);
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
    g_rc++;
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
  int    _rc = g_rc;

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
  fprintf (stderr, "'unpack_1()' and 'unpack_2()' returned %s files.\n",
           _rc == g_rc ? "the same" : "different");
}

static int compare_on_name (const void *_a, const void *_b)
{
  const file_packed *a = (const file_packed*) _a;
  const file_packed *b = (const file_packed*) _b;
  return strcmp (a->name, b->name);
}

static void create_lookup_table (unlist_func unlist, unpack_func unpack)
{
  const char  *fname;
  size_t       num;
  file_packed *l;

  lookup_table = l = malloc (sizeof(*lookup_table) * lookup_table_sz);
  for (num = 0; (fname = (*unlist)(num)) != NULL; num++, l++)
  {
    l->name = fname;
    l->data = (const unsigned char*) (*unpack) (fname, &l->size, &l->mtime);
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
  file_packed key;
  double      now = get_usec_now();
  const char *data;

  key.name = fname;
  data = bsearch (&key, lookup_table, lookup_table_sz, sizeof(*lookup_table), compare_on_name);
  assert (data);
  return (get_usec_now() - now);
}

/*
 * Check the lookup speed of a normal 'unpack()' vs. a 'bsearch()' based lookup.
 */
static void check_speed (unlist_func unlist_1, unlist_func unlist_2,
                         unpack_func unpack_1, unpack_func unpack_2,
                         size_t max_loops)
{
  const char *fname_1, *fname_2;
  double      time_normal  = 0.0;
  double      time_bsearch = 0.0;
  uint64_t    per_sec;
  size_t      loops, idx;

  create_lookup_table (unlist_1, unpack_1);

  for (loops = 0; loops < max_loops; loops++)
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

  per_sec = (uint64_t) (loops * 1E6 / time_bsearch);
  fprintf (stderr, "bsearch: %3.2f usec/lookup, %s lookups/sec.\n",
           time_bsearch / loops, qword_str(per_sec));

  per_sec = (uint64_t) (loops * 1E6 / time_normal);
  fprintf (stderr, "normal:  %3.2f usec/lookup, %s lookups/sec.\n",
           time_normal / loops, qword_str(per_sec));
  free (lookup_table);
}

#if defined(USE_PACKED_DLL)
static spec_func   dll_mg_spec_1,   dll_mg_spec_2;
static unlist_func dll_mg_unlist_1, dll_mg_unlist_2;
static unpack_func dll_mg_unpack_1, dll_mg_unpack_2;

static bool load_web_dll (const char *dll)
{
  MODES_NOTUSED (dll);
  return (true);
}

static bool unload_web_dll (void)
{
  return (true);
}
#endif

static void check_DLL (const char *dll_basename)
{
  mg_file_path dll_fullname;

  snprintf (dll_fullname, sizeof(dll_fullname), "%s\\%s", Modes.where_am_I, dll_basename);

#if defined(USE_PACKED_DLL) && 0
  /*
   * Add a version of this from net_io.c:
   */
  if (!load_web_dll(dll_fullname))
     rc++;

  check_specs   (dll_mg_spec_1, dll_mg_spec_2);
  check_numbers (dll_mg_unlist_1, dll_mg_unlist_2);
  check_listing (dll_mg_unlist_1, dll_mg_unlist_2, dll_mg_unpack_1, dll_mg_unpack_2);
  check_sizes   (dll_mg_unlist_1, dll_mg_unlist_2, dll_mg_unpack_1, dll_mg_unpack_2);
  check_speed   (dll_mg_unlist_1, dll_mg_unlist_2, dll_mg_unpack_1, dll_mg_unpack_2, 1000);

  unload_web_dll();
#endif

  MODES_NOTUSED (dll_fullname);
  MODES_NOTUSED (dll_basename);
}

static void init (void)
{
  memset (&Modes, '\0', sizeof(Modes));
  GetModuleFileNameA (NULL, Modes.who_am_I, sizeof(Modes.who_am_I));
  snprintf (Modes.where_am_I, sizeof(Modes.where_am_I), "%s", dirname(Modes.who_am_I));
}

int main (void)
{
  init();
//check_specs   (mg_spec_1, mg_spec_2);
  check_numbers (mg_unlist_1, mg_unlist_2);
  check_listing (mg_unlist_1, mg_unlist_2, mg_unpack_1, mg_unpack_2);
  check_sizes   (mg_unlist_1, mg_unlist_2, mg_unpack_1, mg_unpack_2);
  check_speed   (mg_unlist_1, mg_unlist_2, mg_unpack_1, mg_unpack_2, 1000);
  check_DLL ("web-pages.dll");
  return (g_rc);
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
DEAD_CODE (uint32_t,     0,   rtlsdr_last_error, (void))

#if defined(MG_ENABLE_PACKED_FS) && (MG_ENABLE_PACKED_FS == 1)
  DEAD_CODE (const char *, "?", mg_unpack, (const char *name, size_t *size, time_t *mtime))
  DEAD_CODE (const char *, "?", mg_unlist, (size_t i))
#endif