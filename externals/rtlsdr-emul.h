/**\file    rtlsdr-emul.h
 * \ingroup Misc
 *
 * Various RTLSDR emulator interface for SDRPlay.
 */
#ifndef RTLSDR_EMUL_H
#define RTLSDR_EMUL_H

extern int RTLSDR_emul_load_DLL (void);

typedef struct RTLSDR_emul {
  const char *(*rtlsdr_strerror) (int rc);
  int         (*rtlsdr_open) (rtlsdr_dev_t **dev, int index);
  int         (*rtlsdr_close) (rtlsdr_dev_t *dev);
  int         (*rtlsdr_cancel_async)   (rtlsdr_dev_t *dev);
  int         (*rtlsdr_set_tuner_gain) (rtlsdr_dev_t *dev, int gain);
  int         (*rtlsdr_read_async)     (rtlsdr_dev_t *dev,
                                        rtlsdr_read_async_cb_t cb,
                                        void  *ctx,
                                        uint32_t buf_num,
                                        uint32_t buf_len);

} RTLSDR_emul;

extern struct RTLSDR_emul emul;

#endif /* RTLSDR_EMUL_H */

