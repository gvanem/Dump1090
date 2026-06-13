/**\file    gns-hulc.c
 * \ingroup Devices
 *
 * Stuff for GNS / HULC protocol:
 * serial functions for GNS Electronics' HULC -M smart antenna.
 *
 * \ref https://www.gns-electronics.de/flight-information-for-avionics/
 */
#include "GNS-Hulc/gns-hulc.h"

#define BAUD_RATE 921600

static struct {
       bool     filter_df045;
       bool     filter_df1117;
       bool     mode_AC;
       bool     mlat_timestamp;
       bool     fec;
       bool     crc;
       uint16_t padding;
     } beast_settings;

/**
 * Setup the serial line baudrate, flowcontrol, parity etc.
 */
static bool sio_setup (HANDLE handle)
{
  COMMPROP     comprop;
  COMSTAT      comstat;
  COMMTIMEOUTS cto;
  DCB          dcb;

  /* flush I/O buffers first just in case.
   */
  FlushFileBuffers (handle);

  memset (&comprop, '\0', sizeof(comprop));
  if (!GetCommProperties(handle, &comprop))
  {
    LOG_STDERR ("GetCommProperties() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  memset (&comstat, '\0', sizeof(comstat));
  if (ClearCommError(handle, NULL, &comstat))
  {
    DEBUG (DEBUG_GENERAL2, "comstat.fCtsHold   %d\n", comstat.fCtsHold);
    DEBUG (DEBUG_GENERAL2, "comstat.fDsrHold:  %d\n", comstat.fDsrHold);
    DEBUG (DEBUG_GENERAL2, "comstat.fRlsdHold: %d\n", comstat.fRlsdHold);
    DEBUG (DEBUG_GENERAL2, "comstat.fEof:      %d\n", comstat.fEof);
    DEBUG (DEBUG_GENERAL2, "comstat.fTxim:     %d\n", comstat.fTxim);
    DEBUG (DEBUG_GENERAL2, "comstat.cbInQue:   %lu\n", comstat.cbInQue);
    DEBUG (DEBUG_GENERAL2, "comstat.cbOutQue:  %lu\n\n", comstat.cbOutQue);
  }
  else
  {
    LOG_STDERR ("ClearCommError() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  /* Setup baudrate and other communication settings
   */
  memset (&dcb, '\0', sizeof(dcb));
  if (!GetCommState(handle, &dcb))
  {
    LOG_STDERR ("GetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  /* Set the new data
   */
  dcb.DCBlength = sizeof(dcb);
  dcb.BaudRate  = BAUD_RATE;
  dcb.ByteSize  = 8;
  dcb.StopBits  = ONESTOPBIT;
  dcb.Parity    = NOPARITY;
  dcb.fParity   = 0;
  dcb.fBinary   = TRUE;

  /* Set RTS + DTR flow-control
   */
  dcb.fRtsControl  = RTS_CONTROL_ENABLE;
  dcb.fDtrControl  = DTR_CONTROL_ENABLE;
  dcb.fOutxDsrFlow = TRUE;

  /* Set the new DCB structure
   */
  if (!SetCommState(handle, &dcb))
  {
    LOG_STDERR ("SetCommState() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  memset (&cto, '\0', sizeof(cto));
  if (!GetCommTimeouts(handle, &cto))
  {
    LOG_STDERR ("GetCommTimeouts() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  /* Change read timeout
   */
  cto.ReadIntervalTimeout        = 1;
  cto.ReadTotalTimeoutMultiplier = 0;
  cto.ReadTotalTimeoutConstant   = 1; /* 1 ms */
  if (!SetCommTimeouts(handle, &cto))
  {
    LOG_STDERR ("SetCommTimeouts() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }
  return (true);
}

/**
 * Read from the serial device.
 * Returns immediately if no data is available and never blocks.
 */
int gns_hulc_read (HANDLE hnd, char *data, size_t len)
{
  DWORD bytes_read = 0;

  if (!ReadFile(hnd, data, len, &bytes_read, NULL))
     return (-1);
  Modes.stat.gns_hulc_rx_bytes += bytes_read;
  return (bytes_read);
}

/**
 * Write to the serial device.
 */
int gns_hulc_write (HANDLE hnd, const char *buf, size_t len)
{
  DWORD bytes_written = 0;

  if (!WriteFile(hnd, buf, len, &bytes_written, NULL))
     return (-1);

  Modes.stat.gns_hulc_tx_bytes += bytes_written;
  return (bytes_written);
}

/*
 * Try to decode HULC messages. E.g. "1A 48 01" is the periodic status message:
 *
 */
void gns_hulc_hexdump (const unsigned char *buf, size_t len, const char *in_out, const char *file, unsigned line)
{
  size_t i, count, ofs;
  char   hex_digits[] = "0123456789ABCDEF";
  char   line_buf [200];
  int    line_idx;

  printf ("\n%s(%u) %s:\n", file , line, in_out);

  for (ofs = 0; len > 0; len -= count, buf += count, ofs += count)
  {
    count = (len > 16) ? 16 : len;
    line_idx = snprintf (line_buf, sizeof(line_buf), "%4.4X  ", (int)ofs);

    for (i = 0; i < count; i++)
    {
      line_buf [line_idx++] = hex_digits [buf[i] >> 4];
      line_buf [line_idx++] = hex_digits [buf[i] & 15];
      line_buf [line_idx++] = ' ';
    }

    for ( ; i < 16; i++) /* pad with spaces */
    {
      line_buf [line_idx++] = ' ';
      line_buf [line_idx++] = ' ';
      line_buf [line_idx++] = ' ';
    }
    line_buf [line_idx++] = ' ';

    for (i = 0; i < count; i++)
    {
      if (!isprint(buf[i]) || buf[i] == '\t')
           line_buf [line_idx++] = '.';
      else line_buf [line_idx++] = buf [i];
    }
    line_buf [line_idx++] = '\0';
    puts (line_buf);
  }
}

static int send_option (HANDLE hnd, const char *msg, size_t msg_sz, const char *what)
{
  int rc = gns_hulc_write (hnd, msg, msg_sz);

  if (rc != msg_sz)
     LOG_STDERR ("%s failed: %s\n", what, win_strerror(GetLastError()));

  if (Modes.debug)
     GNS_HULC_HEXDUMP (msg, msg_sz, "send");
  return (rc);
}

static int send_beast_option (HANDLE hnd, char opt)
{
  char msg [3] = { 0x1A, '1', opt };
  return send_option (hnd, msg, sizeof(msg), "send_beast_option()");
}

HANDLE gns_hulc_init (uint16_t port, bool beast_enable)
{
  HANDLE hnd;
  char   device [256];

  snprintf (device, sizeof(device), "\\\\.\\COM%d", port); /* e.g. "\\.\COM3" */

  hnd = CreateFileA (device, GENERIC_READ | GENERIC_WRITE,
                     0, NULL, OPEN_EXISTING, 0, NULL);
  if (hnd == INVALID_HANDLE_VALUE)
  {
    LOG_STDERR ("Error opening %s: %s\n", device, win_strerror(GetLastError()));
    return (NULL);
  }

  if (!sio_setup(hnd))
  {
    LOG_STDERR ("Setup for %s failed: %s\n", device, win_strerror(GetLastError()));
    return (NULL);
  }

  if (beast_enable)
  {
    LOG_STDERR ("Running Mode-S Beast via COM%u.\n", port);

    beast_settings.filter_df045   = false;
    beast_settings.filter_df1117  = false;
    beast_settings.mode_AC        = Modes.mode_AC = false;
    beast_settings.mlat_timestamp = true;
    beast_settings.fec            = true;
    beast_settings.crc            = true;

    /* set options */
    send_beast_option (hnd, 'B');         /* set classic beast mode */
    send_beast_option (hnd, 'C');         /* use binary format */
    send_beast_option (hnd, 'H');         /* RTS enabled */

    if (beast_settings.filter_df1117)
         send_beast_option (hnd, 'D');    /* enable DF11/17-only filter*/
    else send_beast_option (hnd, 'd');    /* disable DF11/17-only filter, deliver all messages */

    if (beast_settings.mlat_timestamp)
         send_beast_option (hnd, 'E');    /* enable mlat timestamps */
    else send_beast_option (hnd, 'e');    /* disable mlat timestamps */

    if (beast_settings.crc)
         send_beast_option (hnd, 'f');    /* enable CRC checks */
    else send_beast_option (hnd, 'F');    /* disable CRC checks */

    if (beast_settings.filter_df045)
         send_beast_option (hnd, 'G');    /* enable DF0/4/5 filter */
    else send_beast_option (hnd, 'g');    /* disable DF0/4/5 filter, deliver all messages */

    if (beast_settings.fec)
         send_beast_option (hnd, 'i');    /* FEC enabled */
    else send_beast_option (hnd, 'I');    /* FEC disbled */

    if (beast_settings.mode_AC)
         send_beast_option (hnd, 'J');    /* Mode A/C enabled */
    else send_beast_option (hnd, 'j');    /* Mode A/C disabled */
  }
  else
  {
    LOG_STDERR ("Running GNS HULC via COM%u.\n", port);
    /*
     * Request firmware message from GNS HULC
     */
    send_option (hnd, "#00\r\n", 5, "GNS-HULC firmware request");
  }
  return (hnd);
}

int gns_hulc_exit (HANDLE hnd)
{
  if (hnd != INVALID_HANDLE_VALUE)
  {
    Sleep (200);
    CloseHandle (hnd);
  }
  return (0);
}
