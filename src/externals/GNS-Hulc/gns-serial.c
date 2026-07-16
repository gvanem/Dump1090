/**\file    gns-serial.c
 * \ingroup GNS-HULC
 *
 * Serial stuff for the GNS-HULC protocol.
 */
#define GNS_FILE "gns-serial.c"

#include "GNS-Hulc/gns-private.h"
#include "GNS-Hulc/gns-hulc.h"

static char *get_name_space (uint16_t port);

/**
 * Setup the serial line baudrate, flowcontrol, parity etc.
 */
HANDLE COM_init (uint16_t port)
{
  strncpy (g_data.COM.name_space, get_name_space(port), sizeof(g_data.COM.name_space)-1);

  /* "\\.\COMx".
   */
  g_data.COM.dev_name = mg_mprintf ("\\\\.\\COM%d", port);

  DEBUG2 ("Modes.gns_hulc.port:  %u\n", Modes.gns_hulc.port);
  DEBUG2 ("Modes.gns_hulc.name:  %s\n", Modes.gns_hulc.name ? Modes.gns_hulc.name : "<none>");
  DEBUG2 ("Modes.selected_dev:  '%s'\n", Modes.selected_dev ? Modes.selected_dev  : "<none>");
  DEBUG2 ("name_space:          '%s'\n", g_data.COM.name_space);

  HANDLE hnd = CreateFileA (g_data.COM.dev_name, GENERIC_READ | GENERIC_WRITE,
                            0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hnd == INVALID_HANDLE_VALUE)
  {
    LOG_STDERR ("Error opening %s: %s\n", g_data.COM.dev_name, win_strerror(GetLastError()));
    return (NULL);
  }

  /* Setup baudrate and other communication settings
   */
  DCB dcb;

  memset (&dcb, '\0', sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);

  if (!GetCommState(hnd, &dcb))
  {
    LOG_STDERR ("GetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  /* Save for COM_exit()
   */
  memcpy (&g_data.COM.old_DCB, &dcb, sizeof(g_data.COM.old_DCB));

  /* Set the new data
   */
  dcb.DCBlength     = sizeof(dcb);
  dcb.BaudRate      = g_data.COM.baud_rate > 0 ? g_data.COM.baud_rate : COM_BAUD_RATE;
  dcb.ByteSize      = 8;
  dcb.StopBits      = ONESTOPBIT;
  dcb.Parity        = NOPARITY;
  dcb.fParity       = 0;
  dcb.fNull         = FALSE;
  dcb.fBinary       = TRUE;
  dcb.fAbortOnError = FALSE;
  dcb.fErrorChar    = FALSE;

  /* Enable RTS flow-control. Disable DSR flow-control.
   * Hulc uses RTS/CTS handshaking (not DSR/DTR).
   */
  dcb.fRtsControl  = RTS_CONTROL_ENABLE;
  dcb.fDtrControl  = DTR_CONTROL_ENABLE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;

  if (!SetCommState(hnd, &dcb))
  {
    LOG_STDERR ("SetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  COMMTIMEOUTS cto;

  memset (&cto, '\0', sizeof(cto));
  GetCommTimeouts (hnd, &cto);

  /* Save for COM_exit()
   */
  memcpy (&g_data.COM.old_CTO, &cto, sizeof(g_data.COM.old_CTO));

  /* Change read / write timeouts
   */
  cto.ReadIntervalTimeout         = 1;
  cto.ReadTotalTimeoutMultiplier  = 0;
  cto.ReadTotalTimeoutConstant    = 1;
  cto.WriteTotalTimeoutMultiplier = 1000;
  cto.WriteTotalTimeoutConstant   = 1000;

  if (!SetCommTimeouts(hnd, &cto))
  {
    LOG_STDERR ("SetCommTimeouts() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  /* No event monitoring.
   */
  DWORD events = 0;

  if (!SetCommMask(hnd, events))
  {
    LOG_STDERR ("SetCommMask() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  /* Flush the port and it's I/O buffers just in case
   */
  PurgeComm (hnd, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
  FlushFileBuffers (hnd);

  return (hnd);
}

/**
 * Restore the startup `DCB` and `CTO` if known and
 * it's not a fatal-exit condition.
 */
void COM_exit (HANDLE hnd)
{
  if (hnd && hnd != INVALID_HANDLE_VALUE &&
      g_data.COM.old_DCB.DCBlength > 0 && !g_data.fatal_exit)
  {
    if (!SetCommTimeouts(hnd, &g_data.COM.old_CTO))
       LOG_STDERR ("SetCommTimeouts() failed: %s\n", win_strerror(GetLastError()));

    if (!SetCommState(hnd, &g_data.COM.old_DCB))
       LOG_STDERR ("SetCommState() failed: %s\n", win_strerror(GetLastError()));
  }
  memset (&g_data.COM.old_DCB, '\0', sizeof(g_data.COM.old_DCB));
  memset (&g_data.COM.old_CTO, '\0', sizeof(g_data.COM.old_CTO));
}

/**
 * Read from the serial device.
 * Returns immediately if no data is available and never blocks.
 */
int COM_read (HANDLE hnd, uint8_t *data, size_t len)
{
  DWORD bytes = 0;

  if (!ReadFile(hnd, data, len, &bytes, NULL))
  {
    LOG_STDERR ("ReadFile() failed; %s\n", win_strerror(GetLastError()));
    g_data.stat.rx_errors++;
    return (-1);
  }
  return (bytes);
}

/**
 * Write to the serial device.
 * The below functions blocks until the data is sent.
 *
 * \todo
 * Detect a "stuck device". Like using a COM-port over a
 * BlueTooth device. Unless it's associated with a PAN-network,
 * a `WriteFile()` will block forever!
 *
 * Or use Overlapped IO with a timeout.
 * But using the `g_data.COM.dead_count` logic seems enought.
 */
int COM_write (HANDLE hnd, const uint8_t *data, size_t len)
{
  DWORD bytes = 0;

  if (!WriteFile(hnd, data, len, &bytes, NULL))
  {
    LOG_STDERR ("WriteFile() failed; %s\n", win_strerror(GetLastError()));
    g_data.stat.tx_errors++;
    return (-1);
  }
  return (bytes);
}

bool COM_get_comstat (HANDLE hnd, DWORD *err_mask, COMSTAT *comstat)
{
  if (err_mask)
     *err_mask = 0;

  if (comstat)
     memset (comstat, '\0', sizeof(*comstat));

  if (!ClearCommError(hnd, err_mask, comstat))
  {
    LOG_STDERR ("ClearCommError() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }
  return (true);
}

#define ADD_VALUE(v)  { (DWORD)v, #v }

static const search_list com_events[] = {
             ADD_VALUE (EV_RXCHAR),
             ADD_VALUE (EV_RXFLAG),
             ADD_VALUE (EV_TXEMPTY),
             ADD_VALUE (EV_CTS),
             ADD_VALUE (EV_DSR),
             ADD_VALUE (EV_RLSD),
             ADD_VALUE (EV_BREAK),
             ADD_VALUE (EV_ERR),
             ADD_VALUE (EV_RING),
             ADD_VALUE (EV_PERR),
             ADD_VALUE (EV_RX80FULL),
             ADD_VALUE (EV_EVENT1),
             ADD_VALUE (EV_EVENT2)
           };
#undef ADD_VALUE

/**
 * Call WaitCommEvents to poll for events.
 * Wanted events are set once in below `SetCommMask()`.
 */
bool COM_poll_events (void)
{
  static bool done = false;
  uint64_t    start;
  const char *ev_str;
  DWORD       events = 0;

  if (!done)
  {
    if (!SetCommMask(Modes.gns_hulc.handle, EV_RXCHAR | EV_CTS | EV_DSR | EV_TXEMPTY | EV_ERR))
       LOG_STDERR ("SetCommMask() failed: %s\n", win_strerror(GetLastError()));
    done = true;
  }

  start = get_usec_now();

  if (!WaitCommEvent(Modes.gns_hulc.handle, &events, NULL))
  {
    DEBUG1 ("WaitCommEvent() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  ev_str = flags_decode (events, com_events, DIM(com_events));

  if (events & EV_ERR)
  {
    DEBUG1 ("WaitCommEvent(): %s, %.0f usec\n", ev_str, get_usec_now() - start);
    COM_poll_error();
  }
  else
  {
    DEBUG1 ("WaitCommEvent(): %s, %.0f usec\n", ev_str, get_usec_now() - start);
  }
  return (events & EV_RXCHAR);
}

/**
 * Check error-mask for errors.
 * This is typically Rx-overruns (CE_OVERRUN).
 */
void COM_poll_error (void)
{
  static DWORD err_mask_old;
  DWORD        err_mask = 0;

  if (!ClearCommError(Modes.gns_hulc.handle, &err_mask, NULL) ||
      err_mask == err_mask_old)
     return;

  err_mask_old = err_mask;
  if (err_mask & CE_OVERRUN)
  {
    g_data.stat.rx_overruns++;
    LOG_FILEONLY ("CE_OVERRUN\n");
  }
}

/**
 * \def COM_DEVICEMAP
 * The HKLM Registry key for Serial-COM to Device mappings.
 */
#define COM_DEVICEMAP "Hardware\\Devicemap\\SerialCOMM"

/**
 * Look in the Registry COM-port mapping at `COM_DEVICEMAP` for
 * a NT-namespace name for `COMx`.
 *
 * \eg if `look_for == "COM4"` and `RegEnumValue()` returns
 *     `value == "\Device\VCP0"` and `data == "COM4"`,
 *     then return `"\Device\VCP0"` for the name-space name.
 *     (the first Virtual COM-port).
 */
static char *get_name_space (uint16_t port)
{
  static char ret [100];
  char   look_for [100];
  HKEY   key;
  DWORD  rc, num = 0;

  strcpy (ret, "?");
  snprintf (look_for, sizeof(look_for), "COM%u", port);

  DEBUG2 ("look_for: '%s'\n", look_for);

  rc = RegOpenKeyExA (HKEY_LOCAL_MACHINE, COM_DEVICEMAP, 0, KEY_READ, &key);
  if (rc != ERROR_SUCCESS)
  {
    DEBUG2 ("RegOpenKeyExA (\"HKLM\\%s\") failed; %s\n", COM_DEVICEMAP, win_strerror(rc));
    return (ret);
  }

  while (1)
  {
    char  value [100] = { '\0' };
    char  data [100]  = { '\0' };
    DWORD value_size  = sizeof(value);
    DWORD data_size   = sizeof(data);
    DWORD type        = REG_NONE;
    const char *err = "No more items";

    rc = RegEnumValue (key, num++, value, &value_size, NULL, &type, (BYTE*)&data, &data_size);
    if (rc != ERROR_NO_MORE_ITEMS)
       err = win_strerror (GetLastError());

    DEBUG2 ("RegEnumValue(): %s\n", err);
    if (rc == ERROR_NO_MORE_ITEMS)
       goto quit;

    if (type != REG_SZ)
       continue;

    if (!strnicmp(data, look_for, strlen(look_for)))
    {
      strncpy (ret, value, sizeof(ret)-1);
      goto quit;
    }
  }
quit:
  RegCloseKey (key);
  DEBUG2 ("ret: '%s'\n", ret);
  return (ret);
}

