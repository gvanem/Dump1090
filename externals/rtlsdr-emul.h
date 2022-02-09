/**\file    rtlsdr-emul.h
 * \ingroup Misc
 *
 * RTLSDR emulator interface for SDRplay.
 */
#ifndef RTLSDR_EMUL_H
#define RTLSDR_EMUL_H

extern bool RTLSDR_emul_load_DLL (void);
extern bool RTLSDR_emul_unload_DLL (void);

struct RTLSDR_emul {
       const char  *dll_name;
       HANDLE       dll_hnd;
       char         last_err [256];
  //   int          last_rc;

        const char *(*rtlsdr_strerror) (int rc);
        int         (*rtlsdr_open) (rtlsdr_dev_t **dev, int index);
        int         (*rtlsdr_close) (rtlsdr_dev_t *dev);
        int         (*rtlsdr_cancel_async)   (rtlsdr_dev_t *dev);
        int         (*rtlsdr_set_tuner_gain) (rtlsdr_dev_t *dev, int gain);
        int         (*rtlsdr_read_async)     (rtlsdr_dev_t *dev,
                                              rtlsdr_read_async_cb_t cb,
                                              void    *ctx,
                                              uint32_t buf_num,
                                              uint32_t buf_len);
      };

#ifndef RTLSDR_EMUL_CONST
#define RTLSDR_EMUL_CONST const
#endif

extern RTLSDR_EMUL_CONST struct RTLSDR_emul emul;

#endif /* RTLSDR_EMUL_H */

