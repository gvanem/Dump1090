/**\file    gns-serial.c
 * \ingroup GNS-HULC
 *
 * Serial stuff for the GNS-HULC protocol.
 */
#define GNS_FILE "gns-serial.c"

#include "GNS-Hulc/gns-private.h"
#include "GNS-Hulc/gns-hulc.h"

/*
 * '#include <ntddser.h>' is needed for
 * `DeviceIoControl()` but causes a lot of redefinitions warnings.
 * Turn them off.
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4005)
#endif

#include <ntddser.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif


/**
 * \def COM_DEVICEMAP
 * \def COM_DEVICES
 * The HKLM Registry keys for Serial-COM to Device mappings and device addresses.
 */
#define COM_DEVICEMAP "Hardware\\Devicemap\\SerialCOMM"
#define COM_DEVICES   "SYSTEM\\CurrentControlSet\\Control\\COM Name Arbiter\\Devices"

static char *get_name_space (uint16_t port);
static void  get_port_mappings (void);

/**
 * Setup the serial line baudrate, flowcontrol, parity etc.
 */
HANDLE COM_init (uint16_t port)
{
  COMMPROP     comprop;
  COMSTAT      comstat;
  COMMTIMEOUTS cto;
  DCB          dcb;
  HANDLE       hnd;

  strncpy (g_data.COM.name_space, get_name_space(port), sizeof(g_data.COM.name_space)-1);
  if (Modes.debug & DEBUG_GNS_HULC)
     get_port_mappings();

  /* "\\.\COMx".
   */
  g_data.COM.dev_name = mg_mprintf ("\\\\.\\COM%d", port);

  DEBUG2 ("Modes.gns_hulc.port:  %u\n", Modes.gns_hulc.port);
  DEBUG2 ("Modes.gns_hulc.name:  %s\n", Modes.gns_hulc.name ? Modes.gns_hulc.name : "<none>");
  DEBUG2 ("Modes.selected_dev:  '%s'\n", Modes.selected_dev ? Modes.selected_dev  : "<none>");
  DEBUG2 ("name_space:          '%s'\n", g_data.COM.name_space);

  hnd = CreateFileA (g_data.COM.dev_name, GENERIC_READ | GENERIC_WRITE,
                     0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  if (hnd == INVALID_HANDLE_VALUE)
  {
    LOG_STDERR ("Error opening %s: %s\n", g_data.COM.dev_name, win_strerror(GetLastError()));
    return (NULL);
  }

  memset (&comprop, '\0', sizeof(comprop));
  if (!GetCommProperties(hnd, &comprop))
  {
    LOG_STDERR ("GetCommProperties() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  memset (&comstat, '\0', sizeof(comstat));
  if (!ClearCommError(hnd, NULL, &comstat))
  {
    LOG_STDERR ("ClearCommError() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  /* Setup baudrate and other communication settings
   */
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

  /* Set RTS + DTR flow-control
   */
  dcb.fRtsControl  = RTS_CONTROL_ENABLE;
  dcb.fDtrControl  = DTR_CONTROL_ENABLE;
  dcb.fOutxDsrFlow = TRUE;
  dcb.fOutxCtsFlow = FALSE;

  if (!SetCommState(hnd, &dcb))
  {
    LOG_STDERR ("SetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

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
  if (!SetCommMask(hnd, 0))
  {
    LOG_STDERR ("SetCommMask() failed: %s\n", win_strerror(GetLastError()));
    return (NULL);
  }

  DWORD bits1 = 0;
  DWORD bits2 = 0;
  DWORD bytes = 0;
  char  bits_str [200] = "";

  GetCommModemStatus (hnd, &bits1);

  if (bits1 & MS_CTS_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_CTS|");
  if (bits1 & MS_DSR_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_DSR|");
  if (bits1 & MS_RLSD_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_DCD|");
  if (bits1 & MS_RING_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_RI|");

  if (DeviceIoControl(hnd, IOCTL_SERIAL_GET_DTRRTS, NULL, 0,
                      &bits2, sizeof(bits2), &bytes, NULL))
  {
    if (bits2 & SERIAL_DTR_STATE)
       strcat_s (bits_str, sizeof(bits_str), "DTR_STATE|");
    if (bits2 & SERIAL_RTS_STATE)
       strcat_s (bits_str, sizeof(bits_str), "RTS_STATE|");
  }

  if (bits_str[0] == '\0')
     strcpy (bits_str, "<none> ");

  DEBUG2 ("GetCommModemStatus():  %.*s\n", (int)strlen(bits_str) - 1, bits_str);

  if (Modes.debug & DEBUG_GNS_HULC)
     COM_get_status (hnd);

  /* Flush the port and it's I/O buffers just in case
   */
  PurgeComm (hnd, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
  FlushFileBuffers (hnd);

  return (hnd);
}

/**
 * Restore the startup DCB + CTO if known.
 */
void COM_exit (HANDLE hnd)
{
  if (hnd && hnd != INVALID_HANDLE_VALUE && g_data.COM.old_DCB.DCBlength > 0)
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
  g_data.stat.rx_bytes += bytes;
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
  g_data.stat.tx_bytes += bytes;
  return (bytes);
}

/**
 * Get some details from the driver.
 */
#define COM_REQUEST(hnd, code, in_buf, in_size, out_buf, out_size, bytes)                     \
        do {                                                                                  \
          bytes = 0;                                                                          \
          if (!DeviceIoControl (hnd, code, in_buf, in_size, out_buf, out_size, &bytes, NULL)) \
          {                                                                                   \
            DEBUG1 ("DeviceIoControl() failed: %s\n", win_strerror(GetLastError()));          \
            return (false);                                                                   \
          }                                                                                   \
        } while (0)


bool COM_get_status (HANDLE hnd)
{
  SERIAL_STATUS    status;
  SERIALPERF_STATS perf;
  DWORD            bytes;

  /*
   * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddser/ni-ntddser-ioctl_serial_get_commstatus
   */
  COM_REQUEST (hnd, IOCTL_SERIAL_GET_COMMSTATUS, NULL, 0, &status, sizeof(status), bytes);

  DEBUG1 ("IOCTL_SERIAL_GET_COMMSTATUS (bytes: %lu):\n", bytes);
  DEBUG1 ("  status.Errors:                %lu\n", status.Errors);
  DEBUG1 ("  status.HoldReasons:           %lu\n", status.HoldReasons);
  DEBUG1 ("  status.AmountInInQueue:       %lu\n", status.AmountInInQueue);
  DEBUG1 ("  status.AmountInOutQueue:      %lu\n", status.AmountInOutQueue);
  DEBUG1 ("  status.EofReceived:           %d\n",  status.EofReceived);
  DEBUG1 ("  status.WaitForImmediate:      %d\n",  status.WaitForImmediate);

  /*
   * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddser/ni-ntddser-ioctl_serial_get_stats
   */
  COM_REQUEST (hnd, IOCTL_SERIAL_GET_STATS, NULL, 0, &perf, sizeof(perf), bytes);
  DEBUG1 ("IOCTL_SERIAL_GET_STATS (bytes: %lu):\n", bytes);
  DEBUG1 ("  perf.ReceivedCount:           %lu\n", perf.ReceivedCount);
  DEBUG1 ("  perf.TransmittedCount:        %lu\n", perf.TransmittedCount);
  DEBUG1 ("  perf.FrameErrorCount:         %lu\n", perf.FrameErrorCount);
  DEBUG1 ("  perf.SerialOverrunErrorCount: %lu\n", perf.SerialOverrunErrorCount);
  DEBUG1 ("  perf.BufferOverrunErrorCount: %lu\n", perf.BufferOverrunErrorCount);
  DEBUG1 ("  perf.ParityErrorCount:        %lu\n", perf.ParityErrorCount);
  return (true);
}

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

/**
 * Just prints the COM-port to physical serial map. Like:
 * ```
 *  1: COM9 -> \\?\usb#vid_1390&pid_5454#bq4395a97393#{86e0d1e0-8089-11d0-9ce4-08003e301f73}
 *  ...
 *  4: COM5 -> \\?\ftdibus#vid_0403+pid_6001+a5069rr4a#0000#{86e0d1e0-8089-11d0-9ce4-08003e301f73}
 * ```
 */
static void get_port_mappings (void)
{
  HKEY  key;
  DWORD num = 0;
  DWORD rc = RegOpenKeyExA (HKEY_LOCAL_MACHINE, COM_DEVICES, 0, KEY_READ, &key);

  if (rc != ERROR_SUCCESS)
  {
    DEBUG1 ("RegOpenKeyExA (\"HKLM\\%s\") failed; %s\n", COM_DEVICES, win_strerror(rc));
    return;
  }

  while (1)
  {
    char  value [100] = { '\0' };
    char  data [200]  = { '\0' };
    DWORD value_size  = sizeof(value);
    DWORD data_size   = sizeof(data);
    DWORD type        = REG_NONE;

    if (RegEnumValue(key, num, value, &value_size, NULL, &type, (BYTE*)&data, &data_size) != ERROR_SUCCESS)
       break;

    if (type == REG_SZ)
    {
      DEBUG1 ("%lu: %s -> %s\n", num, value, data);

      /**
       * \todo
       * And if the above 'data' start with e.g. "\\?\FTDIBUS#", get things like
       * "FriendlyName" and "Service" from this branch:
       * "HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\FTDIBUS\VID_x+PID_yxx\0000\Device Parameters"
       */
    }
    num++;
  }
  RegCloseKey (key);
}


