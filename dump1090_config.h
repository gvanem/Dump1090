/*
 * A simple config-file that gets force-included for
 * ALL .c-files in Dump1090.
 * Ref. option '-FI./dump1090_config.h'.
 */
#ifndef DUMP1090_CONFIG_H
#define DUMP1090_CONFIG_H

#define VER_MAJOR 0
#define VER_MINOR 2
#define VER_MICRO 0

/* Warning control:
 */
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-value"
  #pragma clang diagnostic ignored "-Wunused-variable"
  #pragma clang diagnostic ignored "-Wunused-function"
  #pragma clang diagnostic ignored "-Wignored-attributes"
  #pragma clang diagnostic ignored "-Wignored-pragma-optimize"
  #pragma clang diagnostic ignored "-Wmissing-field-initializers"

#elif defined(_MSC_VER)
  #pragma warning (disable:4005 4244 4267)

  /*
   * misc.c(524): warning C4152: nonstandard extension,
   *  function/data pointer conversion in expression
   */
  #pragma warning (disable:4152)

  #ifdef _WIN64
    /*
     * 'type cast': conversion from 'int' to 'void *' of greater size
    */
    #pragma warning (disable:4312)
  #endif
#endif

#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#define _CRT_SECURE_NO_WARNINGS         1
#define _CRT_SECURE_NO_DEPRECATE        1
#define _CRT_NONSTDC_NO_WARNINGS        1

#define _STR2(x)  #x
#define _STR(x)   _STR2(x)

#define PROG_VERSION  _STR(VER_MAJOR) "." _STR(VER_MINOR) "." _STR(VER_MICRO)

#define rtlsdr_STATIC    1
#define USE_gettimeofday 1
#define HAVE_rtlsdr_cal_imr

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT 0x0600
#endif

#define _USE_MATH_DEFINES 1  /* To pull in 'M_PI' in <math.h> */

/* Support various features in 'externals/mongoose.c':
 */
#define MG_ENABLE_IPV6          0  /* IPv6 */
#define MG_ENABLE_FILE          1  /* 'opendir()' etc. */
#define MG_ENABLE_POLL          0  /* 'WSAPoll()' over 'select()' */
#define MG_ENABLE_DIRLIST       0  /* listing directories for HTTP */
#define MG_ENABLE_CUSTOM_MILLIS 1  /* use 64-bit tick-time */

/* Use "Visual Leak Detector"
 */
#if defined(USE_VLD)
#include <vld.h>
#endif

#include <string.h>

/* Avoid the dependency on 'oldnames.lib'
 */
#define stricmp(s1, s2) _stricmp (s1, s2)
#define strdup(s)       _strdup (s)

#if defined(_DEBUG)
  #include <malloc.h>

  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>
#endif

#endif  /* DUMP1090_CONFIG_H */
