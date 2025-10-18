/*
 * Adapted from:
 *   http://bjoern.hoehrmann.de/utf-8/decoder/dfa/
 */
#include "misc.h"

static const uint8_t utf8_table [] = {
       0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 00 ... 1F
       0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 20 ... 3F
       0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 40 ... 5F
       0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 60 ... 7F
       1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,  // 80 ... 9F
       7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  // A0 ... BF
       8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  // C0 ... DF
       0xA,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3,  // E0 ... EF
       0xB,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,  // F0 ... FF
       0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1,  // S0 ... S0
       1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1,  // S1 ... S2
       1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,  // S3 ... S4
       1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1,  // S5 ... S6
       1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  // S7 ... S8
     };

#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

#ifndef U8_SIZE
#define U8_SIZE 100
#endif

#ifndef U8_NUM
#define U8_NUM 4
#endif

#undef TRACE

#if defined(TEST)
  #define TRACE(...)  printf (__VA_ARGS__)
#else
  #define TRACE(...)  (void)0
#endif

uint32_t utf8_decode (uint32_t *state, uint32_t *codep, uint32_t byte)
{
  uint32_t type = utf8_table [byte];

  *codep = (*state != UTF8_ACCEPT) ?
            (byte & 0x3F) | (*codep << 6) : ((0xFF >> type) & byte);

  *state = utf8_table [256 + 16 * (*state) + type];
  return (*state);
}

bool utf8_check (const char *s)
{
  uint32_t codepoint;
  uint32_t state = UTF8_ACCEPT;

  while (*s)
  {
    utf8_decode (&state, &codepoint, *(uint8_t*)s++);
  }
  return (state == UTF8_ACCEPT);
}

bool utf8_code_points (const char *s, size_t *count)
{
  uint32_t codepoint;
  uint32_t state = UTF8_ACCEPT;

  for (*count = 0; *s; s++)
  {
    if (!utf8_decode(&state, &codepoint, *(uint8_t*)s))
       (*count) ++;
  }
  return (state != UTF8_ACCEPT);
}

bool utf8_print_code_points (const char *s)
{
  uint32_t codepoint;
  uint32_t state = UTF8_ACCEPT;
  int      num = 0;
  bool     rc = true;

  for ( ; *s; s++)
  {
    if (!utf8_decode(&state, &codepoint, *(uint8_t*)s))
    {
      TRACE ("  U+%04X,", codepoint);
      num++;
    }
  }
  if (state != UTF8_ACCEPT)
  {
    TRACE ("The string is not well-formed\n");
    rc = false;
  }
  TRACE (" num: %d\n", num);
  return (rc);
}

bool utf8_to_unicode (const char *s, uint32_t *uc)
{
  uint32_t codepoint;
  uint32_t state = UTF8_ACCEPT;
  bool     rc = true;

  for ( ; *s; s++)
  {
    if (!utf8_decode(&state, &codepoint, *(uint8_t*)s))
       *(uc++) = codepoint;
  }

  if (state != UTF8_ACCEPT)
  {
    TRACE ("The string is not well-formed\n");
    rc = false;
  }
  return (rc);
}

/*
 * Returns length of an utf8 string `s`.
 * Should be the same as  number of codepoints in `s`.
 * Always `codepoints <= strlen(s)`.
 */
size_t utf8_len (const char *s)
{
  size_t len = 0;

  for ( ; *s; s++)
      if ((*s & 0xC0) != 0x80)
         len++;
  return (len);
}

/**
 * Return a `char *` string for a UTF-8 string with proper right padding.
 * (no `wcswidth()` in Windows-SDK).
 */
const char *utf8_format (const char *s, int width)
{
  static char buf [U8_NUM] [U8_SIZE];
  static int  idx = 0;
  size_t      codepoints;
  char       *ret = buf [idx];
  char       *p = ret;
  int         len, extras;

  idx++;             /* use `U8_NUM` buffers in round-robin */
  idx &= (U8_NUM-1);

  utf8_code_points (s, &codepoints);
  extras = strlen(s) - codepoints;

  len = snprintf (p, U8_SIZE, "%-*.*s", extras + width, width, s);
  p += len;

  printf ("extras: %d, len: %2d ", extras, len);

// Årø Airport  '   len: 15
// San Francisco  ' len: 15

  while (len - extras < width)
  {
    *p++ = '-';
    len++;
  }

#if 1
  len = p - ret + 1;
  while (len > width)
  {
    *p-- = '+';
    len--;
  }
#endif

  *p = '\0';

  ret [width] = '\0';
  return (ret);
}

/**
 * Return a `wchar_t *` string for a UTF-8 string with proper left
 * adjusted width. Do it the easy way without `wcswidth()`
 * (which is missing in WinKit).
 */
const wchar_t *utf8_format2 (const char *s, int min_width)
{
  static wchar_t buf [U8_NUM] [U8_SIZE];
  static int     idx = 0;
  wchar_t        s_w [U8_SIZE];
  wchar_t       *ret = buf [idx];
  size_t         wlen, width;

  idx++;             /* use `U8_NUM` buffers in round-robin */
  idx &= (U8_NUM-1);

  memset (s_w, '\0', sizeof(s_w));
  wlen = MultiByteToWideChar (CP_UTF8, 0, s, -1, s_w, U8_SIZE);

  width = min_width + (strlen(s) - wcslen(s_w)) / 2;
  _snwprintf (ret, U8_SIZE-1, L"wlen: %zd, %-*.*s", wlen, (int)width, min_width, s_w);

  return (ret);
}

#if defined(TEST)

#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wbraced-scalar-init"
  #pragma clang diagnostic ignored "-Wunused-parameter"
  #pragma clang diagnostic ignored "-Wpointer-sign"
#endif

static const uint8_t *utf8_tests_1[] = {           // In LaTeX syntax
       { u8"\xC3\x85r\xC3\xB8 Airport" },          // "\AAr\o Airport", Molde, Norway
       { u8"Flor\xC3\xB8 Airport" },               // "Flor\o Airport", Norway
       { u8"Reykjav\xC3\xADk" },                   // "Reykjav\'ik", Iceland
       { u8"Grafenw\xC3\xB6hr" },                  // 'Grafenw\"ohr' Medevac Helipad, Germany
       { u8"San Francisco" },                      // no UTF-8, 14 characters
       { u8"S\xC3\xA3o Va\xC3\xA9\x72\x69\x6F" },  // "S\~ao Val\'erio", Fazenda Pirassununga Airport, Brazil
       { u8"Grafenw\xC3\xB6hr,"
           "Grafenw\xC3\xB6hr,"
           "Grafenw\xC3\xB6hr" }                   // > 15 in width
     };

static const char *column_2 = "column-2";

void run_tests_1 (bool verbose, bool use_utf8_format2)
{
  size_t i, codepoints;
  int    disp_len;

  if (use_utf8_format2)
       puts ("\nUsing utf8_format2():");
  else puts ("\nUsing utf8_format():");

  for (i = 0; i < DIM(utf8_tests_1); i++)
  {
    const char *s = utf8_tests_1 [i];

    assert (utf8_check(s));

    utf8_code_points (s, &codepoints);
    printf ("  codepoints: %2zd(%2zd), ", codepoints, strlen(s));

    if (use_utf8_format2)
         printf ("  '%S' %s\n", utf8_format2(s, 15), column_2);
    else printf ("  '%s' %s\n", utf8_format(s, 15), column_2);

    assert (codepoints <= strlen(s));
    assert (codepoints > 0);

    if (verbose)
       utf8_print_code_points (s);
  }
}

static void run_tests_py (void)
{
  puts ("\nCalling 'py.exe -3 ./utf8-test.py':");
  system ("py.exe -3 ./utf8-test.py");
}

int main (int argc, char **argv)
{
  bool verbose = (argc == 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--verbose")));

  SetConsoleOutputCP (CP_UTF8);

  run_tests_1 (verbose, false);
//run_tests_1 (verbose, true);
//run_tests_py();
  return (0);
}
#endif  /* TEST */


