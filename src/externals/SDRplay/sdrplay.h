/**\file    sdrplay.h
 * \ingroup Samplers
 */
#ifndef SDRPLAY_H
#define SDRPLAY_H

#include <stdint.h>

#define sdrplay_dev void

typedef void (*sdrplay_cb) (uint8_t *buf, uint32_t len, void *ctx);

int  sdrplay_init (const char *name, int index, sdrplay_dev **device);
int  sdrplay_exit (sdrplay_dev *device);
bool sdrplay_set_adsb_mode (const char *arg);
bool sdrplay_set_if_mode (const char *arg);
bool sdrplay_set_antenna (const char *arg);
bool sdrplay_set_dll_name (const char *arg);
bool sdrplay_set_minver (const char *arg);
bool sdrplay_set_tuner (const char *arg);
bool sdrplay_set_USB_bulk_mode (const char *arg);
bool sdrplay_set_decay_filter (const char *arg);
int  sdrplay_set_gain (sdrplay_dev *device, int gain);
int  sdrplay_get_gain (sdrplay_dev *device, int *gain);
int  sdrplay_cancel_async (sdrplay_dev *device);
int  sdrplay_read_async (sdrplay_dev *device,
                         sdrplay_cb   cb,
                         void        *ctx,
                         uint32_t     buf_num,
                         uint32_t     buf_len);

const char *sdrplay_strerror (int rc);

/*
 * for debug
 */
void sdrplay_dump_mag_vector (const uint16_t *m, uint16_t len);

#endif /* SDRPLAY_H */
