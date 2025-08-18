/**\file    pconsole.c
 * \ingroup Misc
 * \brief   Functions for "Pseudo Console" in Win-10, October 2018 Update and later.
 */
#include "misc.h"

/**
 * Launch ourself with a pseudo console.
 *
 * Rewritten from: https://github.com/arakiken/mlterm/blob/master/vtemu/vt_pty_win32.c
 *
 * \sa
 *  https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session/
 *  https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/
 *  https://github.com/microsoft/terminal/discussions/15814/
 *  https://github.com/microsoft/terminal/discussions/14257/
 */
typedef struct pconsole_t {
        HANDLE   master_input;    /* master read(stdout,stderr) */
        HANDLE   master_output;   /* master write */
        HANDLE   slave_stdout;    /* slave write handle */
        HANDLE   slave_read;      /* slave read handle */
        HANDLE   child_proc;
        void    *pseudo_hnd;
        COORD    coord;
        char     ev_name [40];
        u_char   read_ch;
        HANDLE   read_ev;
        HANDLE   read_thrd;
        DWORD    read_tid;
        void    *attr_list;
      } pconsole_t;

#if !defined(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE)
  #define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016

#elif !defined(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE)
  /*
   * From ' enum _PROC_THREAD_ATTRIBUTE_NUM' and
   * '(NTDDI_VERSION >= NTDDI_WIN10_RS5)'
   */
  #define ProcThreadAttributePseudoConsole 22

  #define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
          ProcThreadAttributeValue (ProcThreadAttributePseudoConsole, FALSE, TRUE, FALSE)
#endif

/**
 * Load and use the 3 *Pseudo Console* functions dynamically.
 */
#undef  ADD_VALUE
#define ADD_VALUE(func)  { false, NULL, "kernel32.dll", #func, (void**) &p_##func }
                        /* ^ no functions are optional */

DEF_WIN_FUNC (HRESULT, CreatePseudoConsole, (COORD  size,
                                             HANDLE input,
                                             HANDLE output,
                                             DWORD  flags,
                                             void **hnd));

DEF_WIN_FUNC (HRESULT, ClosePseudoConsole,  (void **hnd));
DEF_WIN_FUNC (HRESULT, ResizePseudoConsole, (void **hnd, COORD size));

static struct dyn_struct kernel32_funcs[] = {
                         ADD_VALUE (CreatePseudoConsole),
                         ADD_VALUE (ClosePseudoConsole),
                         ADD_VALUE (ResizePseudoConsole)
                       };

static void pconsole_trigger_read (void)
{
  PostThreadMessage (GetCurrentThreadId(), WM_APP, 0, 0);
}

/*
 * Monitors handle for input. Exits when child exits or pipe is broken.
 */
static DWORD WINAPI pconsole_read (void *arg)
{
  pconsole_t *pty = (pconsole_t*) arg;
  DWORD       n_rd;

  DEBUG (DEBUG_GENERAL, "Starting %s() thread.\n", __FUNCTION__);

  while (1)
  {
    if (!ReadFile(pty->master_input, &pty->read_ch, 1, &n_rd, NULL) || n_rd == 0)
    {
      DEBUG (DEBUG_GENERAL, "ReadFile() failed; %s\n", win_strerror(GetLastError()));
      break;
    }
    pconsole_trigger_read();

    if (WaitForSingleObject (pty->read_ev, INFINITE) == WAIT_OBJECT_0 ||
        !pty->child_proc)
       break;
  }

  DEBUG (DEBUG_GENERAL, "Exiting %s() thread.\n", __FUNCTION__);
  ExitThread (0);
}

static bool pconsole_init (pconsole_t *pty)
{
  if (load_dynamic_table(kernel32_funcs, DIM(kernel32_funcs)) != DIM(kernel32_funcs))
  {
    DEBUG (DEBUG_NET, "Failed to load the needed 'kernel32.dll' functions.\n");
    return (false);
  }

  snprintf (pty->ev_name, sizeof(pty->ev_name), "PCONSOLE_READY_%lx", HandleToUlong(pty->child_proc));
  pty->read_ev = CreateEvent (NULL, FALSE, FALSE, pty->ev_name);
  if (!pty->read_ev)
  {
    DEBUG (DEBUG_GENERAL, "CreateEvent (\"%s\") failed; %s.\n", pty->ev_name, win_strerror(GetLastError()));
    return (false);
  }

  /* Launch the thread that read the child's output.
   */
  pty->read_thrd = CreateThread (NULL, 0, pconsole_read, (void*)pty, 0, &pty->read_tid);
  if (!pty->read_thrd)
  {
    DEBUG (DEBUG_GENERAL, "CreateThread() failed; %s.\n", win_strerror(GetLastError()));
    return (false);
  }

  return (true);
}

static void pconsole_exit (pconsole_t *pty)
{
  unload_dynamic_table (kernel32_funcs, DIM(kernel32_funcs));
  if (pty->read_thrd)
  {
    SetEvent (pty->read_ev);
    WaitForSingleObject (pty->read_ev, 500);
    CloseHandle (pty->read_ev);
    CloseHandle (pty->read_thrd);
  }
}

bool pconsole_create (struct pconsole_t *pty, const char *cmd_path, const char **cmd_argv)
{
  STARTUPINFOEX       si;
  PROCESS_INFORMATION pi;
  HRESULT             ret;
  SIZE_T              list_size;
  char               *cmd_line;

  memset (pty, '\0', sizeof(*pty));
  memset (&si, '\0', sizeof(si));

  if (!pconsole_init(pty))
     return (false);

  if (!CreatePipe(&pty->slave_read, &pty->master_output, NULL, 0))
     goto error;

  if (!CreatePipe(&pty->master_input, &pty->slave_stdout, NULL, 0))
     goto error;

  ret = (*p_CreatePseudoConsole) (pty->coord,
                                  pty->slave_read,
                                  pty->slave_stdout,
                                  0,
                                  &pty->pseudo_hnd);
  if (!SUCCEEDED(ret))
  {
    DEBUG (DEBUG_GENERAL, "'(*p_CreatePseudoConsole)()' failed; %s.\n", win_strerror(ret));
    goto error;
  }

  si.StartupInfo.cb = sizeof(si);

  /* the size required for the list
   */
  list_size = 0;
  if (!InitializeProcThreadAttributeList (NULL, 1, 0, &list_size) || list_size == 0)
     goto error;

  si.lpAttributeList = calloc (list_size, 1);
  if (!si.lpAttributeList)
     goto error;

  /* Set the pseudoconsole information into the list
   */
  if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &list_size) ||
      !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 pty->pseudo_hnd, sizeof(pty->pseudo_hnd), NULL, NULL))
   {
    DEBUG (DEBUG_GENERAL, "InitializeProcThreadAttributeList() or "
                          "UpdateProcThreadAttribute() failed; %s.\n",
                          win_strerror(HRESULT_FROM_WIN32(GetLastError())));
    goto error;
  }

  if (cmd_argv)
  {
    size_t cmd_line_len;
    int    i;

    /* Because cmd_path == cmd_argv[0], cmd_argv[0] is ignored.
     */
    cmd_line_len = strlen (cmd_path) + 1;
    for (i = 1; cmd_argv[i]; i++)
       cmd_line_len += strlen (cmd_argv[i]) + 1;

    cmd_line = alloca (cmd_line_len);
    strcpy (cmd_line, cmd_path);
    for (i = 1; cmd_argv[i]; i++)
    {
      strcat (cmd_line, " ");
      strcat (cmd_line, cmd_argv[i]);
    }
  }
  else
    cmd_line = (char*) cmd_path;

  memset (&pi, '\0', sizeof(pi));

  if (!CreateProcess(cmd_path, cmd_line, NULL, NULL, FALSE,
                     EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                     &si.StartupInfo, &pi))
  {
    DEBUG (DEBUG_GENERAL, "CreateProcess() failed; %s.\n", win_strerror(HRESULT_FROM_WIN32(GetLastError())));
    goto error;
  }

  pty->child_proc = pi.hProcess;
  pty->attr_list  = si.lpAttributeList;

  CloseHandle (pty->slave_read);
  CloseHandle (pi.hThread);

  pconsole_exit (pty);
  return (true);

error:
  if (si.lpAttributeList)
  {
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free (si.lpAttributeList);
  }

  if (pty->pseudo_hnd)
     (*p_ClosePseudoConsole) (pty->pseudo_hnd);

  if (pty->master_input)
     CloseHandle (pty->master_input);

  if (pty->slave_read)
     CloseHandle (pty->slave_read);

  if (pty->master_output)
     CloseHandle (pty->master_output);

  if (pty->slave_stdout)
     CloseHandle (pty->slave_stdout);

  pconsole_exit (pty);
  return (false);
}

