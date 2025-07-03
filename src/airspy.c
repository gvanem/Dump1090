/**
 * \file    airspy.c
 * \ingroup Samplers
 * \brief   The interface for AirSpy devices.
 *
 * Load all needed functions from "airspy.dll" dynamically.
 */
#define INSIDE_AIRSPY_C
#include "misc.h"
#include "airspy.h"
#include <AirSpy/airspy.h>

#define MODES_BUF_SIZE   (256*1024)   /**< 256k, same as MODES_ASYNC_BUF_SIZE */
#define MODES_BUFFERS     16          /**< Must be power of 2 */

#define AIRSPY_ACC_SHIFT  13          /**< Sets time constant of averaging filter */

#define SAMPLE_TYPE      uint16_t
#define SAMPLE_TYPE_STR "uint16_t"

/**
 * \def CALL_FUNC()
 *
 * Call *all* AirSpy functions via this macro. This is for tracing and
 * to ensure any error gets stored to `sdr.last_err`.
 */
#define CALL_FUNC(func, ...)                                     \
        do {                                                     \
          airspy_error rc = (*p_ ## func) (__VA_ARGS__);         \
          if (rc != AIRSPY_SUCCESS)                              \
          {                                                      \
            airspy_store_error (rc);                             \
            TRACE ( "%s(): %d / %s\n", #func, rc, sdr.last_err); \
          }                                                      \
          else                                                   \
          {                                                      \
            airspy_clear_error();                                \
            TRACE ( "%s(): OKAY\n", #func);                      \
          }                                                      \
        } while (0)

/**
 * \typedef struct airspy_priv
 *
 * Data private for AirSpy.
 */
typedef struct airspy_priv {
        mg_file_path                dll_name;
        airspy_lib_version_t        version;
        volatile bool               cancelling;
        volatile bool               uninit_done;
        airspy_device              *devices [4];     /**< 4 should be enough for `p_airspy_list_devices()`. */
        airspy_device              *chosen_dev;      /**< `airspy_select()` sets this to one of the above */
        uint64_t                    serials [4];
        unsigned int                num_devices;
        char                        last_err [256];
        int                         last_rc;
        int                         max_sig, max_sig_acc;
        airspy_sample_block_cb_fn   callbacks;
        uint16_t                   *rx_data;
        uint32_t                    rx_data_idx;
        airspy_cb                   rx_callback;
        void                       *rx_context;
        uint64_t                    rx_num_callbacks;

      } airspy_priv;

static airspy_priv sdr;

/* 4 - 44 dB
 */
static int gain_table [10] = { 40, 100, 150, 170, 210, 260, 310, 350, 390, 440 };

/**
 * Load and use the AirSpy-API dynamically.
 */
#define ADD_FUNC(func)  { false, NULL, NULL, #func, (void**) &p_ ##func }

static struct dyn_struct airspy_funcs [] = {
              ADD_FUNC (airspy_open),
              ADD_FUNC (airspy_close),
              ADD_FUNC (airspy_init),
              ADD_FUNC (airspy_exit),
              ADD_FUNC (airspy_set_freq),
              ADD_FUNC (airspy_set_lna_gain),
              ADD_FUNC (airspy_set_mixer_gain),
              ADD_FUNC (airspy_set_linearity_gain),
              ADD_FUNC (airspy_set_sensitivity_gain),
              ADD_FUNC (airspy_set_vga_gain),
              ADD_FUNC (airspy_set_lna_agc),
              ADD_FUNC (airspy_set_mixer_agc),
              ADD_FUNC (airspy_set_rf_bias),
              ADD_FUNC (airspy_is_streaming),
              ADD_FUNC (airspy_stop_rx),
              ADD_FUNC (airspy_lib_version),
              ADD_FUNC (airspy_list_devices),
              ADD_FUNC (airspy_error_name)
          };

#undef ADD_FUNC

/**
 * Load the `airspy.dll` from a specific location or let `LoadLibraryA()`
 * search along the `%PATH%`.
 *
 * The `airspy-dll` value in `dump1090.cfg` is empty by default and
 * `Modes.airspy.dll_name` equals `airspy.dll`. Hence `LoadLibraryA()`
 * will search along the `%PATH%`.
 *
 * But the config-callback `airspy_set_dll_name()` could have set another
 * custom value.
 */
static bool airspy_load_funcs (void)
{
  mg_file_path full_name;
  size_t       i, num;

  for (i = 0; i < DIM(airspy_funcs); i++)
      airspy_funcs[i].mod_name = Modes.airspy.dll_name;

  SetLastError (0);

  num = load_dynamic_table (airspy_funcs, DIM(airspy_funcs));
  if (num < DIM(airspy_funcs) - 1 || !airspy_funcs[0].mod_handle)
  {
    DWORD err = GetLastError();

    /**
     * The `LoadLibraryA()` above will fail with `err == ERROR_BAD_EXE_FORMAT` (193)
     * if we're running a 32-bit Dump1090 and we loaded a 64-bit "airspy.dll".
     * And vice-versa.
     */
    if (err == ERROR_BAD_EXE_FORMAT)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "\"%s\" is not a %d bit DLL", Modes.airspy.dll_name, 8*(int)sizeof(void*));
    else if (err == ERROR_MOD_NOT_FOUND)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "\"%s\" not found on PATH", Modes.airspy.dll_name);
    else snprintf (sdr.last_err, sizeof(sdr.last_err), "Failed to load \"%s\"; %s", Modes.airspy.dll_name, win_strerror(err));
    return (false);
  }

  if (!GetModuleFileNameA(airspy_funcs[0].mod_handle, full_name, sizeof(full_name)))
     strcpy (full_name, "?");

  /* These 2 names better be the same
   */
  TRACE ("full_name: '%s'\n", full_name);
  TRACE ("dll_name:  '%s'\n", Modes.airspy.dll_name);
  return (true);
}

/**
 * Store the last error-code and error-text from the
 * last failed `CALL_FUNC()` macro call.
 */
static void airspy_store_error (airspy_error rc)
{
  sdr.last_rc = rc;

  if (p_airspy_error_name)
       strcpy_s (sdr.last_err, sizeof(sdr.last_err), (*p_airspy_error_name)(rc));
  else sdr.last_err[0] = '\0';
}

/**
 * Clear any last error-codes and error-text from the
 * last successful `CALL_FUNC()` macro call.
 */
static void airspy_clear_error (void)
{
  sdr.last_rc = AIRSPY_SUCCESS;
  strcpy (sdr.last_err, "none");
}

/**
 * The AirSpy event callback.
 *
 * 16-bit data is received from RSP at 2MHz. It is interleaved into a circular buffer.
 * Each time the pointer passes a multiple of `MODES_BUF_SIZE`, that segment of
 * buffer is handed off to the callback-routine `rx_callback()` in `dump1090.c`.
 *
 * For each packet from the RSP, the maximum `I` signal value is recorded.
 * This is entered into a slow, exponentially decaying filter. The output from this filter
 * is occasionally checked and a decision made whether to step the RSP gain by
 * plus or minus 1 dB.
 */
static void airspy_event_callback (int event_id, void *cb_context)
{
  if (sdr.cancelling || Modes.exit)
     return;

  EnterCriticalSection (&Modes.print_mutex);

  switch (event_id)
  {
  }

  LeaveCriticalSection (&Modes.print_mutex);
  MODES_NOTUSED (cb_context);
}

/**
 * The main AirSpy stream callback.
 */
static void airspy_callback_A (short        *xi,
                               short        *xq,
                               unsigned int  num_samples,
                               unsigned int  reset,
                               void         *cb_context)
{
  int          i, count1, count2;
  int          sig_I, sig_Q, max_sig;
  bool         new_buf_flag;
  uint32_t     end, input_index;
  uint32_t     rx_data_idx = sdr.rx_data_idx;
  int          max_sig_acc = sdr.max_sig;
  SAMPLE_TYPE *dptr = (SAMPLE_TYPE*) sdr.rx_data;

  /**
   * `count1` is lesser of input samples and samples to end of buffer.
   * `count2` is the remainder, generally zero
   */

  end = rx_data_idx + (num_samples << 1);
  count2 = end - (MODES_BUF_SIZE * MODES_BUFFERS);
  if (count2 < 0)
     count2 = 0;            /* count2 is samples wrapping around to start of buf */

  count1 = (num_samples << 1) - count2;   /* count1 is samples fitting before the end of buf */

  /* Flag is set if this packet takes us past a multiple of MODES_BUF_SIZE
   */
  new_buf_flag = ((rx_data_idx & (MODES_BUF_SIZE-1)) < (end & (MODES_BUF_SIZE-1))) ? false : true;

  /* Now interleave data from I/Q into circular buffer, and note max I value
   */
  input_index = 0;
  max_sig = 0;

  for (i = (count1 >> 1) - 1; i >= 0; i--)
  {
    sig_I = xi [input_index];
    dptr [rx_data_idx++] = sig_I;

    sig_Q = xq [input_index++];
    dptr [rx_data_idx++] = sig_Q;

    if (sig_I > max_sig)
       max_sig = sig_I;
  }

  /* Apply slowly decaying filter to max signal value
   */
  max_sig -= 127;
  max_sig_acc += max_sig;
  max_sig = max_sig_acc >> AIRSPY_ACC_SHIFT;
  max_sig_acc -= max_sig;

  /* This code is triggered as we reach the end of our circular buffer
   */
  if (rx_data_idx >= (MODES_BUF_SIZE * MODES_BUFFERS))
  {
    rx_data_idx = 0;  /* pointer back to start of buffer */
  }

  /* Insert any remaining signal at start of buffer
   */
  for (i = (count2 >> 1) - 1; i >= 0; i--)
  {
    sig_I = xi [input_index];
    dptr [rx_data_idx++] = sig_I;

    sig_Q = xq [input_index++];
    dptr [rx_data_idx++] = sig_Q;
  }

  /* Send buffer downstream if enough available
   */
  if (new_buf_flag)
  {
    /* Go back by one buffer length, then round down further to start of buffer
     */
    end = rx_data_idx + MODES_BUF_SIZE * (MODES_BUFFERS-1);
    end &= (MODES_BUF_SIZE * MODES_BUFFERS) - 1;
    end &= ~(MODES_BUF_SIZE - 1);

    sdr.rx_num_callbacks++;
    (*sdr.rx_callback) ((uint8_t*)sdr.rx_data + end, MODES_BUF_SIZE, sdr.rx_context);
  }

  /* Stash static values in `sdr` struct
   */
  sdr.max_sig     = max_sig_acc;
  sdr.rx_data_idx = rx_data_idx;

  MODES_NOTUSED (reset);
  MODES_NOTUSED (cb_context);
}

/**
 * Select an AirSpy device on name of index.
 */
static bool airspy_select (int wanted_index)
{
  airspy_device *device;
  int     i, select_this = -1;
  char    current_dev [100];
  bool    select_first = true;

  if (wanted_index != -1)
     select_first = false;

  sdr.num_devices = (*p_airspy_list_devices) (sdr.serials, DIM(sdr.serials));
  if (sdr.num_devices == 0)
  {
    LOG_STDERR ("No AirSpy devices found.\n");
    return (false);
  }

  device = sdr.devices [0];  /**< start checking the 1st device returned */

  TRACE ("wanted_index: %d. Found %d devices\n", wanted_index, sdr.num_devices);

  for (i = 0; i < (int)sdr.num_devices; i++)
  {
    TRACE ("Device Index %d: %s - SerialNumber = %llu\n", i, current_dev, sdr.serials[i]);

    if (select_this == -1)
    {
      if (select_first)
         select_this = i;
      else if (i == wanted_index)
         select_this = i;
    }
  }

  if (select_this == -1)
  {
    LOG_STDERR ("airspy device at index: %d not found.\n", wanted_index);
    return (false);
  }

  CALL_FUNC (airspy_open, &device);
  if (sdr.last_rc != AIRSPY_SUCCESS)
     return (false);

  sdr.chosen_dev = device;        /**< we only support 1 device */

  Modes.selected_dev = mg_mprintf ("airspy-%s", current_dev);
  return (true);
}

/**
 * \brief Reads samples from the AirSpy DLL.
 *
 * This routine should be called from the main application in a separate thread.
 *
 * It enters an infinite loop only returning when the main application sets
 * the stop-condition specified in the `context`.
 *
 * \param[in] device   The device handle which is ignored. Since it's already
 *                     retured in `airspy_init()` (we support only one device at a time.
 *                     But check for a NULL-device just in case).
 * \param[in] callback The address of the receiver callback.
 * \param[in] context  The address of the "stop-variable".
 * \param[in] buf_num  The number of buffers to use (ignored for now).
 * \param[in] buf_len  The length of each buffer to use (ignored for now).
 */
int airspy_read_async (airspy_dev *device,
                       airspy_cb   callback,
                       void       *context,
                       uint32_t    buf_num,
                       uint32_t    buf_len)
{
  MODES_NOTUSED (callback);
  MODES_NOTUSED (context);
  MODES_NOTUSED (buf_num);
  MODES_NOTUSED (buf_len);

  if (!device || device != sdr.chosen_dev)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No device");
    sdr.last_rc = AIRSPY_ERROR_OTHER;
    return (sdr.last_rc);
  }

  if (sdr.last_rc != AIRSPY_SUCCESS)
     return (sdr.last_rc);

  while (1)
  {
    Sleep (1000);
    if (*(volatile bool*) sdr.rx_context)
    {
      TRACE ("'exit' was set\n");
      break;
    }
    TRACE ("rx_num_callbacks: %llu, sdr.max_sig: %6d, sdr.rx_data_idx: %6u\n",
           sdr.rx_num_callbacks, sdr.max_sig, sdr.rx_data_idx);
  }
  return (0);
}

/**
 *
 */
int airspy_set_gain (airspy_dev *device, int gain)
{
  LOG_FILEONLY ("gain: %.1f dB\n", (double)gain / 10);
  MODES_NOTUSED (device);
  return (0);
}

/**
 * \brief Cancels the callbacks from the AirSpy DLL.
 *
 * Force the `airspy_read_async()` to stop and return from it's loop.
 * Called in the `SIGINT` handler in dump1090.c.
 */
int airspy_cancel_async (airspy_dev *device)
{
  if (device != sdr.chosen_dev)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No device");
    sdr.last_rc = AIRSPY_ERROR_OTHER;
  }
  else if (sdr.cancelling)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "Cancelling");
    sdr.last_rc = AIRSPY_ERROR_STREAMING_STOPPED;
  }
  else if (!sdr.uninit_done)
  {
    CALL_FUNC (airspy_stop_rx, sdr.chosen_dev);
    sdr.cancelling = sdr.uninit_done = true;
  }
  return (sdr.last_rc);
}

/**
 * Returns the last error set.
 * Called from the outside only.
 */
const char *airspy_strerror (int rc)
{
  if (sdr.last_rc == -1)
     return ("<unknown>");
  if (rc == 0 || !sdr.last_err[0])
     return ("<success>");
  return (sdr.last_err);
}

/**
 * Load all needed AirSpy DLL functions dynamically
 * from `Modes.airspy.dll_name`.
 */
int airspy_init (const char *name, int index, airspy_dev **device)
{
  MODES_NOTUSED (name);  /* possible to select on name? */
  *device = NULL;

  TRACE ("index: %d\n", index);

  sdr.chosen_dev = NULL;
  sdr.last_rc    = -1;  /* no idea yet */

  sdr.cancelling = false;

#if 0  // + a lot more
  Modes.airspy.antenna_port    = airspy_api_Rsp2_ANTENNA_B;
  Modes.airspy.DX_antenna_port = airspy_api_RspDx_ANTENNA_B;
  Modes.airspy.BW_mode         = 1;  /* 5 MHz */
  Modes.airspy.over_sample     = true;
#endif

  sdr.rx_data = malloc (MODES_BUF_SIZE * MODES_BUFFERS * sizeof(short));
  if (!sdr.rx_data)
     goto nomem;

  Modes.airspy.gains = malloc (10 * sizeof(int));
  if (!Modes.airspy.gains)
      goto nomem;

  Modes.airspy.gain_count = 10;
  memcpy (Modes.airspy.gains, &gain_table, 10 * sizeof(int));

  if (!airspy_load_funcs())
     goto failed;

  CALL_FUNC (airspy_init);
  if (sdr.last_rc != AIRSPY_SUCCESS)
  {
    fprintf (stderr, "The AirSpy DLL failed\n");
    goto failed;
  }

  (*p_airspy_lib_version) (&sdr.version);

  if (!airspy_select(index))
     goto failed;

  *device = sdr.chosen_dev;

  return (AIRSPY_SUCCESS);

nomem:
  strcpy_s (sdr.last_err, sizeof(sdr.last_err), "Insufficient memory");
  sdr.last_rc = ENOMEM;       /* fall through to 'failed' */

failed:
  LOG_STDERR ("%s\n", sdr.last_err);
  airspy_exit (NULL);
  return (AIRSPY_ERROR_OTHER);  /* A better error-code? */
}

/**
 * Free the API and the device.
 */
static int airspy_release (airspy_dev *device)
{
  if (device != sdr.chosen_dev)    /* support only 1 device */
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No device");
    sdr.last_rc = AIRSPY_ERROR_OTHER;
  }
  else
  {
    if (!sdr.cancelling)
    {
      CALL_FUNC (airspy_close, sdr.chosen_dev);
      sdr.uninit_done = true;
    }
  }

  sdr.chosen_dev = NULL;
  return (sdr.last_rc);
}

/**
 * Exit-function for this module:
 *  \li Release the device.
 *  \li Unload the handle of `Modes.airspy.dll_name`.
 */
int airspy_exit (airspy_dev *device)
{
  if (device)
     airspy_release (device);

  free (sdr.rx_data);
  sdr.rx_data = NULL;

  if (!airspy_funcs[0].mod_handle)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No DLL loaded");
    sdr.last_rc = AIRSPY_ERROR_OTHER;
  }
  else
  {
    CALL_FUNC (airspy_close, sdr.chosen_dev);
    unload_dynamic_table (airspy_funcs, DIM(airspy_funcs));
  }
  sdr.chosen_dev = NULL;
  return (sdr.last_rc);
}

/**
 * Config-parser callback; <br>
 * parses "airspy-dll" and sets `Modes.airspy.dll_name`.
 */
bool airspy_set_dll_name (const char *arg)
{
  mg_file_path dll = "?";
  DWORD        len, attr;

  if (!strpbrk(arg, "/\\"))  /* not absolute or relative; assume on PATH */
  {
    len = SearchPath (getenv("PATH"), arg, NULL, sizeof(dll), dll, NULL);

    TRACE ("dll: '%s', len: %lu\n", dll, len);
    if (len > 0)
         strcpy_s (Modes.airspy.dll_name, sizeof(Modes.airspy.dll_name), dll);
    else strcpy_s (Modes.airspy.dll_name, sizeof(Modes.airspy.dll_name), arg);
    return (true);
  }

  attr = INVALID_FILE_ATTRIBUTES;
  len  = GetFullPathName (arg, sizeof(dll), dll, NULL);

  TRACE ("dll: '%s', len: %lu\n", dll, len);
  if (len > 0)
     attr = GetFileAttributes (dll);
  else if (attr != FILE_ATTRIBUTE_NORMAL)
  {
    LOG_STDERR ("\nThe \"airspy-dll = %s\" was not found. "
                "Using the default \"%s\"\n", arg, Modes.airspy.dll_name);
    return (false);
  }
  strcpy_s (Modes.airspy.dll_name, sizeof(Modes.airspy.dll_name), dll);
  TRACE ("Modes.airspy.dll_name: '%s'\n", Modes.airspy.dll_name);
  return (true);
}

