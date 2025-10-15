/**
 *\file     cfg_file.c
 * \ingroup Misc
 * \brief   Config-file handling.
 */
#include "misc.h"
#include "cfg_file.h"

/**
 * \def CFG_WARN()
 * Warn about unknown keys etc.
 */
#define CFG_WARN(fmt, ...)                           \
        do {                                         \
           fprintf (stderr, "%s(%u): WARNING: " fmt, \
                    cfg_current_file(),              \
                    cfg_current_line(),              \
                    __VA_ARGS__);                    \
        } while (0)

/**
 * \def TRACE()
 * For development; more compact `DEBUG (DEBUG_GENERAL...)` macro.
 */
#undef  TRACE
#define TRACE(fmt, ...)  DEBUG (DEBUG_CFG_FILE, fmt, ## __VA_ARGS__)
#define TRACE_ARG(type)  TRACE ("Doing '%s' for '%s = %s'.\n", #type, key, value)

/**
 * \def MAX_VALUE_LEN
 * Max length of an `ARG_STRCPY` parameter.
 */
#ifndef MAX_VALUE_LEN
#define MAX_VALUE_LEN 300
#endif

static_assert (MAX_VALUE_LEN >= sizeof(mg_file_path), "MAX_VALUE_LEN too small");

/**
 * \def MAX_ENV_LEN
 * Max length of an environment variable value. According to:
 *  \sa https://devblogs.microsoft.com/oldnewthing/20100203-00/?p=15083
 *  \sa https://learn.microsoft.com/en-gb/windows/win32/api/processenv/nf-processenv-getenvironmentvariablea
 */
#define MAX_ENV_LEN  32767

/**
 * \def MAX_LINE_LEN
 * Max length of a line; key + value.
 */
#define MAX_LINE_LEN (1000 + MAX_ENV_LEN)

/**
 * \typedef cfg_context
 * The context for one config-file.
 */
typedef struct cfg_context {
        const cfg_table *table;
        FILE            *file;
        mg_file_path     current_file;    /**< Current config-file; Possibly included from context above us. */
        mg_file_path     current_dir;     /**< Directory of current config-file; Not CWD. */
        unsigned         current_line;
        char             current_key [256];
        char             current_val [512];
      } cfg_context;

/**
 * The globals:
 */
static cfg_context g_ctx [4];
static int         g_idx = 0;

static int   cfg_parse_file    (cfg_context *ctx);
static bool  cfg_parse_line    (cfg_context *ctx, char **key_p, char **value_p);
static bool  cfg_parse_table   (cfg_context *ctx, const char *key, const char *value);
static char *cfg_getenv_expand (cfg_context *ctx, const char *variable);

char *cfg_current_file (void)
{
  if (g_idx > 0)
     return (g_ctx[g_idx-1].current_file);
  return (NULL);
}

unsigned cfg_current_line (void)
{
  if (g_idx > 0)
     return (g_ctx[g_idx-1].current_line);
  return (0);
}

/*
 * Open and parse a config-file for internal or external key/value pairs.
 *
 * \param fname the config-file to parse.
 * \param table the config-table to use for matching key/values.
 *
 * \retval false for externals on error.
 * \retval true  for externals on success.
 * \retval true  for internal values to avoid calling `cfg_parse_table()`
 *               again for external key/values.
 */
bool cfg_open_and_parse (const char *fname, const cfg_table *table)
{
  mg_file_path full_name = "?";
  cfg_context *ctx;
  FILE        *file;
  DWORD        len;
  int          rc = 0;

  if (g_idx == DIM(g_ctx))
  {
    CFG_WARN ("Too many nested include files. Max %zu.\n", DIM(g_ctx));
    return (false);
  }

  ctx = g_ctx + g_idx;

  strncpy (ctx->current_file, fname, sizeof(ctx->current_file) -1);

  len = GetFullPathName (fname, sizeof(full_name), full_name, NULL);
  if (len > 0)
       strncpy (ctx->current_dir, dirname(full_name), sizeof(ctx->current_dir)-1);
  else strncpy (ctx->current_dir, dirname(fname), sizeof(ctx->current_dir)-1);

  TRACE ("ctx->current_dir: '%s'.\n", ctx->current_dir);

  file = fopen (fname, "rb");
  if (!file)
  {
    g_idx++;
    CFG_WARN ("Failed to open \"%s\".\n", fname);
    g_idx--;
    return (false);
  }

  ctx->table = (cfg_table*) table;
  ctx->file  = file;

  TRACE ("g_idx: %d\n", g_idx);
  g_idx++;

  rc = cfg_parse_file (ctx);
  TRACE ("rc from `cfg_parse_file()': %d, g_idx: %d\n", rc, g_idx);

  fclose (ctx->file);
  g_idx--;

  memset (ctx, '\0', sizeof(*ctx));
  return (rc > 0);
}

/**
 * Match a value as (an alias for) `true` or `false`.
 */
bool cfg_true (const char *arg)
{
  assert (arg);

  if (*arg == '1' || !stricmp(arg, "true") || !stricmp(arg, "yes") || !stricmp(arg, "on"))
     return (true);

  if (!(*arg == '0' || !stricmp(arg, "false") || !stricmp(arg, "no") || !stricmp(arg, "off")))
     CFG_WARN ("failed to match '%s' as 'false'.\n", arg);

  return (false);
}

/**
 * Handle an `include = "file"` statement.
 *
 * \todo Handle recursive inclusion of a previous file.
 */
static int cfg_include_file (const char *value)
{
  struct stat st;
  const char *new_file = value;
  bool        ignore   = false;

  memset (&st, '\0', sizeof(st));

  if (*new_file != '?' && (stat(new_file, &st) || ((st.st_mode) & _S_IFMT) != _S_IFREG))
  {
    CFG_WARN ("include-file \"%s\" does not exist.\n", new_file);
    return (1);
  }

  if (*new_file == '?')
  {
    new_file++;
    if (stat(new_file, &st) || ((st.st_mode) & _S_IFMT) != _S_IFREG)
    {
      CFG_WARN ("Ignoring include-file \"%s\" not found.\n", new_file);
      ignore = true;
    }
  }

  if (!ignore)
  {
    const cfg_table *table = g_ctx [g_idx-1].table;

    table = g_ctx [0].table;
    TRACE ("new_file \"%s\", ignore: %d, g_idx: %d, table: 0x%p\n", new_file, ignore, g_idx, table);
     return cfg_open_and_parse (new_file, table);
  }
  return (1);
}

/**
 * Parse the config-file given in `ctx->file`.
 *
 * First parse for internals keywords. If no match was found,
 * proceed to parsing the external key/values.
 */
static int cfg_parse_file (cfg_context *ctx)
{
  int rc = 0;

  do
  {
    char *key, *value, *expanded;

    if (!cfg_parse_line(ctx, &key, &value))
       break;

     expanded = cfg_getenv_expand (ctx, value);
     if (expanded)
        value = expanded;

     if (!stricmp(key, "message"))
     {
       printf ("Message: '%s'\n", value);
       rc++;
     }
     else if (!stricmp(key, "include"))
     {
       rc += cfg_include_file (value);
     }
     else
     {
       rc += cfg_parse_table (ctx, key, value);
     }
     free (expanded);
  }
  while (1);

  return (rc);
}

/**
 * Return the next line from the config-file with key and value.
 * Increment line of config-file after a read.
 */
static bool cfg_parse_line (cfg_context *ctx, char **key_p, char **value_p)
{
  char *p;

  while (1)
  {
    char buf [MAX_LINE_LEN];
    int  num;

    ctx->current_key[0] = ctx->current_val[0] = '\0';

    if (!fgets(buf, sizeof(buf)-1, ctx->file))  /* EOF */
    {
      TRACE ("%s(%u): EOF\n", ctx->current_file, ctx->current_line);
      return (false);
   }

    p = str_trim (buf);   /* remove leadings/trailing spaces */
    if (*p == '#')        /* ignore comment lines */
    {
      ctx->current_line++;
      continue;
    }

    if (*p == '\r' || *p == '\n')   /* ignore empty lines */
    {
      ctx->current_line++;
      continue;
    }

    num = sscanf (p, "%[^= ] = %[^\r\n]", ctx->current_key, ctx->current_val);
    if (num != 2)
    {
      if (num == 1 || !ctx->current_val[0])
           TRACE ("%s(%u): Empty value for '%s'\n", ctx->current_file, ctx->current_line, ctx->current_key);
      else TRACE ("%s(%u): No match for key/val in '%s'\n", ctx->current_file, ctx->current_line, p);
      ctx->current_line++;
      continue;
    }

    p = strchr (ctx->current_val, '#');
    if (p)     /* Remove trailing comments */
       *p = '\0';

    p = str_trim (ctx->current_val);
    if (!*p)      /* foo = <empty value> */
    {
      TRACE ("Ignore empty value for current_key: '%s'.\n", ctx->current_key);
      ctx->current_line++;
      continue;
    }
    break;
  }

  ctx->current_line++;
  *key_p   = ctx->current_key;
  *value_p = str_trim (ctx->current_val);
  return (true);
}

#define RANGE_CHECK(val, low, high)                                \
        do {                                                       \
          if (((val) < (low)) || ((val) > (high))) {               \
             CFG_WARN ("Value %llu exceed range [%llu - %llu].\n", \
                       val, (uint64_t)low, (uint64_t)high);        \
             return (false);                                       \
          }                                                        \
        } while (0)

/**
 * Parse and store a `ARG_ATOx` value.
 */
static bool parse_and_set_value (const char *key, const char *value, void *arg, int size)
{
  int64_t val = 0;

  TRACE ("parsing key: '%s', value: '%s'\n", key, value);

  if (size == -1)  /* parse and set a `bool` */
  {
    if (*value == '1' || !stricmp(value, "true") || !stricmp(value, "yes") || !stricmp(value, "on"))
    {
      *(bool*) arg = true;
      return (true);
    }
    if (*value == '0' || !stricmp(value, "false") || !stricmp(value, "no") || !stricmp(value, "off"))
    {
      *(bool*) arg = false;
      return (true);
    }
    CFG_WARN ("failed to match '%s' as a 'bool'.\n", value);
    return (false);
  }

  if (sscanf(value, "%lld", &val) != 1)
  {
    CFG_WARN ("failed to match '%s' as decimal in key '%s'.\n", value, key);
    return (false);
  }

  switch (size)
  {
    case 1:
         RANGE_CHECK (val, 0, UCHAR_MAX);
         *(uint8_t*) arg = (uint8_t) val;
         break;
    case 2:
         RANGE_CHECK (val, 0, USHRT_MAX);
         *(uint16_t*) arg = (uint16_t) val;
         break;
    case 4:
         RANGE_CHECK (val, 0, UINT32_MAX);
         *(uint32_t*) arg = (uint32_t) val;
         break;
    case 8:
         RANGE_CHECK (val, 0, INT64_MAX);
         *(uint64_t*) arg = (uint64_t) val;
         break;
    default:
        *(int*) arg = val;
        break;
  }
  return (true);
}

/**
 * Parse and store `ARG_ATO_IP4/6` values.
 */
static bool parse_and_set_ip (const char *value, void *arg, bool is_ip6)
{
  TRACE ("parsing value: '%s'\n", value);

  if (is_ip6)
  {
    mg_str  str6 = mg_str (value);
    mg_addr addr6;

    if (!mg_aton(str6, &addr6) || !addr6.is_ip6)
    {
      CFG_WARN ("Illegal IPv6-address: '%s'.\n", value);
      return (false);
    }
    memcpy (arg, &addr6, sizeof(addr6));
  }
  else
  {
    mg_str  str4 = mg_str (value);
    mg_addr addr4;

    if (!mg_aton(str4, &addr4) || addr4.is_ip6)
    {
      CFG_WARN ("Illegal IPv4-address: '%s'.\n", value);
      return (false);
    }
    memcpy (arg, &addr4, sizeof(addr4));
  }
  return (true);
}

static bool cfg_parse_table (cfg_context *ctx, const char *key, const char *value)
{
  const cfg_table *table;
  bool  rc    = false;
  bool  found = false;

  for (table = ctx->table; table->key; table++)
  {
    void *arg;
    char *str;

    if (stricmp(table->key, key))
       continue;

    found = true;
    arg   = table->arg_func;   /* storage or function to call */

    switch (table->type)
    {
      case ARG_ATOB:
           TRACE_ARG (ARG_ATOB);
           rc = parse_and_set_value (key, value, arg, -1);
           break;

      case ARG_ATOI:
           TRACE_ARG (ARG_ATOI);
           rc = parse_and_set_value (key, value, arg, sizeof(int));
           break;

      case ARG_ATO_U8:
           TRACE_ARG (ARG_ATO_U8);
           rc = parse_and_set_value (key, value, arg, sizeof(uint8_t));
           break;

      case ARG_ATO_U16:
           TRACE_ARG (ARG_ATO_U16);
           rc = parse_and_set_value (key, value, arg, sizeof(uint16_t));
           break;

      case ARG_ATO_U32:
           TRACE_ARG (ARG_ATO_U32);
           rc = parse_and_set_value (key, value, arg, sizeof(uint32_t));
           break;

      case ARG_ATO_U64:
           TRACE_ARG (ARG_ATO_U64);
           rc = parse_and_set_value (key, value, arg, sizeof(uint64_t));
           break;

      case ARG_ATO_IP4:
           TRACE_ARG (ARG_ATO_IP4);
           rc = parse_and_set_ip (value, arg, false);
           break;

      case ARG_ATO_IP6:
           TRACE_ARG (ARG_ATO_IP6);
           rc = parse_and_set_ip (value, arg, true);
           break;

      case ARG_FUNC:
           TRACE_ARG (ARG_FUNC);
           rc = ((cfg_callback) arg) (value);
           break;

      case ARG_STRDUP:
           TRACE_ARG (ARG_STRDUP);
           str = strdup (value);
           if (str)
             *(char**)arg = str;
           rc = true;
           break;

      case ARG_STRCPY:
           TRACE_ARG (ARG_STRCPY);
           strncpy ((char*)arg, value, MAX_VALUE_LEN);
           rc = true;
           break;
    }
    break;
  }

  if (!found)
     CFG_WARN ("Unknown key/value: '%s = %s'\n", key, value);
  return (rc);
}

/**
 * Returns the expanded version of an variable.
 *
 * E.g. If `INCLUDE=c:\VC\include;%C_INCLUDE_PATH%` and
 *   + `C_INCLUDE_PATH=c:\MinGW\include`, the expansion returns
 *   + `c:\VC\include;c:\MinGW\include`.
 *
 * Also allow a variable like `%FOO`; no trailing `%`.
 */
static char *cfg_getenv_expand (cfg_context *ctx, const char *variable)
{
  char *p1, *p2, *rc;
  char *var = NULL;
  char  buf1 [MAX_ENV_LEN];
  char  buf2 [MAX_ENV_LEN];
  DWORD ret;

  p1 = strstr (variable, "%0");
  p2 = strstr (variable, "%~dp0");
//p3 = strstr (variable, "%~nx0"); /* expand to a file name and extension only */
//p4 = strstr (variable, "%~f0");  /* expand to a fully qualified path name */

  if (p1)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p1 - variable), variable, ctx->current_file, p1 + 2);
    var = buf1;
  }
  else if (p2)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p2 - variable), variable, ctx->current_dir, p2 + 5);
    var = buf1;
  }
#if 0
  else if (p3)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p3 - variable), variable, basename(ctx->current_file), p3 + 5);
    var = buf1;
  }
  else if (p4)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p4 - variable), variable, dirname(ctx->current_file), p4 + 4);
    var = buf1;
  }
#endif
  else
  {
    /* Don't use getenv(); it doesn't find variable added after program was
     * started. Don't accept truncated results (i.e. rc >= sizeof(buf1)).
     */
    ret = GetEnvironmentVariable (variable, buf1, sizeof(buf1));
    if (ret > 0 && ret < sizeof(buf1))
    {
      var      = buf1;
      variable = buf1;
    }
    if (strchr(variable, '%'))
    {
      /* buf2 == variable if not expanded.
       */
      char var2 [MAX_ENV_LEN];

      strncpy (var2, variable, sizeof(var2)-1);
      p1 = strrchr (var2, '\0') - 1;
      if (p1 > var2 && *p1 != '%')     /* Turn `%FOO` into `%FOO%` */
      {
        *p1++ = '%';
        *p1 = '\0';
      }
      ret = ExpandEnvironmentStrings (var2, buf2, sizeof(buf2));
      TRACE ("var2: '%s', buf2: '%s'\n", var2, buf2);

      if (ret > 0 && ret < sizeof(buf2) && !strchr(buf2, '%'))    /* no variables still un-expanded */
      {
        var = buf2;
      }
    }
  }
  rc = (var && var[0]) ? strdup(var) : NULL;
  return (rc);
}
