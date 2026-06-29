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
bool COM_setup (HANDLE handle)
{
  COMMPROP     comprop;
  COMSTAT      comstat;
  COMMTIMEOUTS cto;
  DCB          dcb;
  DWORD        mask;

  memset (&comprop, '\0', sizeof(comprop));
  if (!GetCommProperties(handle, &comprop))
  {
    LOG_STDERR ("GetCommProperties() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  memset (&comstat, '\0', sizeof(comstat));
  if (!ClearCommError(handle, NULL, &comstat))
  {
    LOG_STDERR ("ClearCommError() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  DEBUG2 ("comstat.fCtsHold:   %d\n", comstat.fCtsHold);
  DEBUG2 ("comstat.fDsrHold:   %d\n", comstat.fDsrHold);
  DEBUG2 ("comstat.fRlsdHold:  %d\n", comstat.fRlsdHold);
  DEBUG2 ("comstat.fEof:       %d\n", comstat.fEof);
  DEBUG2 ("comstat.fTxim:      %d\n", comstat.fTxim);

  /* Setup baudrate and other communication settings
   */
  memset (&dcb, '\0', sizeof(dcb));
  if (!GetCommState(handle, &dcb))
  {
    LOG_STDERR ("GetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

#if 0 // todo
  snprintf (dcb_spec, sizeof(dcb_spec), "baud=%d parity=%c data=%d stop=%d",
            baud, parity, databits, stopbits);
  BuildCommDCB (dcb_spec, &dcb);
#endif

  /* Set the new data
   */
  dcb.DCBlength = sizeof(dcb);
  dcb.BaudRate  = g_data.COM.baud_rate > 0 ? g_data.COM.baud_rate : COM_BAUD_RATE;
  dcb.ByteSize  = 8;
  dcb.StopBits  = ONESTOPBIT;
  dcb.Parity    = NOPARITY;
  dcb.fParity   = 0;
  dcb.fBinary   = TRUE;

  /* Set RTS + DTR flow-control
   */
  dcb.fRtsControl  = g_data.COM.RTS_ctrl;
  dcb.fDtrControl  = g_data.COM.DTR_ctrl;
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

  /* Change read/write timeout
   */
  cto.ReadIntervalTimeout        = 1;
  cto.ReadTotalTimeoutMultiplier = 0;
  cto.ReadTotalTimeoutConstant   = 1;

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

  DWORD bits = 0;

  if (!GetCommModemStatus(handle, &bits))
  {
    LOG_STDERR ("GetCommModemStatus() failed: %s\n", win_strerror(GetLastError()));
 // return (false);  /* Ignore this error */
  }
  else
  {
    char bits_str [100] = "";

    if (bits & MS_CTS_ON)
       strcat_s (bits_str, sizeof(bits_str), "SP_SIG_CTS|");
    if (bits & MS_DSR_ON)
       strcat_s (bits_str, sizeof(bits_str), "SP_SIG_DSR|");
    if (bits & MS_RLSD_ON)
       strcat_s (bits_str, sizeof(bits_str), "SP_SIG_DCD|");
    if (bits & MS_RING_ON)
       strcat_s (bits_str, sizeof(bits_str), "SP_SIG_RI|");

     if (bits_str[0] == '\0')
        strcpy (bits_str, "<none> ");

     DEBUG2 ("GetCommModemStatus():            %.*s\n",
            (int)strlen(bits_str) - 1, bits_str);
  }

  /* Flush the port and it's I/O buffers just in case
   */
  PurgeComm (handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
  FlushFileBuffers (handle);

  return (true);
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

#if 0
/**
 * Fill up `g_data.sio_buf` with fresh data
 */
static int COM_fill_buf (void)
{
  int    len = 0;
  size_t space;

  /* Contiguous free space in g_data.sio_buf[]
   */
  if (g_data.rx_head < g_data.rx_tail)
       space = (g_data.rx_tail - g_data.rx_head);
  else if (g_data.rx_tail > 0)
       space = COM_RX_SIZE - g_data.rx_head;
  else space = COM_RX_SIZE - g_data.rx_head;

  assert (space >= 0 && space <= COM_RX_SIZE);

  if (space > 0)
  {
    size_t   head, old_rx_head, i;
    uint8_t *rx_buf = alloca (COM_RX_SIZE);

    len = COM_read (Modes.gns_hulc.handle, rx_buf, space);
    if (len < 0)
    {
      Modes.exit = true;
      return (-1);
    }

    /* Copy 'rx_buf[]' into 'g_data.sio_buf + g_data.rx_head' one byte at a time.
     * Update the rx-buf head as we go along.
     * The inverse of COM_getch().
     */
    head = g_data.rx_head + space;
    old_rx_head = head;

    for (i = 0; i < len; i++, head++)
    {
      if (head >= COM_RX_SIZE)
         head -= COM_RX_SIZE;
      g_data.sio_buf [head] = rx_buf [i];
    }

    g_data.rx_head = head;
    DEBUG1 ("ch: --, %s(): space: %zd, old_rx_head: %zd, rx_head: %zd\n", __FUNCTION__, space, old_rx_head, g_data.rx_head);
  }
  return (len);
}

int COM_getch (void)
{
  int rc;

  if (g_data.rx_head != g_data.rx_tail)
  {
    rc = g_data.sio_buf [g_data.rx_tail++];
    if (g_data.rx_tail >= COM_RX_SIZE)
       g_data.rx_tail = 0;
  }
  else
  {
    rc = COM_fill_buf();
    if (rc > 0)
          g_data.rx_head = rc;
     else return (-1);   /* sio_buf[] still empty */
  }

   // ... etc.

  return (rc);
}
#endif  // 0
