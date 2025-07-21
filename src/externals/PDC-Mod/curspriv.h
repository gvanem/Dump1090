/* Private definitions and declarations for use within PDCurses.
   These should generally not be referenced by applications. */

#pragma once

#include <curses.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {        /* structure for ripped off lines */
        int     line;
        int   (*init)(WINDOW *, int);
        WINDOW *win;
      } RIPPEDOFFLINE;

/* Window properties */

#define _SUBWIN    0x01  /* window is a subwindow */
#define _PAD       0x10  /* X/Open Pad. */
#define _SUBPAD    0x20  /* X/Open subpad. */

/* Miscellaneous */

#define _NO_CHANGE -1    /* flags line edge unchanged */

#define _ECHAR     0x08  /* Erase char       (^H) */
#define _DWCHAR    0x17  /* Delete Word char (^W) */
#define _DLCHAR    0x15  /* Delete Line char (^U) */

/*
 * Microsoft(R) Windows defines these.
 * So lets define these differently?
 */
#if 0
  #define PDC_LOW_SURROGATE_START   0xDC00
  #define PDC_LOW_SURROGATE_END     0xDFFF
  #define PDC_HIGH_SURROGATE_START  0xD800
  #define PDC_HIGH_SURROGATE_END    0xDBFF

  #define PDC_IS_LOW_SURROGATE(c)   ((c) >= PDC_LOW_SURROGATE_START  && (c) <= PDC_LOW_SURROGATE_END)
  #define PDC_IS_HIGH_SURROGATE(c)  ((c) >= PDC_HIGH_SURROGATE_START && (c) <= PDC_HIGH_SURROGATE_END)
  #define PDC_IS_SURROGATE(c)       ((PDC_IS_HIGH_SURROGATE(c) && PDC_IS_LOW_SURROGATE(c)))

#else
  #define PDC_LOW_SURROGATE_START   LOW_SURROGATE_START
  #define PDC_LOW_SURROGATE_END     LOW_SURROGATE_END
  #define PDC_HIGH_SURROGATE_START  HIGH_SURROGATE_START
  #define PDC_HIGH_SURROGATE_END    HIGH_SURROGATE_END

  #define PDC_IS_LOW_SURROGATE(c)   IS_LOW_SURROGATE (c)
  #define PDC_IS_HIGH_SURROGATE(c)  IS_HIGH_SURROGATE (c)
  #define PDC_IS_SURROGATE(c)       IS_SURROGATE_PAIR (c, c)
#endif

/*----------------------------------------------------------------------*/

/* Platform implementation functions */

void    PDC_beep(void);
bool    PDC_can_change_color(void);
int     PDC_color_content(int, int *, int *, int *);
bool    PDC_check_key(void);
int     PDC_curs_set(int);
void    PDC_flushinp(void);
int     PDC_get_columns(void);
int     PDC_get_cursor_mode(void);
int     PDC_get_key(void);
int     PDC_get_rows(void);
void    PDC_gotoyx(int, int);
bool    PDC_has_mouse(void);
int     PDC_init_color(int, int, int, int);
int     PDC_modifiers_set(void);
int     PDC_mouse_set(void);
void    PDC_napms(int);
void    PDC_reset_prog_mode(void);
void    PDC_reset_shell_mode(void);
int     PDC_resize_screen(int, int);
void    PDC_restore_screen_mode(int);
void    PDC_save_screen_mode(int);
void    PDC_scr_close(void);
void    PDC_scr_free(void);
int     PDC_scr_open(void);
void    PDC_set_keyboard_binary(bool);
void    PDC_transform_line(int, int, int, const chtype *);
void    PDC_transform_line_sliced(int, int, int, const chtype *);
const char *PDC_sysname(void);

/* Internal cross-module functions */

void   *PDC_realloc_array( void *ptr, size_t nmemb, size_t size);
int     PDC_init_atrtab(void);
void    PDC_free_atrtab(void);
WINDOW *PDC_makelines(WINDOW *);
WINDOW *PDC_makenew(int, int, int, int);
long    PDC_millisecs( void);
int     PDC_mouse_in_slk(int, int);
void    PDC_slk_free(void);
void    PDC_slk_initialize(void);
void    PDC_sync(WINDOW *);
void    PDC_add_window_to_list( WINDOW *win);
void    PDC_set_default_colors(int, int);
void    PDC_set_changed_cells_range( WINDOW *, int y, int start, int end);
void    PDC_mark_line_as_changed( WINDOW *win, int y);
void    PDC_mark_cells_as_changed( WINDOW *, int y, int start, int end);
void    PDC_mark_cell_as_changed( WINDOW *, int y, int x);
bool    PDC_touched_range( const WINDOW *win, int y, int *firstch, int *lastch);
int     PDC_wscrl(WINDOW *win, int top, int bottom, int n);

int     PDC_mbtowc(wchar_t *, const char *, size_t);
size_t  PDC_mbstowcs(wchar_t *, const char *, size_t);
size_t  PDC_wcstombs(char *, const wchar_t *, size_t);
int     PDC_wcwidth(int32_t ucs);
int     PDC_expand_combined_characters(cchar_t c, cchar_t *added);
int     PDC_find_combined_char_idx(cchar_t root, cchar_t added);
int     PDC_pnoutrefresh_with_stored_params (WINDOW *pad);

#define MAX_UNICODE 0x110000

/* Internal macros for attributes */

#define DIVROUND(num, divisor) ((num) + ((divisor) >> 1)) / (divisor)

#define PDC_CLICK_PERIOD 150  /* time to wait for a click, if
                                 not set by mouseinterval() */
#define PDC_MAXCOL       768  /* maximum possible COLORS; may be less */

#define _INBUFSIZ        512  /* size of terminal input buffer */
#define NUNGETCH         256  /* max # chars to ungetch() */
#define MAX_PACKET_LEN    90  /* max # chars to send to PDC_transform_line */

#define OFF_SCREEN_WINDOWS_TO_RIGHT_AND_BOTTOM        1
#define OFF_SCREEN_WINDOWS_TO_LEFT_AND_TOP            2

#define INTENTIONALLY_UNUSED_PARAMETER( param) (void)(param)

#define _is_altcharset( ch)  (((ch) & (A_ALTCHARSET | (A_CHARTEXT ^ 0x7f))) == A_ALTCHARSET)

typedef struct _win _win;

typedef struct _win {             /* definition of a window */
    int      _cury;               /* current pseudo-cursor */
    int      _curx;
    int      _maxy;               /* max window Y-coordinate */
    int      _maxx;               /* max window X-coordinate */
    int      _begy;               /* Y-origin on screen */
    int      _begx;               /* X-origin on screen */
    int      _flags;              /* window properties */
    chtype   _attrs;              /* standard attributes and colors */
    chtype   _bkgd;               /* background, normally blank */
    bool     _clear;              /* causes clear at next refresh */
    bool     _leaveit;            /* leaves cursor where it is */
    bool     _scroll;             /* allows window scrolling */
    bool     _nodelay;            /* input character wait flag */
    bool     _immed;              /* immediate update flag */
    bool     _sync;               /* synchronise window ancestors */
    bool     _use_keypad;         /* flags keypad key mode active */
    chtype **_y;                  /* pointer to line pointer array */
    int     *_firstch;            /* first changed character in line */
    int     *_lastch;             /* last changed character in line */
    int      _tmarg;              /* top of scrolling region */
    int      _bmarg;              /* bottom of scrolling region */
    int      _delayms;            /* milliseconds of delay for getch() */
    int      _parx, _pary;        /* coords relative to parent (0,0) */
    _win    *_parent;             /* subwin's pointer to parent win */
    int      _pminrow, _pmincol;  /* saved position used only for pads */
    int      _sminrow, _smaxrow;  /* saved position used only for pads */
    int      _smincol, _smaxcol;  /* saved position used only for pads */
 } _win;

typedef int32_t hash_idx_t;

#define MAX_RIPPEDOFFLINES 5

struct _screen {
    bool    alive;                 /* if initscr() called, and not endwin() */
    bool    autocr;                /* if cr -> lf */
    bool    cbreak;                /* if terminal unbuffered */
    bool    echo;                  /* if terminal echo */
    bool    raw_inp;               /* raw input mode (v. cooked input) */
    bool    raw_out;               /* raw output mode (7 v. 8 bits) */
    bool    audible;               /* FALSE if the bell is visual */
    bool    mono;                  /* TRUE if current screen is mono */
    bool    resized;               /* TRUE if TERM has been resized */
    bool    orig_attr;             /* TRUE if we have the original colors */
    short   orig_fore;             /* original screen foreground color */
    short   orig_back;             /* original screen foreground color */
    int     cursrow;               /* position of physical cursor */
    int     curscol;               /* position of physical cursor */
    int     visibility;            /* visibility of cursor */
    int     orig_cursor;           /* original cursor size */
    int     lines;                 /* new value for LINES */
    int     cols;                  /* new value for COLS */
    mmask_t _trap_mbe;             /* trap these mouse button events */
    int     mouse_wait;            /* time to wait (in ms) for a
                                      button release after a press, in
                                      order to count it as a click */
    int     slklines;              /* lines in use by slk_init() */
    WINDOW *slk_winptr;            /* window for slk */
    int     linesrippedoff;        /* lines ripped off via ripoffline() */
    RIPPEDOFFLINE *linesripped;

    int   delaytenths;             /* 1/10ths second to wait block getch() for */
    bool  _preserve;               /* TRUE if screen background
                                      to be preserved */
    int   _restore;                /* specifies if screen background
                                      to be restored, and how */
    unsigned long key_modifiers;   /* key modifiers (SHIFT, CONTROL, etc.)
                                      on last key press */
    bool  return_key_modifiers;    /* TRUE if modifier keys are
                                      returned as "real" keys */
    bool  in_endwin;               /* if we're in endwin(),  we should use
                                      only signal-safe code */
    MOUSE_STATUS mouse_status;     /* last returned mouse status */
    short line_color;     /* color of line attributes - default -1 */
    attr_t termattrs;     /* attribute capabilities */
    WINDOW *lastscr;      /* the last screen image */
    FILE *dbfp;           /* debug trace file pointer */
    bool  color_started;  /* TRUE after start_color() */
    bool  dirty;          /* redraw on napms() after init_color() */
    int   sel_start;      /* start of selection (y * COLS + x) */
    int   sel_end;        /* end of selection */
    int  *c_buffer;       /* character buffer */
    int   c_pindex;       /* putter index */
    int   c_gindex;       /* getter index */
    int  *c_ungch;        /* array of ungotten chars */
    int   c_ungind;       /* ungetch() push index */
    int   c_ungmax;       /* allocated size of ungetch() buffer */
    struct _pdc_pair *pairs;
    int pairs_allocated;
    int first_col;
    int blink_state;
    bool default_colors;
    int default_foreground_idx;     /* defaults to COLOR_WHITE */
    int default_background_idx;     /* defaults to COLOR_BLACK */
    hash_idx_t *pair_hash_tbl;
    int pair_hash_tbl_size, pair_hash_tbl_used;
    int n_windows, off_screen_windows;
    WINDOW **window_list;
    unsigned trace_flags;
    bool want_trace_fflush;
    bool ncurses_mouse;          /* map wheel events to button 4,5 presses */
};

extern SCREEN  *SP;          /* curses variables */

/* Formerly in "pdcwin.h":
 */
typedef struct PDCCOLOR {
        short r, g, b;
        bool mapped;
      } PDCCOLOR;

extern PDCCOLOR pdc_color [PDC_MAXCOL];

extern HANDLE pdc_con_out, pdc_con_in;
extern DWORD  pdc_quick_edit;
extern DWORD  pdc_last_blink;
extern short  pdc_curstoreal[16], pdc_curstoansi[16];
extern short  pdc_oldf, pdc_oldb, pdc_oldu;
extern bool   pdc_conemu, pdc_wt, pdc_ansi;

void PDC_blink_text (void);

