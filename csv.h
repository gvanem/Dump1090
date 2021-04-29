/**\file    csv.h
 * \ingroup Misc
 */
#ifndef _CSV_H
#define _CSV_H

/**
 * \enum The CSV-parser states
 */
typedef enum CSV_STATE {
        STATE_ILLEGAL = 0,
        STATE_NORMAL,
        STATE_QUOTED,
        STATE_ESCAPED,
        STATE_COMMENT,
        STATE_STOP,
        STATE_EOF,
      } CSV_STATE;

struct CSV_context;

/**
 * \typedef The CSV-parser state-functions matches this:
 */
typedef void (*csv_state_t) (struct CSV_context *ctx);

/**
 * \typedef CSV_context
 * Keep all data used for CSV parsing in this context.
 */
typedef struct CSV_context {
        const char *file_name;
        FILE       *file;
        unsigned    field_num;
        unsigned    num_fields;
        int         delimiter;
        int       (*callback) (struct CSV_context *ctx, const char *value);
        unsigned    rec_num;
        unsigned    rec_max;
        unsigned    line_size;
        char       *parse_buf, *parse_ptr;
        csv_state_t state_func;
        CSV_STATE   state;
        int         c_in;
      } CSV_context;

int CSV_open_and_parse_file (struct CSV_context *ctx);

#endif /* _CSV_H */
