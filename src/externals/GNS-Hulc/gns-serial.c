/**\file    gns-serial.c
 * \ingroup GNS-HULC
 *
 * Serial stuff for the GNS-HULC protocol.
 */
#define GNS_FILE "gns-serial.c"

#include "GNS-Hulc/gns-private.h"
#include "GNS-Hulc/gns-hulc.h"

#include <setupapi.h>
#include <devguid.h>

static COM_port_info port_info [64];

static int  COM_find_all (COM_port_info *port, int max_ports);
static void COM_print_info (int found, const COM_port_info *port);

/**
 * Find details on all COM-ports on machine.
 * And setup the serial line baudrate, flowcontrol, parity etc for `port`.
 */
HANDLE COM_init (uint16_t port)
{
  HANDLE hnd;
  int    i, count;

  memset (&port_info, '\0', sizeof(port_info));
  count = COM_find_all (port_info, DIM(port_info));

  if (Modes.debug & DEBUG_GNS_HULC)
     COM_print_info (count, port_info);

  /* Iterate over `port_info` to find `port_info[i].port_number == port`.
   */
  for (i = 0; i < count; i++)
  {
    if (port_info[i].port_number != port)
       continue;
    g_data.COM.info     = port_info + i;
    g_data.COM.dev_name = mg_mprintf ("\\\\.\\%s", g_data.COM.info->port_name);  /* "\\.\COMx" */
    break;
  }
  if (!g_data.COM.dev_name)
  {
    LOG_STDERR ("No such device COM%u\n", port);
    return (NULL);
  }

  DEBUG1 ("Modes.gns_hulc.port:  %u\n", Modes.gns_hulc.port);
  DEBUG1 ("Modes.gns_hulc.name:  %s\n", Modes.gns_hulc.name ? Modes.gns_hulc.name : "<none>");
  DEBUG2 ("Modes.selected_dev:  '%s'\n", Modes.selected_dev ? Modes.selected_dev  : "<none>");

  hnd = CreateFileA (g_data.COM.dev_name, GENERIC_READ | GENERIC_WRITE,
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
  memset (&port_info, '\0', sizeof(port_info));
  g_data.COM.info = NULL;
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
  DEBUG1 ("WaitCommEvent(): %s, %.0f usec\n", ev_str, get_usec_now() - start);
  if (events & EV_ERR)
     COM_poll_error();

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
    DEBUG1 ("CE_OVERRUN\n");
  }
}

/**
 * Print information on COM-devices found from 0 to `found-1`.
 */
static void COM_print_info (int found, const COM_port_info *port)
{
  int i;

  printf ("Found %d COM port(s):\n", found);
  for (i = 0; i < found; i++, port++)
  {
    printf ("  Port-name:    %s\n", port->port_name);
    printf ("  Description:  %s\n", port->description);
    printf ("  Manufacturer: %s\n", port->manufacturer);
    printf ("  Hardware ID:  %s\n", port->hardware_id);
    printf ("  DOS-device:   %s\n", port->DOS_device[0] ? port->DOS_device : "?");
    printf ("  Driver-key:   %s\n", port->driver_key[0] ? port->driver_key : "?");
    if (port->has_VID_PID)
         printf ("  VID:PID:      0x%s:0x%s\n\n", port->vid, port->pid);
    else printf ("  VID:PID:      none (a virtual/legacy port?)\n\n");
  }
}

/**
 * Parse VID_xxxx and PID_xxxx substrings out of a hardware ID string like
 * "USB\VID_0403&PID_6001\SOMESERIAL" or "USB\VID_0403&PID_6001&MI_00".
 *
 * Non-USB ports (e.g. a physical motherboard COM1, or software-only virtual ports)
 * often have a hardware ID like ACPI\PNP0501\... with no VID/PID.
 */
static void parse_VID_PID (const char *hw_id, char *vid_out, size_t vid_len,
                           char *pid_out, size_t pid_len, bool *found)
{
  const char *vid_pos = strstr (hw_id, "VID_");
  const char *pid_pos = strstr (hw_id, "PID_");
  size_t      i = 0;

  *found = false;
  vid_out [0] = '\0';
  pid_out [0] = '\0';

  if (!vid_pos || !pid_pos)
     return;

  vid_pos += 4; /* skip "VID_" */
  pid_pos += 4; /* skip "PID_" */

  while (vid_pos[i] && vid_pos[i] != '&' && vid_pos[i] != '\\' && i < vid_len - 1)
  {
    vid_out [i] = vid_pos [i];
    i++;
  }
  vid_out [i] = '\0';

  i = 0;
  while (pid_pos[i] && pid_pos[i] != '&' && pid_pos[i] != '\\' && i < pid_len - 1)
  {
    pid_out [i] = pid_pos [i];
    i++;
  }
  pid_out [i] = '\0';

  if (vid_out[0] && pid_out[0])
     *found = true;
}

/**
 * Find information on COM-devices from 0 to `max_ports`.
 */
static int COM_find_all (COM_port_info *port, int max_ports)
{
  SP_DEVINFO_DATA dev_info_data;
  HDEVINFO        dev_info;
  int             i, count = 0;

  /* GUID_DEVCLASS_PORTS gets a device info set for all present port devices (COM and LPT).
   */
  dev_info = SetupDiGetClassDevsA (&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
  if (dev_info == INVALID_HANDLE_VALUE)
     return (0);

  dev_info_data.cbSize = sizeof(dev_info_data);

  /* Iterates through each device.
   */
  for (i = 0; SetupDiEnumDeviceInfo(dev_info, (DWORD)i, &dev_info_data); i++)
  {
    if (count >= max_ports)
       break;

    char  friendly_name [256] = "";
    char  description [256]   = "";
    char  manufacturer [256]  = "";
    char  hardware_id [256]   = "";
    char  driver_key [256]    = "";
    DWORD data_type, required_size;

    SetupDiGetDeviceRegistryPropertyA (dev_info, &dev_info_data,
                                       SPDRP_FRIENDLYNAME, &data_type,
                                       (BYTE*)friendly_name, sizeof(friendly_name),
                                       &required_size);

    SetupDiGetDeviceRegistryPropertyA (dev_info, &dev_info_data,
                                       SPDRP_DEVICEDESC, &data_type,
                                       (BYTE*)description, sizeof(description),
                                       &required_size);

    SetupDiGetDeviceRegistryPropertyA (dev_info, &dev_info_data,
                                       SPDRP_MFG, &data_type,
                                       (BYTE*)manufacturer, sizeof(manufacturer),
                                       &required_size);

    /* Hardware ID can be a REG_MULTI_SZ; the first one is the most
     * specific and is what we want here.
     */
    SetupDiGetDeviceRegistryPropertyA (dev_info, &dev_info_data,
                                       SPDRP_HARDWAREID, &data_type,
                                       (BYTE*)hardware_id, sizeof(hardware_id),
                                       &required_size);

    SetupDiGetDeviceRegistryPropertyA (dev_info, &dev_info_data,
                                       SPDRP_DRIVER, &data_type,
                                       (BYTE*)driver_key, sizeof(driver_key),
                                       &required_size);

    /* Extract "COMx" from the friendly name, e.g. "USB Serial Port (COM5)"
     */
    char port_name [sizeof(friendly_name)+10] = "";
    int  port_number = -1;
    int  num = sscanf (friendly_name, "%256[^()] (COM%d)", port_name, &port_number);

    if (num == 2 && port_number > -1)
    {
      port->port_number = port_number;
      snprintf (port->port_name, sizeof(port->port_name), "COM%d", port->port_number);
    }
    else
      port->port_number = -1;  /* unknown */

    str_lcpy (port->description, description, sizeof(port->description));
    str_lcpy (port->manufacturer, manufacturer, sizeof(port->manufacturer));
    str_lcpy (port->hardware_id, hardware_id, sizeof(port->hardware_id));
    str_lcpy (port->driver_key, driver_key, sizeof(port->driver_key));

    parse_VID_PID (hardware_id, port->vid, sizeof(port->vid),
                   port->pid, sizeof(port->pid), &port->has_VID_PID);

    if (!strncmp(port->port_name, "COM", 3) && port->port_number > -1)
       QueryDosDeviceA (port->port_name, port->DOS_device, sizeof(port->DOS_device));

    count++;
    port++;
  }
  SetupDiDestroyDeviceInfoList (dev_info);
  return (count);
}

