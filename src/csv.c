/**\file    csv.c
 * \ingroup Misc
 *
 * \brief Implements a generic parser for CSV files.
 *
 * The parsing is loosely adapting the rules in: https://tools.ietf.org/html/rfc4180
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include "csv.h"

/**
 * The default size of a CSV_context::parse_buf.
 */
#define DEFAULT_BUF_SIZE 1000

/**
 * Macro for getting a character into the CSV_context::parse_buf.
 */
#define PUTC(c)  do {                                                    \
                   if (ctx->parse_ptr < ctx->parse_buf + ctx->line_size) \
                      *ctx->parse_ptr++ = c;                             \
                 } while (0)

/**
 * A simple state-machine for parsing CSV records.
 *
 * The parser starts in this state.
 */
static void state_normal (struct CSV_context *ctx)
{
  if (ctx->c_in == ctx->delimiter)
  {
    ctx->state = STATE_STOP;
    return;
  }

  switch (ctx->c_in)
  {
    case -1:
         ctx->state = STATE_EOF;
         break;
    case '"':
         ctx->state = STATE_QUOTED;
         break;
    case '\r':     /* ignore */
         break;
    case '\n':
         if (ctx->field_num > 0)     /* If field == 0, ignore empty lines */
            ctx->state = STATE_STOP;
         break;
    case '#':
         if (ctx->field_num == 0)    /* If field == 0, ignore comment lines */
              ctx->state = STATE_COMMENT;
         else PUTC (ctx->c_in);
         break;
    default:
         PUTC (ctx->c_in);
         break;
  }
}

/**
 * If the parser find a quote (`"`) in `state_normal()`, it enters this state
 * to find the end of the quote. Ignoring escaped quotes (i.e. a `\\"`).
 */
static void state_quoted (struct CSV_context *ctx)
{
  switch (ctx->c_in)
  {
    case -1:
         ctx->state = STATE_EOF;
         break;
    case '"':
         ctx->state = STATE_NORMAL;
         break;
    case '\r':     /* ignore, but should not occur since `fopen (file, "rt")` was used */
         break;
    case '\n':     /* add a space in this field */
         PUTC (' ');
         break;
    case '\\':
         ctx->state = STATE_ESCAPED;
         break;
    default:
         PUTC (ctx->c_in);
         break;
  }
}

/**
 * Look for an escaped quote. <br>
 * Go back to `state_quoted()` when found.
 */
static void state_escaped (struct CSV_context *ctx)
{
  switch (ctx->c_in)
  {
    case -1:
         ctx->state = STATE_EOF;
         break;
    case '"':       /* '\"' -> '"' */
         PUTC ('"');
         ctx->state = STATE_QUOTED;
         break;
    case '\r':
    case '\n':
         break;
    default:
         ctx->state = STATE_QUOTED; /* Unsupported ctrl-char. Go back */
         break;
  }
}

/**
 * Do nothing until a newline. <br>
 * Go back to `state_normal()` when found.
 */
static void state_comment (struct CSV_context *ctx)
{
  switch (ctx->c_in)
  {
    case -1:
         ctx->state = STATE_EOF;
         break;
    case '\n':
         ctx->state = STATE_NORMAL;
         break;
  }
}

static void state_illegal (struct CSV_context *ctx)
{
  ctx->state = STATE_EOF;
}

/**
 * Read from `ctx->file` until end-of-field.
 */
static const char *CSV_get_next_field (struct CSV_context *ctx)
{
  char     *ret;
  CSV_STATE new_state = STATE_ILLEGAL;

  ctx->parse_ptr = ctx->parse_buf;

  while (1)
  {
    ctx->c_in = fgetc (ctx->file);

    (*ctx->state_func) (ctx);
    new_state = ctx->state;

    /* Set new state for this context. (Or stay in same state).
     */
    switch (new_state)
    {
      case STATE_NORMAL:
           ctx->state_func = state_normal;
           break;
      case STATE_QUOTED:
           ctx->state_func = state_quoted;
           break;
      case STATE_ESCAPED:
           ctx->state_func = state_escaped;
           break;
      case STATE_COMMENT:
           ctx->state_func = state_comment;
           break;
      case STATE_ILLEGAL:   /* Avoid compiler warning */
      case STATE_STOP:
      case STATE_EOF:
           break;
    }
    if (new_state == STATE_STOP && ctx->delimiter == ' ')
    {
      while ((ctx->c_in = fgetc (ctx->file)) == ' ') ;
      ungetc (ctx->c_in, ctx->file);
    }

    if (new_state == STATE_STOP || new_state == STATE_EOF)
       break;
  }

  *ctx->parse_ptr = '\0';
  ret = ctx->parse_buf;
  if (new_state == STATE_EOF)
     return (NULL);
  return (ret);
}

/**
 * Open and parse CSV file and extract one record by calling the callback for each found field.
 *
 * \param[in]  ctx  the CSV context to work with.
 * \retval     the number of CSV-records that could be parsed.
 */
static int CSV_parse_file (struct CSV_context *ctx)
{
  for (ctx->field_num = 0; ctx->field_num < ctx->num_fields; ctx->field_num++)
  {
    const char *val;
    int         rc;

    ctx->state = STATE_NORMAL;
    ctx->state_func = state_normal;

    val = CSV_get_next_field (ctx);
    if (!val)
       return (0);

    rc = (*ctx->callback) (ctx, val);
    if (!rc)
       break;
  }
  ctx->rec_num++;
  return (ctx->field_num == ctx->num_fields);
}

/**
 * Try to auto-detect the number of fields in the CSV-file.
 *
 * Open and parse the first line and count the number of delimiters.
 * If this line ends in a newline, this should count as the last field.
 * Hence increment by 1.
 *
 * \param[in]  ctx  the CSV context to work with.
 * \retval     0 on failure. 1 on success.
 */
static int CSV_autodetect_num_fields (struct CSV_context *ctx)
{
  unsigned num_fields = 0;
  const char *delim, *next;

  ctx->file = fopen (ctx->file_name, "rt");
  if (!ctx->file)
     return (0);

  if (!fgets(ctx->parse_buf, ctx->line_size, ctx->file))
  {
    fclose (ctx->file);
    return (0);
  }

  delim = ctx->parse_buf;
  while (*delim)
  {
    next = strchr (delim, ctx->delimiter);
    if (!next)
    {
      if (strchr(delim, '\r') || strchr(delim, '\n'))
         num_fields++;
      break;
    }
    delim = next + 1;
    num_fields++;
  }
  ctx->num_fields = num_fields;
  fseek (ctx->file, 0, SEEK_SET);
  return (1);
}

/**
 * Check for unset members of the CSV-context. <br>
 * Set the field-delimiter to `,` if not already done.
 *
 * \param[in]  ctx  the CSV context to work with.
 * \retval     1 if the members are okay.
 * \retval    -1 if some members are not okay etc.
 */
static int CSV_check_and_fill_ctx (struct CSV_context *ctx)
{
  if (!ctx->callback || !ctx->file_name)
  {
    errno = EINVAL;
    return (0);
  }

  if (!ctx->delimiter)
     ctx->delimiter = ',';

  if (strchr("#\"\r\n", ctx->delimiter))
  {
    errno = EINVAL;
    return (0);
  }

  if (ctx->rec_max == 0)
     ctx->rec_max = UINT_MAX;

  if (ctx->line_size == 0)
     ctx->line_size = DEFAULT_BUF_SIZE;

  ctx->parse_buf = malloc (ctx->line_size+1);
  if (!ctx->parse_buf)
     return (0);

  if (ctx->num_fields == 0 && !CSV_autodetect_num_fields(ctx))
  {
    free (ctx->parse_buf);
    errno = EINVAL;
    return (0);
  }

  if (!ctx->file)
     ctx->file = fopen (ctx->file_name, "rt");

  if (!ctx->file)
  {
    free (ctx->parse_buf);
    return (0);
  }

  setvbuf (ctx->file, NULL, _IOFBF, 100*ctx->line_size);
  ctx->state_func = state_illegal;
  ctx->state      = STATE_ILLEGAL;
  ctx->rec_num    = 0;
  return (1);
}

/**
 * Open and parse a CSV-file.
 */
int CSV_open_and_parse_file (struct CSV_context *ctx)
{
  if (!CSV_check_and_fill_ctx(ctx))
     return (0);

  while (1)
  {
    if (!CSV_parse_file(ctx) || ctx->rec_num >= ctx->rec_max)
       break;
  }
  fclose (ctx->file);
  ctx->file = NULL;
  free (ctx->parse_buf);
  return (ctx->rec_num);
}

