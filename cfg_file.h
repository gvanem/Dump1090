#pragma once

#include "misc.h"

typedef enum cfg_tab_types {
        ARG_ATOI,          /**< convert to int */
        ARG_ATO_U8,        /**< convert to 8-bit  'uint8_t' */
        ARG_ATO_U16,       /**< convert to 16-bit 'uint16_t */
        ARG_ATO_U32,       /**< convert to 32-bit 'uint32_t */
        ARG_ATO_U64,       /**< convert to 64-bit 'uint64_t */
        ARG_ATO_IP4,       /**< convert to a IPv4 `struct mg_addr` */
        ARG_ATO_IP6,       /**< convert to a IPv6 `struct mg_addr` */
        ARG_STRDUP,        /**< duplicate string value */
        ARG_STRCPY,        /**< copy string value */
        ARG_FUNC1,         /**< call function matching `cfg_callback1` */
        ARG_FUNC2          /**< call function matching `cfg_callback2` */
      } cfg_tab_types;

typedef struct cfg_table {
        const char    *key;
        cfg_tab_types  type;
        void          *arg_func;
      } cfg_table;

typedef struct cfg_context {
       const char      *fname;
       const cfg_table *tab;
       FILE            *file;
       bool             internal;
       unsigned         test_level;
       mg_file_path     current_file;
       unsigned         current_line;
       unsigned         current_level;
       char             current_key [256];
       char             current_val [512];
     } cfg_context;

typedef bool (*cfg_callback1) (const char *value);
typedef bool (*cfg_callback2) (cfg_context *ctx,
                               const char  *key,
                               const char  *value);

bool cfg_open_and_parse (cfg_context *ctx);
bool cfg_true           (const char *arg);

