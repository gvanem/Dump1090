/**\file    gns-serial.c
 * \ingroup GNS-HULC
 *
 * Serial stuff for the GNS-HULC protocol.
 */
#define GNS_FILE "gns-serial.c"

#include "GNS-Hulc/gns-private.h"
#include "GNS-Hulc/gns-hulc.h"

/**
 * Setup the serial line baudrate, flowcontrol, parity etc.
 */
bool COM_init (HANDLE handle)
{
  COMMPROP     comprop;
  COMSTAT      comstat;
  COMMTIMEOUTS cto;
  DCB          dcb;
  DWORD        mask;

  memset (&comprop, '\0', sizeof(comprop));
//comprop.wPacketLength = sizeof(commprop));
//comprop.dwProvSpec1   = COMMPROP_INITIALIZED;
  if (!GetCommProperties(handle, &comprop))
  {
    LOG_STDERR ("GetCommProperties() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }
  DEBUG1 ("comprop.dwProvSubType:      %08lX\n", comprop.dwProvSubType);
  DEBUG1 ("comprop.dwProvCapabilities: %08lX\n", comprop.dwProvCapabilities);
  DEBUG1 ("comprop.dwSettableParams:   %08lX\n", comprop.dwSettableParams);
  DEBUG1 ("comprop.dwCurrentRxQueue:   %lu\n",   comprop.dwCurrentRxQueue);
  DEBUG1 ("comprop.dwCurrentTxQueue:   %lu\n",   comprop.dwCurrentTxQueue);

  memset (&comstat, '\0', sizeof(comstat));
  if (!ClearCommError(handle, NULL, &comstat))
  {
    LOG_STDERR ("ClearCommError() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  DEBUG1 ("comstat.fCtsHold:   %d\n", comstat.fCtsHold);
  DEBUG1 ("comstat.fDsrHold:   %d\n", comstat.fDsrHold);
  DEBUG1 ("comstat.fRlsdHold:  %d\n", comstat.fRlsdHold);
  DEBUG1 ("comstat.fEof:       %d\n", comstat.fEof);
  DEBUG1 ("comstat.fTxim:      %d\n", comstat.fTxim);

  /* Setup baudrate and other communication settings
   */
  memset (&dcb, '\0', sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);

  if (!GetCommState(handle, &dcb))
  {
    LOG_STDERR ("GetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  /* Save for COM_exit()
   */
  memcpy (&g_data.COM.old_DCB, &dcb, sizeof(g_data.COM.old_DCB));

  /* Set the new data
   */
  dcb.DCBlength = sizeof(dcb);
  dcb.BaudRate  = g_data.COM.baud_rate > 0 ? g_data.COM.baud_rate : COM_BAUD_RATE;
  dcb.ByteSize  = 8;
  dcb.StopBits  = ONESTOPBIT;
  dcb.Parity    = NOPARITY;
  dcb.fParity   = 0;
  dcb.fNull     = FALSE;
  dcb.fBinary   = TRUE;

  /* Set RTS + DTR flow-control
   */
  dcb.fRtsControl  = RTS_CONTROL_ENABLE;  /* Or 'RTS_CONTROL_HANDSHAKE'? */
  dcb.fDtrControl  = DTR_CONTROL_ENABLE;  /* Or 'DTR_CONTROL_HANDSHAKE'? */
//dcb.fDsrSensitivity = TRUE;
  dcb.fOutxDsrFlow = TRUE;

  /* Set the new DCB structure
   */
  if (!SetCommState(handle, &dcb))
  {
    LOG_STDERR ("SetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  memset (&cto, '\0', sizeof(cto));
  GetCommTimeouts (handle, &cto);

  /* Save for COM_exit()
   */
  memcpy (&g_data.COM.old_CTO, &cto, sizeof(g_data.COM.old_CTO));

  /* Change read timeout
   */
#if 0
  cto.ReadIntervalTimeout        = 1;
  cto.ReadTotalTimeoutMultiplier = 0;
  cto.ReadTotalTimeoutConstant   = 1;
#else
  cto.ReadIntervalTimeout        = MAXDWORD;
//cto.ReadTotalTimeoutMultiplier = MAXDWORD;
//cto.ReadTotalTimeoutConstant   = GNS_HULC_SLEEP / 2;
#endif

  cto.WriteTotalTimeoutMultiplier = 1000;
  cto.WriteTotalTimeoutConstant   = 1000;

  if (!SetCommTimeouts(handle, &cto))
  {
    LOG_STDERR ("SetCommTimeouts() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  DEBUG2 ("cto.ReadTotalTimeoutMultiplier:  %lu\n", cto.ReadTotalTimeoutMultiplier);
  DEBUG2 ("cto.ReadTotalTimeoutConstant:    %lu\n", cto.ReadTotalTimeoutConstant);
  DEBUG2 ("cto.WriteTotalTimeoutMultiplier: %lu\n", cto.WriteTotalTimeoutMultiplier);
  DEBUG2 ("cto.WriteTotalTimeoutConstant:   %lu\n", cto.WriteTotalTimeoutConstant);

  mask = 0;
  if (!GetCommMask(handle, &mask))
  {
    LOG_STDERR ("GetCommMask() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  if (!SetCommMask(handle, EV_RXCHAR | EV_TXEMPTY | EV_ERR))
  {
    LOG_STDERR ("SetCommMask() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  DWORD bits1 = 0;
  DWORD bits2 = 0;
  DWORD bytes = 0;
  char bits_str [200] = "";

  GetCommModemStatus (handle, &bits1);

  if (bits1 & MS_CTS_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_CTS|");
  if (bits1 & MS_DSR_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_DSR|");
  if (bits1 & MS_RLSD_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_DCD|");
  if (bits1 & MS_RING_ON)
     strcat_s (bits_str, sizeof(bits_str), "SP_SIG_RI|");

  if (DeviceIoControl(handle, IOCTL_SERIAL_GET_DTRRTS, NULL, 0,
                      &bits2, sizeof(bits2), &bytes, NULL))
  {
    if (bits2 & SERIAL_DTR_STATE)
       strcat_s (bits_str, sizeof(bits_str), "DTR_STATE|");
    if (bits2 & SERIAL_RTS_STATE)
       strcat_s (bits_str, sizeof(bits_str), "RTS_STATE|");
  }

  if (bits_str[0] == '\0')
     strcpy (bits_str, "<none> ");

  DEBUG1 ("GetCommModemStatus():            %.*s\n",
          (int)strlen(bits_str) - 1, bits_str);

  /* Flush the port and it's I/O buffers just in case
   */
  PurgeComm (handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
  FlushFileBuffers (handle);

  return (true);
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
 *   Detect a "stuck device". Like using a COM-port over a
 *   BlueTooth device. Unless it's associated with a PAN-network,
 *   a WriteFile() will block forever!
 *
 *   Or use Overlapped IO with a timeout.
 *   But using the `g_data.COM.dead_count` logic seems enought.
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
