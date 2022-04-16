/**\file    rtlsdr-emul.c
 * \ingroup Misc
 *
 * RTLSDR emulator interface for SDRplay.
 */
#if defined(USE_RTLSDR_EMUL)  /* rest of file */

/*
 * \def RTLSDR_EMUL_CONST
 * Make `struct RTLSDR_emul emul` const-data from the user-side.
 */
#define RTLSDR_EMUL_CONST

#include "misc.h"
#include "rtlsdr-emul.h"

/**
 * This .dll MUST be on PATH or in current working directory.
 */
#ifdef _WIN64
  struct RTLSDR_emul emul = { "rtlsdr-emul-x64.dll" };
#else
  struct RTLSDR_emul emul = { "rtlsdr-emul-x86.dll" };
#endif

/**
 * \def LOAD_FUNC(func)
 *   A `GetProcAddress()` helper.
  *  \param func  the name of the function (without any `"`).
 */
#define LOAD_FUNC(func)                                                     \
        do {                                                                \
          emul.func = (int (*)()) GetProcAddress (emul.dll_hnd, #func);     \
          if (!emul.func)                                                   \
          {                                                                 \
            snprintf (emul.last_err, sizeof(emul.last_err),                 \
                      "Failed to find '%s()' in %s", #func, emul.dll_name); \
            emul.last_rc = ERROR_PROC_NOT_FOUND;                            \
            goto failed;                                                    \
          }                                                                 \
          TRACE (DEBUG_GENERAL2, "Function: %-30s -> 0x%p.\n",              \
                 #func, emul.func); \
        } while (0)

static const char *rtlsdr_emul_strerror (int rc)
{
  static char buf[30];

  snprintf (buf, sizeof(buf), "Emul-err: %d", rc);
  return (buf);
}

bool RTLSDR_emul_load_DLL (void)
{
  emul.rtlsdr_strerror = rtlsdr_emul_strerror;
  emul.dll_hnd = LoadLibrary (emul.dll_name);

  if (!emul.dll_hnd)
  {
    emul.last_rc = GetLastError();

    /* The 'LoadLibrary()' will fail with 'GetLastError() ==  ERROR_BAD_EXE_FORMAT' (193)
     * if we're running a 32-bit Dump1090 and loaded a 64-bit "rtlsdr-emul-x64.dll".
     * And vice-versa.
     */
    if (emul.last_rc == ERROR_BAD_EXE_FORMAT)
         snprintf (emul.last_err, sizeof(emul.last_err), "%s is not a %d bit version", emul.dll_name, 8*(int)sizeof(void*));
    else snprintf (emul.last_err, sizeof(emul.last_err), "Failed to load %s; %lu", emul.dll_name, emul.last_rc);
    TRACE (DEBUG_GENERAL, "emul.dll_hnd: NULL. error: %s (%lu)\n", emul.last_err, emul.last_rc);
    return (false);
  }

  emul.last_rc = 0;  /* assume success */
  TRACE (DEBUG_GENERAL2, "emul.dll_name: %s, emul.dll_hnd: 0x%p.\n", emul.dll_name, emul.dll_hnd);

  /*
   * Turn off this annoying cast warnings:
   * int (__cdecl *)()' differs in parameter lists from 'int (__cdecl *)(rtlsdr_dev_t **,int)
   */
#if defined(__clang__) && (__clang_major__ >= 13)
  #pragma clang diagnostic ignored "-Wincompatible-function-pointer-types"
#elif defined(_MSC_VER)
  #pragma warning (disable:4113)
#endif

  LOAD_FUNC (rtlsdr_open);
  LOAD_FUNC (rtlsdr_close);
  LOAD_FUNC (rtlsdr_cancel_async);
  LOAD_FUNC (rtlsdr_set_tuner_gain);
  LOAD_FUNC (rtlsdr_read_async);

  return (true);

failed:
  return (false);
}

bool RTLSDR_emul_unload_DLL (void)
{
  if (emul.dll_hnd && emul.dll_hnd != INVALID_HANDLE_VALUE)
     FreeLibrary (emul.dll_hnd);
  emul.dll_hnd = INVALID_HANDLE_VALUE;
  return (true);
}

#else
int rtlsdr_emul_dummy;
#endif  /* USE_RTLSDR_EMUL */
