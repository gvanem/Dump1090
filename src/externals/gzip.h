/**\file    gzip.h
 * \ingroup Misc
 * \brief   GZIP decompression
 */
#pragma once

typedef int (*gzip_cb) (const char *filename, void *arg);

int gzip_extract (const char *gzip_name, const char *dir,
                  gzip_cb extract_cb, void *cb_arg);
