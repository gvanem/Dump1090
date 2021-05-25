#ifdef SDRPLAY_SUPPORT

#include <sdrplay_api.h>

#include "misc.h"
#include "sdrplay.h"

/**
 * \def LOAD_FUNC(pi, f)
 *   A `GetProcAddress()` helper.
 *   \param pi   the `struct python_info` to work on.
 *   \param f    the name of the function (without any `"`).
 */
#define LOAD_FUNC(pi, f)  do {                                               \
                            f = (func_##f) GetProcAddress (pi->dll_hnd, #f); \
                            if (!f) {                                        \
                              WARN ("Failed to find \"%s()\" in %s.\n",      \
                                    #f, pi->dll_name);                       \
                              goto failed;                                   \
                            }                                                \
                            TRACE (3, "Function %s(): %*s 0x%p.\n",          \
                                   #f, 23-(int)strlen(#f), "", f);           \
                          } while (0)

/**
 * We only need 1 set of function-pointers for each embeddable Python program
 * since we currently only embed 1 Python at a time.
 *
 * But ideally, these function-pointers should be put in `struct python_info` to be able
 * to use several Pythons without calling `py_init_embedding()` and `py_exit_embedding()`
 * for each embeddable Python variant.
 *
 * \def DEF_FUNC(ret,f,(args))
 *   define the `typedef` and declare the function-pointer for
 *   the Python-2/3 function we want to import.
 *   \param ret    the return value type (or `void`)
 *   \param f      the name of the function (without any `"`).
 *   \param (args) the function arguments (as one list).
 */
#define DEF_FUNC(ret,f,args)  typedef ret (__cdecl *func_##f) args; \
                              static func_##f f

DEF_FUNC (sdrplay_api_ErrT, sdrplay_api_Open,        (void));


/**
 * Do NOT call this unless `py->is_embeddable == TRUE`.
 */
static BOOL load_sdrplay_dll (struct python_info *pi)
{
  char *exe = pi->exe_name;
  char *dll = pi->dll_name;

  if (!dll)
  {
    WARN ("Failed to find Python DLL for %s.\n", exe);
    return (FALSE);
  }

  pi->dll_hnd = LoadLibrary (dll);
  if (!pi->dll_hnd)
  {
    WARN ("Failed to load %s; %s\n", dll, win_strerror(GetLastError()));
    pi->is_embeddable = FALSE;  /* Do not do this again */
    return (FALSE);
  }

  TRACE (2, "Full DLL name: \"%s\". Handle: 0x%p\n", pi->dll_name, pi->dll_hnd);

  LOAD_FUNC (pi, Py_InitializeEx);
  LOAD_FUNC (pi, Py_IsInitialized);
  LOAD_FUNC (pi, Py_Finalize);
  LOAD_FUNC (pi, PySys_SetArgvEx);
  LOAD_FUNC (pi, Py_FatalError);
  LOAD_FUNC (pi, Py_SetProgramName);

failed:
}

int modeS_init_SDRplay (void)
{
  LOG_STDERR ("No supported SDRplay devices found.\n");
  return (1);
}

#endif

int dummy;
