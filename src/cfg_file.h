/**\file    cfg_file.h
 * \ingroup Misc
 * \brief   Config-file handling.
 */
#pragma once

#include <stdio.h>
#include <stdbool.h>

typedef enum cfg_tab_types {
        ARG_ATOB,          /**< convert to `bool` */
        ARG_ATOI,          /**< convert to `int` */
        ARG_ATO_U8,        /**< convert to 8-bit  `uint8_t` */
        ARG_ATO_U16,       /**< convert to 16-bit `uint16_t` */
        ARG_ATO_U32,       /**< convert to 32-bit `uint32_t` */
        ARG_ATO_U64,       /**< convert to 64-bit `uint64_t` */
        ARG_ATO_IP4,       /**< convert to a IPv4 `struct mg_addr` */
        ARG_ATO_IP6,       /**< convert to a IPv6 `struct mg_addr` */
        ARG_STRDUP,        /**< duplicate string value */
        ARG_STRCPY,        /**< copy string value */
        ARG_FUNC,          /**< call function matching `cfg_callback` */
      } cfg_tab_types;

typedef struct cfg_table {
        const char    *key;
        cfg_tab_types  type;
        void          *arg_func;
      } cfg_table;

typedef bool (*cfg_callback) (const char *value);

bool     cfg_open_and_parse (const char *fname, const cfg_table *cfg);
bool     cfg_true           (const char *arg);
char    *cfg_current_file   (void);
unsigned cfg_current_line   (void);

