/*
 * A simple config-file that gets force-included for
 * ALL .c-files in Dump1090. Option '-FI./dump1090_config.h'.
 */
#ifndef DUMP1090_CONFIG_H
#define DUMP1090_CONFIG_H

/* Warning control:
 */
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-value"
  #pragma clang diagnostic ignored "-Wunused-variable"
  #pragma clang diagnostic ignored "-Wmacro-redefined"
  #pragma clang diagnostic ignored "-Wignored-attributes"
  #pragma clang diagnostic ignored "-Wignored-pragma-optimize"
#else
  #pragma warning (disable:4005 4244)
#endif

#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#define _CRT_SECURE_NO_WARNINGS         1
#define _CRT_SECURE_NO_DEPRECATE        1
#define _CRT_NONSTDC_NO_WARNINGS        1

#define _USE_MATH_DEFINES 1  /* To pull in 'M_PI' in <math.h> */
#define STDIN_FILENO      0

#if defined(_DEBUG)
  #include <malloc.h>
  #include <string.h>

  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>
#endif

#if defined(USE_VLD)
  /*
   * Visual Leak Detector is mostly useful in a '_DEBUG' ('-MDd' or '-MTd') build.
   * But including it for '_RELEASE' ('-MD' or '-MT') works too.
   *
   * Refs:
   *   https://kinddragon.github.io/vld/
   *   https://github.com/KindDragon/vld/wiki
   */
  #include <vld.h>
#endif

#endif /* DUMP1090_CONFIG_H */
