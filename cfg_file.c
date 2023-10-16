/**
 *\file     cfg_file.c
 * \ingroup Misc
 * \brief   Config-file handling.
 */
#include "cfg_file.h"

/**
 * \def CFG_WARN()
 * Warn about unknown keys etc.
 */
#define CFG_WARN(fmt, ...)                                    \
        do {                                                  \
           fprintf (stderr, "%s(%u): WARNING: " fmt ".\n",    \
                    g_ctx->current_file, g_ctx->current_line, \
                    __VA_ARGS__);                             \
        } while (0)

/**
 * \def CTX_CLONE()
 * Clone current context to an new context (`new_ctx`) before
 * parsing internal keywords or an included .cfg-file.
 */
#define CTX_CLONE()                              \
        cfg_context new_ctx;                     \
        g_ctx = &new_ctx;                        \
        memcpy (&new_ctx, ctx, sizeof(new_ctx)); \
        new_ctx.internal = true

/**
 * \def TRACE()
 * A local debug and trace macro.
 */
#undef  TRACE
#define TRACE(level, fmt, ...)                         \
        do {                                           \
          if (ctx->test_level >= level)                \
             printf ("%s(%u): " fmt ".\n",             \
                     __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

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
 *  \ref https://devblogs.microsoft.com/oldnewthing/20100203-00/?p=15083
 *  \ref https://learn.microsoft.com/en-gb/windows/win32/api/processenv/nf-processenv-getenvironmentvariablea
 */
#define MAX_ENV_LEN  32767

/**
 * \def MAX_LINE_LEN
 * Max length of a line; key + value.
 */
#define MAX_LINE_LEN (1000 + MAX_ENV_LEN)

/**
 * The current config-file and the directory it is in.
 */
static mg_file_path  g_our_cfg, g_our_dir;

/**
 * The global context for `CFG_WARN()`
 */
static const cfg_context *g_ctx;

static bool handle_include   (cfg_context *ctx, const char *key, const char *value);
static bool handle_message   (cfg_context *ctx, const char *key, const char *value);
static bool handle_ipv4_test (cfg_context *ctx, const char *key, const char *value);
static bool handle_ipv6_test (cfg_context *ctx, const char *key, const char *value);

static cfg_table internals [] = {
    { "include",           ARG_FUNC2, (void*) handle_include },
    { "message",           ARG_FUNC2, (void*) handle_message },
    { "internal.ip4_test", ARG_FUNC2, (void*) handle_ipv4_test },
    { "internal.ip6_test", ARG_FUNC2, (void*) handle_ipv6_test },
    { NULL,                0,         NULL }
  };

static bool is_internal_key (const char *key)
{
  if (!stricmp(key, "include") || !stricmp(key, "message") || !strnicmp(key, "internal.", 9))
     return (true);
  return (false);
}

static int   cfg_parse_file    (cfg_context *ctx);
static bool  cfg_parse_line    (cfg_context *ctx, char **key_p, char **value_p);
static bool  cfg_parse_table   (cfg_context *ctx, const char *key, const char *value);
static char *cfg_getenv_expand (cfg_context *ctx, const char *variable);

bool cfg_open_and_parse (cfg_context *ctx)
{
  int rc = 0;

  g_ctx = ctx;

  strcpy_s (g_our_cfg, sizeof(g_our_cfg), ctx->fname);
  snprintf (g_our_dir, sizeof(g_our_dir), "%s\\", dirname(g_our_cfg));

  strcpy_s (ctx->current_file, sizeof(ctx->current_file), ctx->fname);
  ctx->file = fopen (ctx->current_file, "rb");
  if (!ctx->file)
  {
    CFG_WARN ("Failed to open \"%s\"", ctx->current_file);
    return (false);
  }

  rc = cfg_parse_file (ctx);
  fclose (ctx->file);

  TRACE (1, "rc from `cfg_parse_file()': %d", rc);
  return (rc > 0);
}

bool cfg_true (const char *arg)
{
  assert (arg);

  if (*arg == '1' || !stricmp(arg, "true") || !stricmp(arg, "yes") || !stricmp(arg, "on"))
     return (true);

  if (!(*arg == '0' || !stricmp(arg, "false") || !stricmp(arg, "no") || !stricmp(arg, "off")))
     CFG_WARN ("failed to match '%s' as 'false'", arg);

  return (false);
}

/*
 * Parse the config-file given in 'file'.
 */
static int cfg_parse_file (cfg_context *ctx)
{
  int rc = 0;

  do
  {
    char *key, *value, *expanded;

    if (!cfg_parse_line(ctx, &key, &value))
       break;

    if (!*value)      /* foo = <empty value> */
       continue;

     expanded = cfg_getenv_expand (ctx, value);

     if (cfg_parse_table(ctx, key, expanded ? expanded : value))
        rc += 1;
     else
     {
       CTX_CLONE();
       new_ctx.tab = internals;
       rc += cfg_parse_table (&new_ctx, key, expanded ? expanded : value);
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
  char *p, *q;

  while (1)
  {
    char buf [MAX_LINE_LEN];

    if (!fgets(buf, sizeof(buf)-1, ctx->file))  /* EOF */
       return (false);

    p = str_trim (buf);            /* remove leadings/trailing spaces */
    if (*p == '#' || *p == ';')    /* ignore comment lines */
    {
      ctx->current_line++;
      continue;
    }

    if (*p == '\r' || *p == '\n')   /* ignore empty lines */
    {
      ctx->current_line++;
      continue;
    }

    if (sscanf(p, "%[^= ] = %[^\r\n]", ctx->current_key, ctx->current_val) != 2)
    {
      TRACE (1, "%s(%u): No match for key/val in '%s'", ctx->current_file, ctx->current_line, p);
      ctx->current_line++;
      continue;
    }

    q = strrchr (ctx->current_val, '\"');
    p = strchr (ctx->current_val, ';');
    if (p > q)                /* Remove trailing comments */
       *p = '\0';

    p = strchr (ctx->current_val, '#');
    if (p > q)
       *p = '\0';

    break;
  }

  ctx->current_line++;
  *key_p   = ctx->current_key;
  *value_p = str_trim (ctx->current_val);
  return (true);
}

static const char *type_name (cfg_tab_types type)
{
  return (type == ARG_ATOB    ? "ARG_ATOB"    :
          type == ARG_ATOI    ? "ARG_ATOI"    :
          type == ARG_ATO_U8  ? "ARG_ATO_U8"  :
          type == ARG_ATO_U16 ? "ARG_ATO_U16" :
          type == ARG_ATO_U32 ? "ARG_ATO_U32" :
          type == ARG_ATO_U64 ? "ARG_ATO_U64" :
          type == ARG_ATO_IP4 ? "ARG_ATO_IP4" :
          type == ARG_ATO_IP6 ? "ARG_ATO_IP6" :
          type == ARG_FUNC1   ? "ARG_FUNC1"   :
          type == ARG_FUNC2   ? "ARG_FUNC2"   :
          type == ARG_STRDUP  ? "ARG_STRDUP"  :
          type == ARG_STRCPY  ? "ARG_STRCPY"  : "??");
}

#define RANGE_CHECK(val, low, high)                         \
        do {                                                \
          if (((val) < (low)) || ((val) > (high))) {        \
             CFG_WARN ("Value %llu exceed range [%d - %d]", \
                       val, low, high);                     \
             return (0);                                    \
          }                                                 \
        } while (0)

/**
 * Parse and store a `ARG_ATOx` values.
 */
static bool parse_and_set_value (cfg_context *ctx, const char *key, const char *value, void *arg, int size)
{
  uint64_t val = 0;
  bool     ok;

  TRACE (2, "parsing key: '%s', value: '%s'", key, value);

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
    CFG_WARN ("failed to match '%s' as a 'bool'", value);
    return (false);
  }

  ok = (sscanf(value, "%lld", &val) == 1);
  if (!ok)
     CFG_WARN ("failed to match '%s' as decimal in key '%s'", value, key);
  else
  {
    switch (size)
    {
      case 1:
           RANGE_CHECK (val, 0, UCHAR_MAX);
           *(uint8_t*) arg = (uint8_t) val;
           break;
      case 2:
           RANGE_CHECK (val, 0, (int)USHRT_MAX);
           *(uint16_t*) arg = (uint16_t) val;
           break;
      case 4:
           *(uint32_t*) arg = (uint32_t) val;
           break;
      case 8:
           *(uint64_t*) arg = (uint64_t) val;
           break;
      default:
          *(int*) arg = val;
          break;
    }
  }
  return (ok);
}

/**
 * Parse and store `ARG_ATO_IP4/6` values.
 */
static bool parse_and_set_ip (cfg_context *ctx, const char *key, const char *value, void *arg, bool is_ip6)
{
  TRACE (2, "parsing key: '%s', value: '%s'", key, value);

  if (is_ip6)
  {
    mg_str  str6 = mg_str (value);
    mg_addr addr6;

    if (!mg_aton(str6, &addr6) || !addr6.is_ip6)
    {
      CFG_WARN ("Illegal IPv6-address: '%s'", value);
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
      CFG_WARN ("Illegal IPv4-address: '%s'", value);
      return (false);
    }
    memcpy (arg, &addr4, sizeof(addr4));
  }
  return (true);
}

static bool cfg_parse_table (cfg_context *ctx, const char *key, const char *value)
{
  const cfg_table *tab;
  bool  rc    = false;
  bool  found = false;

  for (tab = ctx->tab; tab->key; tab++)
  {
    void *arg;
    char *str;

    if (stricmp(tab->key, key))
       continue;

    found = true;
    arg   = tab->arg_func;   /* storage or function to call */

    switch (tab->type)
    {
      case ARG_ATOB:
           rc = parse_and_set_value (ctx, key, value, arg, -1);
           break;

      case ARG_ATOI:
           rc = parse_and_set_value (ctx, key, value, arg, sizeof(int));
           break;

      case ARG_ATO_U8:
           rc = parse_and_set_value (ctx, key, value, arg, sizeof(uint8_t));
           break;

      case ARG_ATO_U16:
           rc = parse_and_set_value (ctx, key, value, arg, sizeof(uint16_t));
           break;

      case ARG_ATO_U32:
           rc = parse_and_set_value (ctx, key, value, arg, sizeof(uint32_t));
           break;

      case ARG_ATO_U64:
           rc = parse_and_set_value (ctx, key, value, arg, sizeof(uint64_t));
           break;

      case ARG_ATO_IP4:
           rc = parse_and_set_ip (ctx, key, value, arg, false);
           break;

      case ARG_ATO_IP6:
           rc = parse_and_set_ip (ctx, key, value, arg, true);
           break;

      case ARG_FUNC1:
           rc = ((cfg_callback1) arg) (value);
           break;

      case ARG_FUNC2:
           rc = ((cfg_callback2) arg) (ctx, key, value);
           break;

      case ARG_STRDUP:
           str = strdup (value);
           if (str)
             *(char**)arg = str;
           rc = str ? true : false;
           break;

      case ARG_STRCPY:
           strncpy ((char*)arg, value, MAX_VALUE_LEN);
           rc = true;
           break;

      default:
           CFG_WARN ("Unknown type = %s (%d) for '%s = %s'",
                     type_name(tab->type), tab->type, key, value);
           break;
    }
    TRACE (2, "%s, matched '%s' = '%s'", type_name(tab->type), key, value);
    break;
  }

  /* Warn only on unknown "external" key/values
   */
  if (!found && !ctx->internal && !is_internal_key(key))
     CFG_WARN ("Unknown key/value: '%s = %s'.\n", key, value);
  return (rc);
}

/**
 * Returns the expanded version of an variable.
 *
 * \eg If `INCLUDE=c:\VC\include;%C_INCLUDE_PATH%` and
 *   + `C_INCLUDE_PATH=c:\MinGW\include`, the expansion returns
 *   + `c:\VC\include;c:\MinGW\include`.
 */
static char *cfg_getenv_expand (cfg_context *ctx, const char *variable)
{
  const char *orig_var = variable;
  char *p1, *p2, *rc, *env = NULL;
  char  buf1 [MAX_ENV_LEN];
  char  buf2 [MAX_ENV_LEN];

  p1 = strstr (variable, "%0");
  p2 = strstr (variable, "%~dp0");

  if (p1)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p1 - variable), variable, g_our_cfg, p1 + 2);
    env = buf1;
  }
  else if (p2)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p2 - variable), variable, g_our_dir, p2 + 5);
    env = buf1;
  }
  else
  {
    /* Don't use getenv(); it doesn't find variable added after program was
     * started. Don't accept truncated results (i.e. rc >= sizeof(buf1)).
     */
    DWORD ret = GetEnvironmentVariable (variable, buf1, sizeof(buf1));

    if (ret > 0 && ret < sizeof(buf1))
    {
      env      = buf1;
      variable = buf1;
    }
    if (strchr(variable, '%'))
    {
      /* buf2 == variable if not expanded.
       */
      ret = ExpandEnvironmentStrings (variable, buf2, sizeof(buf2));
      if (ret > 0 && ret < sizeof(buf2) &&
          !strchr(buf2, '%'))    /* no variables still un-expanded */
         env = buf2;
    }
  }

  rc = (env && env[0]) ? strdup(env) : NULL;
  TRACE (2, "env: '%s', expanded: '%s'", orig_var, rc);
  return (rc);
}

/**
 * Functions for `internals[]` tests:
 */
static bool handle_ipv4_test (cfg_context *ctx, const char *key, const char *value)
{
  mg_host_name addr;
  mg_addr      ip;
  bool         rc;

  memset (&ip, '\0', sizeof(ip));
  rc = parse_and_set_ip (ctx, key, value, &ip, false);
  if (rc)
     mg_snprintf (addr, sizeof(addr), "%M", mg_print_ip, &ip);
  printf ("internal.ip4_test1: %s\n", rc ? addr : "??");
  return (rc);
}

static bool handle_ipv6_test (cfg_context *ctx, const char *key, const char *value)
{
  mg_host_name addr;
  mg_addr      ip;
  bool         rc;

  memset (&ip, '\0', sizeof(ip));
  rc = parse_and_set_ip (ctx, key, value, &ip, true);
  if (rc)
     mg_snprintf (addr, sizeof(addr), "%M", mg_print_ip, &ip);
  printf ("internal.ip6_test1: %s\n", rc ? addr : "??");
  return (rc);
}

static bool handle_message (cfg_context *ctx, const char *key, const char *value)
{
  printf ("Message: '%s'\n", value);
  (void) ctx;
  (void) key;
  return (true);
}

static bool handle_include (cfg_context *ctx, const char *key, const char *value)
{
  struct stat st;
  const char *new_file = value;
  bool        ignore   = false;

  memset (&st, '\0', sizeof(st));

  if (*new_file != '?' && (stat(new_file, &st) || ((st.st_mode) & _S_IFMT) != _S_IFREG))
  {
    CFG_WARN ("%s-file \"%s\" is not a regular file", key, new_file);
    return (false);
  }

  if (*new_file == '?')
  {
    new_file++;
    if (stat(new_file, &st) || ((st.st_mode) & _S_IFMT) != _S_IFREG)
    {
      CFG_WARN ("Ignoring %s-file \"%s\" not found", key, new_file);
      ignore = true;
    }
  }

  TRACE (1, "new_file \"%s\", ignore: %d", new_file, ignore);
  if (!ignore)
  {
    CTX_CLONE();
    new_ctx.fname = new_file;
    return cfg_open_and_parse (&new_ctx);
  }
  return (true);
}

