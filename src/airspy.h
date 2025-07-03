/**\file    airspy.h
 * \ingroup Samplers
 */
#pragma once

#include <stdint.h>

#define airspy_dev void

typedef void (*airspy_cb) (uint8_t *buf, uint32_t len, void *ctx);

extern int  airspy_init (const char *name, int index, airspy_dev **device);
extern int  airspy_exit (airspy_dev *device);
extern bool airspy_set_dll_name (const char *arg);
extern int  airspy_set_gain (airspy_dev *device, int gain);
extern int  airspy_cancel_async (airspy_dev *device);
extern int  airspy_read_async (airspy_dev *device,
                               airspy_cb   cb,
                               void        *ctx,
                               uint32_t     buf_num,
                               uint32_t     buf_len);

extern const char *airspy_strerror (int rc);
