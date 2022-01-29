/**\file    sdrplay.h
 * \ingroup Main
 */
#ifndef SDRPLAY_LOADER_H
#define SDRPLAY_LOADER_H

#include <stdint.h>

#define sdrplay_dev void

typedef void (*sdrplay_cb) (uint8_t *buf, uint32_t len, void *ctx);

#if defined(USE_RTLSDR_EMUL)
  #include "rtlsdr-emul.h"

  #define sdrplay_init(name, dev_p)                          (*emul.rtlsdr_open) ((rtlsdr_dev_t**)dev_p, 0)
  #define sdrplay_exit(dev)                                  (*emul.rtlsdr_close) (dev)
  #define sdrplay_set_gain(dev, gain)                        (*emul.rtlsdr_set_tuner_gain) (dev, gain)
  #define sdrplay_cancel_async(dev)                          (*emul.rtlsdr_cancel_async) (dev)
  #define sdrplay_read_async(dev, cb, ctx, buf_num, buf_len) (*emul.rtlsdr_read_async) (dev, cb, ctx, buf_num, buf_len)
  #define sdrplay_strerror(rc)                               (*emul.rtlsdr_strerror) (rc)

#else
  extern int sdrplay_init (const char *name, sdrplay_dev **device);
  extern int sdrplay_exit (sdrplay_dev *device);
  extern int sdrplay_set_gain (sdrplay_dev *device, int gain);
  extern int sdrplay_cancel_async (sdrplay_dev *device);
  extern int sdrplay_read_async (sdrplay_dev *device,
                                 sdrplay_cb   cb,
                                 void        *ctx,
                                 uint32_t     buf_num,
                                 uint32_t     buf_len);

  extern const char *sdrplay_strerror (int rc);

#endif /* USE_RTLSDR_EMUL */
#endif /* SDRPLAY_LOADER_H */
