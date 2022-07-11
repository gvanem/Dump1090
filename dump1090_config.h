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

#if !defined(RC_INVOKED)  /* Rest of file */

#define rtlsdr_STATIC    1
#define USE_gettimeofday 1

#define HAVE_rtlsdr_cal_imr

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT 0x0600
#endif

/* Warning control:
 */
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-value"
  #pragma clang diagnostic ignored "-Wunused-variable"
  #pragma clang diagnostic ignored "-Wmacro-redefined"
  #pragma clang diagnostic ignored "-Wignored-attributes"
  #pragma clang diagnostic ignored "-Wignored-pragma-optimize"
  #pragma clang diagnostic ignored "-Wmissing-field-initializers"

#elif defined(_MSC_VER)
  #pragma warning (disable:4005 4244 4267)

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

#define _USE_MATH_DEFINES 1  /* To pull in 'M_PI' in <math.h> */
#define MG_ENABLE_IPV6    1  /* Enable IPv6 in 'externals/mongoose.c' */
#define MG_ENABLE_FILE    1  /* Enable logging to 'stdout' in 'externals/mongoose.c' */
#define MG_ENABLE_POLL    1  /* Enable 'WSAPoll()' in 'externals/mongoose.c'. */

#if defined(_DEBUG)
  #include <malloc.h>
  #include <string.h>

  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>
#endif

#endif  /* RC_INVOKED */
#endif  /* DUMP1090_CONFIG_H */
