/**
 *\file cfg_file.c
 * \ingroup Misc
 * \brief   Config-file handling.
 */
#define INSIDE_CFG_FILE_C
#include "cfg_file.h"

#define CFG_WARN(fmt, ...)                                 \
        do {                                               \
           fprintf (stderr, "%s(%u): WARNING: " fmt ".\n", \
                   ctx->current_file, ctx->current_line,   \
                   __VA_ARGS__);                           \
        } while (0)

#define CTX_CLONE()          \
        cfg_context new_ctx; \
        memcpy (&new_ctx, ctx, sizeof(new_ctx))

#undef  TRACE
#define TRACE(level, fmt, ...)                         \
        do {                                           \
          if (ctx->test_level >= level)                \
             printf ("%s(%u): " fmt ".\n",             \
                     __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

/*
 * Max length of an `ARG_STRCPY` parameter.
 */
#ifndef MAX_VALUE_LEN
#define MAX_VALUE_LEN 300
#endif

static_assert (MAX_VALUE_LEN >= sizeof(mg_file_path), "MAX_VALUE_LEN too small");

/*
 * According to:
 *  https://devblogs.microsoft.com/oldnewthing/20100203-00/?p=15083
 *  https://learn.microsoft.com/en-gb/windows/win32/api/processenv/nf-processenv-getenvironmentvariablea
 */
#define MAX_ENV_LEN  32767

static mg_file_path  g_our_dir;

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

static int   cfg_parse_file    (cfg_context *ctx);
static bool  cfg_parse_line    (cfg_context *ctx, char **key_p, char **value_p);
static bool  cfg_parse_table   (cfg_context *ctx, const char *key, const char *value);
static char *cfg_getenv_expand (cfg_context *ctx, const char *variable);

bool cfg_open_and_parse (cfg_context *ctx)
{
  int rc = 0;

  if (ctx->current_level == 0)
  {
    mg_file_path path;

    GetModuleFileNameA (NULL, path, sizeof(path));
    snprintf (g_our_dir, sizeof(g_our_dir), "%s\\", dirname(path));
  }

  strncpy (ctx->current_file, ctx->fname, sizeof(ctx->current_file)-1);
  ctx->file = fopen (ctx->current_file, "rb");
  if (!ctx->file)
  {
    CFG_WARN ("Failed to open \"%s\"", ctx->current_file);
    return (false);
  }

  ctx->current_level++;
  rc = cfg_parse_file (ctx);
  fclose (ctx->file);

  if (ctx->test_level >= 1)
     printf ("rc from cfg_parse_file(): %d\n\n", rc);
  return (rc > 0);
}

bool cfg_true (const char *arg)
{
  assert (arg);
  if (*arg == '1' || !stricmp(arg, "true") || !stricmp(arg, "yes") || !stricmp(arg, "on"))
     return (true);
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
       new_ctx.tab      = internals;
       new_ctx.internal = true;
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
    char buf [1000];

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
  return (type == ARG_ATOI    ? "ARG_ATOI"    :
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

  ok = (sscanf(value, "%lld", &val) == 1);
  if (!ok)
     CFG_WARN ("failed to match '%s' as decimal in '%s'", value, key);
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
    arg = tab->arg_func;   /* storage or function to call */

    switch (tab->type)
    {
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
           strncpy ((char*)arg, value, MAX_VALUE_LEN-1);
           rc = true;
           break;

      default:
           fprintf (stderr, "Something wrong in %s(): '%s' = '%s'. type = %s (%d).\n",
                    __FUNCTION__, key, value, type_name(tab->type), tab->type);
           exit (1);
    }
    TRACE (2, "%s, matched '%s' = '%s'", type_name(tab->type), key, value);
    break;
  }

  /* Only warn on unknown "external" key/values
   */
  if (!found && !ctx->internal && stricmp(key, "include") && strnicmp(key, "internal.", 9))
     fprintf (stderr, "%s(%u): Unknown key/value: '%s = %s'.\n", ctx->current_file, ctx->current_line, key, value);
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
  char *p, *rc, *env = NULL;
  char  buf1 [MAX_ENV_LEN];
  char  buf2 [MAX_ENV_LEN];

  p = strstr (variable, "%~dp0");
  if (p)
  {
    snprintf (buf1, sizeof(buf1), "%.*s%s%s", (int)(p - variable), variable, g_our_dir, p + 5);
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
 * Functions for 'internals []' tests:
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
  printf ("level: %u, Message: '%s'\n", ctx->current_level, value);
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
    new_ctx.fname    = new_file;
    new_ctx.internal = true;
    return cfg_open_and_parse (&new_ctx);
  }
  return (true);
}

