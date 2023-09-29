#pragma once

#include "misc.h"

typedef enum cfg_tab_types {
        ARG_ATOI,          /**< convert to int */
        ARG_ATOU8,         /**< convert to 8-bit  'uint8_t' */
        ARG_ATOU16,        /**< convert to 16-bit 'uint16_t */
        ARG_ATOU32,        /**< convert to 32-bit 'uint32_t */
        ARG_ATOIP4,        /**< convert to a IPv4 `struct mg_addr` */
        ARG_ATOIP6,        /**< convert to a IPv6 `struct mg_addr` */
        ARG_STRDUP,        /**< duplicate string value */
        ARG_STRCPY,        /**< copy string value */
        ARG_FUNC           /**< call function */
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
       mg_file_path     current_file;
       unsigned         current_line;
       char             current_key [256];
       char             current_val [512];
     } cfg_context;

bool cfg_open_and_parse (cfg_context *ctx);

