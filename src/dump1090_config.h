/**
 * A simple config-file that gets force-included for *all*
 * .c-files in Dump1090.
 * Ref. option `-FI./dump1090_config.h` in Makefile.Windows.
 */
#pragma once

#define VER_MAJOR 0
#define VER_MINOR 4
#define VER_MICRO 8

/* Warning control:
 */
#define BUILD_WINDOWS                   1
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#define _CRT_SECURE_NO_WARNINGS         1
#define _CRT_SECURE_NO_DEPRECATE        1
#define _CRT_NONSTDC_NO_WARNINGS        1

#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wignored-attributes"
  #pragma clang diagnostic ignored "-Wunused-value"
  #pragma clang diagnostic ignored "-Wunused-variable"
  #pragma clang diagnostic ignored "-Wunused-function"
  #pragma clang diagnostic ignored "-Wunused-label"
  #pragma clang diagnostic ignored "-Wunused-parameter"
  #pragma clang diagnostic ignored "-Wpragma-pack"
  #pragma clang diagnostic ignored "-Wsign-compare"
  #pragma clang diagnostic ignored "-Wmissing-field-initializers"
  #pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
  #pragma clang diagnostic ignored "-Wmicrosoft-enum-forward-reference"

  /*
   * Cause a compile-error for these warnings:
   */
  #pragma clang diagnostic error "-Wformat"
  #pragma clang diagnostic error "-Wformat-insufficient-args"

  #if defined(COMPILING_SQLITE3_SHELL)
    #pragma clang diagnostic ignored "-Wformat-security"
    #pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
  #endif

#elif defined(_MSC_VER)
 /*
  * airports.c(1502): warning C4102:
  *   'done': unreferenced label
  */
  #pragma warning (disable: 4102)

  /*
   * misc.c(524): warning C4152: nonstandard extension,
   *   function/data pointer conversion in expression
   */
  #pragma warning (disable: 4152)

  /*
   * windnsdef.h(1072,6): warning C4201:
   *   nonstandard extension used: nameless struct/union
   */
  #pragma warning (disable: 4201)

  /*
   * csv.c(60): warning C4244:
   *   conversion from 'int' to 'char', possible loss of data
   */
  #pragma warning (disable: 4244)

  /*
   * externals/mongoose.c(4482): warning C4267: 'function':
   *   conversion from 'size_t' to 'int', possible loss of data
   */
  #pragma warning (disable: 4267)

  /*
   * externals\miniz.h(6560): warning C4127: conditional expression is constant
   * externals\zip.c(224):    warning C4706: assignment within conditional expression
   */
  #pragma warning (disable: 4127 4706)

  /*
   * externals/mongoose.c(16208): warning C4310: cast truncates constant value
   * externals/mongoose.c(17251): warning C4310: cast truncates constant value
   */
  #pragma warning (disable: 4310)

  /*
   * warning C4702: unreachable code
   */
  #pragma warning (disable: 4702)

  #if defined(_WIN64)
    /*
     * 'type cast': conversion from 'int' to 'void *' of greater size
     */
    #pragma warning (disable: 4312)
  #endif

  #if defined(COMPILING_SQLITE3_SHELL)
    /*
     * warning C4100: 'argv': unreferenced formal parameter
     */
    #pragma warning (disable: 4100)

    /*
     * warning C4456: declaration of 'i' hides previous local declaration
     */
    #pragma warning (disable: 4456)

    /*
     * warning C4457: declaration of 'zLine' hides function parameter
     */
    #pragma warning (disable: 4457)

    /*
     * warning C4459: declaration of 'db' hides global declaration
     */
    #pragma warning (disable: 4459)
  #endif
#endif

#define _STR2(x)  #x
#define _STR(x)   _STR2(x)

#define PROG_VERSION  _STR(VER_MAJOR) "." _STR(VER_MINOR) "." _STR(VER_MICRO)

/** Do not add `__declspec(dllexport)` on `externals/rtl-sdr/` functions.
 */
#define rtlsdr_STATIC    1

/** Prefer `_gettimeofday` over `GetTickCount64()`.
 */
#define USE_gettimeofday 1

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0602)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT 0x0602   /* == _WIN32_WINNT_WIN8 */
#endif

/** To pull in `M_PI` in `<math.h>`
 */
#define _USE_MATH_DEFINES 1

/** Support various features in `externals/mongoose.c`:
 */
#define MG_ENABLE_ASSERT        1  /* Enable `assert()` calls */
#define MG_ENABLE_IPV6          1  /* Enable IPv6 */
#define MG_ENABLE_MD5           0  /* No need for MD5 code */
#define MG_ENABLE_FILE          1  /* For `opendir()` etc. */
#define MG_ENABLE_DIRLIST       0  /* No need for directory listings in HTTP */
#define MG_ENABLE_CUSTOM_MILLIS 1  /* Enable 64-bit tick-time */
#define MG_TLS                  3  /* Enable MG_TLS_BUILTIN code */
#define MG_TLS_SSLKEYLOGFILE    1  /* Enable logging to `$(SSLKEYLOGFILE)` */

#define _CRT_RAND_S             1  /* To pull in 'rand_s()' */

/** Drop some stuff not needed in `externals/zip.c`:
 */
#define MINIZ_NO_ZLIB_APIS      1

/**
 * clang-cl with `ASAN` may not like this in `externals/miniz.h`:
 * ```
 * #define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 1
 * ```
 */
#if defined(USE_ASAN)
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <io.h>

/**
 * Avoid the dependency on `oldnames.lib`:
 */
#define stricmp(s1, s2)      _stricmp (s1, s2)
#define strnicmp(s1, s2, sz) _strnicmp (s1, s2, sz)
#define strlwr(s)            _strlwr (s)
#define strupr(s)            _strupr (s)
#define access(file, mode)   _access (file, mode)
#define fileno(stream)       _fileno (stream)

/**
 * Options for `_DEBUG` / `-MDd` mode:
 */
#if defined(_DEBUG)
  #include <malloc.h>

  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>
#else
  /**
   * Drop the dependency on 'oldnames.lib'
   */
  #define strdup(s)  _strdup (s)
#endif

/**
 * Showing airport names correctly in `--interactive' mode needs
 * Wide-string and UTF8 in PDCurses.
 */
#define PDC_WIDE       1
#define PDC_FORCE_UTF8 1

/**
 * Options for `externals/sqlite3.c`:
 */
#define SQLITE_API
#define SQLITE_DQS               3   /* Double-quoted string literals are allowed */
#define SQLITE_THREADSAFE        0
#define SQLITE_WIN32_MALLOC      1
#define SQLITE_NO_SYNC           1
#define SQLITE_OMIT_AUTOINIT     1
#define SQLITE_DEFAULT_MEMSTATUS 0

/**
 * For `net_io.c` and the "Packed Web FileSystem":
 */
#if defined(USE_PACKED_DLL)
  #undef  MG_ENABLE_PACKED_FS
  #define MG_ENABLE_PACKED_FS 1
#endif

/**
 * For `interactive.c` and `dump1090.rc`
 */
#define IDI_APPICON 100

/**
 * Common stuff for compiling .rc files
 */
#if defined(RC_INVOKED)
  #if defined(__clang__)
    #define RC_BUILDER  "clang-cl"
  #else
    #define RC_BUILDER  "MSVC"
  #endif

  #if defined(_DEBUG)
    #define RC_DBG_REL    "debug"
    #define RC_FILEFLAGS  1
  #else
    #define RC_DBG_REL    "release"
    #define RC_FILEFLAGS  0
  #endif

  #if defined(USE_ASAN)
  #define RC_BUILD_FEATURES  "ASAN"
  #endif

  /**
   * 'RC_BITS' is defined Makefile.Windows to '32' or '64'.
   */
  #define RC_VER_STRING  PROG_VERSION  " (" RC_BUILDER ", " _STR(RC_BITS) "-bits, " RC_DBG_REL ")"
  #define RC_VERSION     VER_MAJOR, VER_MINOR, VER_MICRO, 0
#endif

