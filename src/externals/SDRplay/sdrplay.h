/**\file    sdrplay.h
 * \ingroup Samplers
 */
#ifndef SDRPLAY_H
#define SDRPLAY_H

#include <stdint.h>

#define sdrplay_dev void

typedef void (*sdrplay_cb) (uint8_t *buf, uint32_t len, void *ctx);

extern int  sdrplay_init (const char *name, int index, sdrplay_dev **device);
extern int  sdrplay_exit (sdrplay_dev *device);
extern bool sdrplay_set_adsb_mode (const char *arg);
extern bool sdrplay_set_dll_name (const char *arg);
extern bool sdrplay_set_minver (const char *arg);
extern int  sdrplay_set_gain (sdrplay_dev *device, int gain);
extern int  sdrplay_cancel_async (sdrplay_dev *device);
extern int  sdrplay_read_async (sdrplay_dev *device,
                                sdrplay_cb   cb,
                                void        *ctx,
                                uint32_t     buf_num,
                                uint32_t     buf_len);

extern const char *sdrplay_strerror (int rc);

#endif /* SDRPLAY_H */
