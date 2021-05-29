/**\file    misc.h
 * \ingroup Misc
 *
 * Various macroos and definitions.
 */
#ifndef _MISC_H
#define _MISC_H

#include <stdio.h>

/**
 * Various helper macros.
 */
#define MODES_NOTUSED(V)   ((void)V)
#define IS_SLASH(c)        ((c) == '\\' || (c) == '/')
#define TWO_PI             (2 * M_PI)
#define DIM(array)         (sizeof(array) / sizeof(array[0]))
#define ONE_MBYTE          (1024*1024)
#define STDIN_FILENO       0

/**
 * \def GMAP_HTML
 * Our default main server page relative to `Mode.who_am_I`.
 */
#define GMAP_HTML         "web_root/gmap.html"

#define ADS_B_ACRONYM     "ADS-B; Automatic Dependent Surveillance - Broadcast"
#define AIRCRAFT_CSV      "aircraftDatabase.csv"

#if defined(_DEBUG) || defined(USE_SDRPLAY)
  #undef  AIRCRAFT_CSV
  #define AIRCRAFT_CSV ""  /* since it takes time to load */
#endif

/**
 * \def MSEC_TIME()
 * Returns a 64-bit tick-time value with 1 millisec granularity.
 */
#define MSEC_TIME() GetTickCount64()

/**
 * \def SAFE_COND_SIGNAL(cond, mutex)
 * \def SAFE_COND_WAIT(cond, mutex)
 *
 * Signals are not threadsafe by default.
 * Taken from the Osmocom-SDR code and modified to
 * use Win-Vista+ functions.
 */
#define SAFE_COND_SIGNAL(cond, mutex)   \
        do {                            \
          EnterCriticalSection (mutex); \
          WakeConditionVariable (cond); \
          LeaveCriticalSection (mutex); \
        } while (0)

#define SAFE_COND_WAIT(cond, mutex)     \
        do {                            \
          EnterCriticalSection (mutex); \
          WakeConditionVariable (cond); \
          LeaveCriticalSection (mutex); \
        } while (0)

/**
 * \def TRACE(bit, fmt, ...)
 * A more compact tracing macro
 */
#define TRACE(bit, fmt, ...)                  \
        do {                                  \
          if (Modes.debug & (bit))            \
             modeS_flogf (stdout, "%u: " fmt, \
                 __LINE__, __VA_ARGS__);      \
        } while (0)

/**
 * \def LOG_STDOUT(fmt, ...)
 * \def LOG_STDERR(fmt, ...)
 *
 * Print to both `stdout` and optionally to `Modes.log`.
 * Print to both `stderr` and optionally to `Modes.log`.
 */
#define LOG_STDOUT(fmt, ...)  modeS_flogf (stdout, fmt, __VA_ARGS__)
#define LOG_STDERR(fmt, ...)  modeS_flogf (stderr, fmt, __VA_ARGS__)

/*
 * Defined in MSVC's <sal.h>.
 */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

#if defined(__clang__)
  #define ATTR_PRINTF(_1, _2) __attribute__((format(printf, _1, _2)))
#else
  #define ATTR_PRINTF(_1, _2)
#endif

extern void modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);

#endif