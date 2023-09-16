/**
 * A simple config-file that gets force-included for *all*
 * .c-files in Dump1090.
 * Ref. option `-FI./dump1090_config.h` in Makefile.Windows.
 */
#pragma once

#define VER_MAJOR 0
#define VER_MINOR 4
#define VER_MICRO 1

/* Warning control:
 */
#define BUILD_WINDOWS                   1
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#define _CRT_SECURE_NO_WARNINGS         1
#define _CRT_SECURE_NO_DEPRECATE        1
#define _CRT_NONSTDC_NO_WARNINGS        1

#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-value"
  #pragma clang diagnostic ignored "-Wunused-variable"
  #pragma clang diagnostic ignored "-Wunused-function"
  #pragma clang diagnostic ignored "-Wmissing-field-initializers"

#elif defined(_MSC_VER)
  /*
   * wincontypes.h(103): warning C4005: 'MOUSE_MOVED': macro redefinition
   *   externals\Curses\curses.h(190): note: see previous definition of 'MOUSE_MOVED'
   */
   #pragma warning (disable: 4005)

  /*
   * misc.c(524): warning C4152: nonstandard extension,
   *   function/data pointer conversion in expression
   */
  #pragma warning (disable: 4152)

  /*
   * csv.c(60): warning C4244: '=':
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

  #ifdef _WIN64
    /*
     * 'type cast': conversion from 'int' to 'void *' of greater size
     */
    #pragma warning (disable: 4312)
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

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT 0x0600
#endif

/** To pull in `M_PI` in `<math.h>`
 */
#define _USE_MATH_DEFINES 1

/** Support various features in `externals/mongoose.c`:
 */
#define MG_ENABLE_ASSERT        1  /* Enable `assert()` calls */
#define MG_ENABLE_IPV6          0  /* IPv6 */
#define MG_ENABLE_FILE          1  /* For `opendir()` etc. */
#define MG_ENABLE_POLL          1  /* Prefer `WSAPoll()` over `select()`? */
#define MG_ENABLE_DIRLIST       0  /* Enable listing of directories for HTTP */
#define MG_ENABLE_CUSTOM_MILLIS 1  /* Enable 64-bit tick-time */

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
#include <string.h>
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

#if defined(_DEBUG) && !defined(RC_INVOKED)
  #include <malloc.h>

  #undef  _malloca          /* Avoid MSVC-9 <malloc.h>/<crtdbg.h> name-clash */
  #define _CRTDBG_MAP_ALLOC
  #include <crtdbg.h>
#else
  #define strdup(s)  _strdup (s)
#endif

/**
 * Enable "Windows epoll()"?
 */
#if defined(MG_ENABLE_EPOLL)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT 0x0602

  #include <wepoll.h>

  #define close(fd) epoll_close (fd)

  #if defined(__clang__)
    #pragma clang diagnostic ignored "-Wint-to-void-pointer-cast"
    #pragma clang diagnostic ignored "-Wvoid-pointer-to-int-cast"
  #else
    /*
     *  warning C4311: 'type cast': pointer truncation from 'HANDLE' to 'int'
     */
    #pragma warning (disable: 4311)
  #endif
#endif

#if !defined(USE_WIN_SQLITE)
  /*
   * Options for `externals/sqlite3.c`:
   */
  #define SQLITE_API
  #define SQLITE_DQS           3   /* Double-quoted string literals are allowed */
  #define SQLITE_THREADSAFE    0
  #define SQLITE_WIN32_MALLOC  1
  #define SQLITE_NO_SYNC       1
  #define SQLITE_OMIT_AUTOINIT 1
#endif

/*
 * For `net_io.c`:
 */
#if defined(USE_PACKED_DLL) || !defined(USE_PACKED_WEB)
  #undef USE_PACKED_WEB
  #undef PACKED_WEB_ROOT
#endif

