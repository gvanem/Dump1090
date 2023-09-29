/**
 *\file cfg_file.c
 * \ingroup Misc
 * \brief   Config-file handling.
 */
#define INSIDE_CFG_FILE_C
#include "net_io.h"
#include "cfg_file.h"

#define CFG_WARN(fmt, ...)                                     \
        do {                                                   \
           printf ("%s(%u): WARNING: " fmt ".\n",              \
                   __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

#undef  TRACE
#define TRACE(level, fmt, ...)                         \
        do {                                           \
          if (g_dbg_level >= level)                    \
             printf ("%s(%u): " fmt ".\n",             \
                     __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

/*
 * Max length of an `ARG_STRCPY` parameter.
 */
#define MAX_VALUE_LEN 100

/*
 * According to:
 *  https://devblogs.microsoft.com/oldnewthing/20100203-00/?p=15083
 *  https://learn.microsoft.com/en-gb/windows/win32/api/processenv/nf-processenv-getenvironmentvariablea
 */
#define MAX_ENV_LEN  32767

static unsigned      g_dbg_level = 0;
static mg_file_path  g_our_dir;

static mg_addr ipv4_test;
static mg_addr ipv6_test;

static void handle_include (cfg_context *ctx, const char *key, const char *value);
static void handle_message (cfg_context *ctx, const char *key, const char *value);

static cfg_table internals[] = {
    { "include",  ARG_FUNC2,  (void*) handle_include },
    { "message",  ARG_FUNC2,  (void*) handle_message },
    { "ip4_test", ARG_ATOIP4, (void*) &ipv4_test },
    { "ip6_test", ARG_ATOIP6, (void*) &ipv6_test },
    { NULL,       0,          NULL }
  };

static int   cfg_parse_file  (cfg_context *ctx);
static bool  cfg_parse_line  (cfg_context *ctx, char **key_p, char **value_p);
static bool  cfg_parse_table (cfg_context *ctx, const char *key, const char *value);
static char *cfg_getenv_expand (const char *variable);

bool cfg_open_and_parse (cfg_context *ctx)
{
  int rc = 0;

  if (ctx->current_level == 0)
  {
    mg_file_path path;

    if (Modes.tests)
       g_dbg_level = 2;
    GetModuleFileNameA (NULL, path, sizeof(path));
    snprintf (g_our_dir, sizeof(g_our_dir), "%s\\", dirname(path));
    TRACE (1, "g_our_dir: '%s'", g_our_dir);
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
  if (Modes.tests)
  {
    mg_host_name  ipv4_addr, ipv6_addr;

    mg_snprintf (ipv4_addr, sizeof(ipv4_addr), "%M", mg_print_ip, &ipv4_test);
    mg_snprintf (ipv6_addr, sizeof(ipv6_addr), "%M", mg_print_ip, &ipv6_test);
    printf ("ipv4_test: %s\n", ipv4_addr);
    printf ("ipv6_test: %s\n", ipv6_addr);
  }
  return (rc > 0);
}

static void handle_message (cfg_context *ctx, const char *key, const char *value)
{
  printf ("level: %u, Message: '%s'\n", ctx->current_level, value);
  (void) ctx;
  (void) key;
}

static void handle_include (cfg_context *ctx, const char *key, const char *value)
{
  const char *new_file = value;
  struct stat st;
  bool   ignore = false;

  memset (&st, '\0', sizeof(st));

  if (*new_file != '?' && stat(new_file, &st) == 0 && ((st.st_mode) & _S_IFMT) != _S_IFREG)
  {
    CFG_WARN ("include-file \"%s\" is not a regular file", new_file);
    return;
  }

  if (*new_file == '?')
  {
    new_file++;
    if (stat(new_file, &st) == 0 && ((st.st_mode) & _S_IFMT) != _S_IFREG)
    {
      CFG_WARN ("Ignoring include-file \"%s\" not found", new_file);
      ignore = true;
    }
  }

  TRACE (1, "new_file \"%s\", ignore: %d", new_file, ignore);

  if (!ignore)
  {
    cfg_context new_ctx;

    memset (&new_ctx, '\0', sizeof(new_ctx));
    new_ctx.fname         = new_file;
    new_ctx.tab           = ctx->tab;
    new_ctx.current_level = ctx->current_level + 1;
    cfg_open_and_parse (&new_ctx);
  }
  (void) key;
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

     expanded = cfg_getenv_expand (value);

     if (cfg_parse_table(ctx, key, expanded ? expanded : value))
        rc += 1;
     else
     {
       cfg_context internal_ctx;

       memcpy (&internal_ctx, ctx, sizeof(internal_ctx));
       internal_ctx.tab = internals;
       rc += cfg_parse_table (&internal_ctx, key, expanded ? expanded : value);
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

    p = str_trim (buf);             /* remove leadings/trailing spaces */
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
  *value_p = ctx->current_val;
  return (true);
}

static const char *type_name (cfg_tab_types type)
{
  return (type == ARG_ATOI    ? "ATOI"    :
          type == ARG_ATOU8   ? "ATOU8"   :
          type == ARG_ATOU16  ? "ATOU16"  :
          type == ARG_ATOU32  ? "ATOU32"  :
          type == ARG_ATOIP4  ? "ATOIP4"  :
          type == ARG_ATOIP6  ? "ATOIP6"  :
          type == ARG_FUNC    ? "FUNC"    :
          type == ARG_STRDUP  ? "STRDUP"  :
          type == ARG_STRCPY  ? "STRCPY"  : "??");
}

#define RANGE_CHECK(val, low, high)                        \
        do {                                               \
          if (((val) < (low)) || ((val) > (high))) {       \
             CFG_WARN ("Value %ld exceed range [%d - %d]", \
                       val, low, high);                    \
             return (0);                                   \
          }                                                \
        } while (0)

/**
 * Parse and store a `ARG_ATOx` values.
 */
static int parse_and_set_value (const char *key, const char *value, void *arg, int size)
{
  long val = 0;
  bool ok;

  TRACE (2, "parsing key: '%s', value: '%s'", key, value);

  ok = (sscanf(value, "%10ld", &val) == 1);   /* "2147483647"; 10 digits */
  if (!ok)
  {
    CFG_WARN ("failed to match '%s' as decimal in '%s'", value, key);
    return (0);
  }

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
    default:
        *(int*) arg = val;
        break;
  }
  return (1);
}

static bool cfg_parse_table (cfg_context *ctx, const char *key, const char *value)
{
  struct mg_str    str4, str6;
  struct mg_addr   addr4, addr6;
  const cfg_table *tab;
  void            *arg;
  bool             rc = false;

  for (tab = ctx->tab; tab->key; tab++)
  {
    if (stricmp(key, tab->key))
       continue;

    arg = tab->arg_func;   /* storage or function to call */
    if (!arg)
    {
      CFG_WARN ("No storage for \"%s\", type %s (%d)",
                tab->key, type_name(tab->type), tab->type);
      break;
    }

    switch (tab->type)
    {
      case ARG_ATOI:
           rc = parse_and_set_value (key, value, arg, sizeof(int));
           break;

      case ARG_ATOU8:
           rc = parse_and_set_value (key, value, arg, sizeof(uint8_t));
           break;

      case ARG_ATOU16:
           rc = parse_and_set_value (key, value, arg, sizeof(uint16_t));
           break;

      case ARG_ATOU32:
           rc = parse_and_set_value (key, value, arg, sizeof(uint32_t));
           break;

      case ARG_ATOIP4:
           str4 = mg_str (value);
           if (!mg_aton(str4, &addr4) || addr4.is_ip6)
              CFG_WARN ("Illegal IPv4-address: '%s'", value);
           else
           {
             memcpy (arg, &addr4, sizeof(addr4));
             rc = true;
           }
           break;

      case ARG_ATOIP6:
           str6 = mg_str (value);
           if (!mg_aton(str6, &addr6) || !addr6.is_ip6)
              CFG_WARN ("Illegal IPv6-address: '%s'", value);
           else
           {
             memcpy (arg, &addr6, sizeof(addr6));
             rc = true;
           }
           break;

      case ARG_FUNC:
           (*(void(*)(const char*))arg) (value);
           rc = 1;
           break;

      case ARG_FUNC2:
           (*(cfg_callback*) arg) (ctx, key, value);
           rc = 1;
           break;

      case ARG_STRDUP:
           *(char**)arg = strdup (value);
           rc = true;
           break;

      case ARG_STRCPY:
           strncpy ((char*)arg, value, MAX_VALUE_LEN-1);
           rc = true;
           break;

      default:
           CFG_WARN ("Something wrong in %s(): '%s' = '%s'. type = %s (%d)",
                     __FUNCTION__, key, value, type_name(tab->type), tab->type);
           break;
    }
    TRACE (2, "ARG_%s, matched '%s' = '%s'", type_name(tab->type), tab->key, value);
    break;
  }
  return (rc);
}

/**
 * Returns the expanded version of an variable.
 *
 * \eg If `INCLUDE=c:\VC\include;%C_INCLUDE_PATH%` and
 *   + `C_INCLUDE_PATH=c:\MinGW\include`, the expansion returns
 *   + `c:\VC\include;c:\MinGW\include`.
 */
static char *cfg_getenv_expand (const char *variable)
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
      env = buf1;
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
