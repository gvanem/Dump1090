/**\file    csv.h
 * \ingroup Misc
 */
#ifndef _CSV_H
#define _CSV_H

/**
 * The CSV-parser states.
 */
typedef enum CSV_STATE {
        STATE_ILLEGAL = 0,  /**< Illegal parse state. */
        STATE_NORMAL,       /**< The normal state. */
        STATE_QUOTED,       /**< We are parsing a quoted string. */
        STATE_ESCAPED,      /**< In STATE_QUOTED we got an ESC character. */
        STATE_COMMENT,      /**< Got a `#` in field 0. */
        STATE_STOP,         /**< We reached the end-of-record. */
        STATE_EOF,          /**< We reached the end-of-file. */
      } CSV_STATE;

struct CSV_context;

/**
 * The typedef for the CSV-parser state-functions.
 */
typedef void (*csv_state_t) (struct CSV_context *ctx);

/**
 * Keep all data used for CSV parsing in this context.
 */
typedef struct CSV_context {
        const char *file_name;      /**< The .csv-file we opened. */
        FILE       *file;           /**< The `FILE*`. */
        unsigned    field_num;      /**< The current field we're in. */
        unsigned    num_fields;     /**< Number of fields in a record (line). Autodetected or specified. */
        int         delimiter;      /**< The delimiter for each field. */
        unsigned    rec_num;        /**< The record number so far. */
        unsigned    rec_max;        /**< The maximum number of records to parse. Infinited == `UINT_MAX`. */
        unsigned    line_size;      /**< The maximum size of a file-line. */
        char       *parse_buf;      /**< The malloced parse-buffer. */
        char       *parse_ptr;      /**< The parse position within the parse-buffer. */
        CSV_STATE   state;          /**< The current CSV-parser state. */
        csv_state_t state_func;     /**< The current parse function set by CSV_context::state. */
        int         c_in;           /**< The current character read from CSV_context::parse_buf. */

        /** The user callback for adding records.
         */
        int (*callback) (struct CSV_context *ctx, const char *value);
      } CSV_context;

int CSV_open_and_parse_file (struct CSV_context *ctx);

#endif /* _CSV_H */
