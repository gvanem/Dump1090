/**
 * \file    sdrplay.c
 * \ingroup Samplers
 * \brief   The interface for SDRplay devices.
 *
 * Load all needed functions from "sdrplay_api.dll" dynamically.
 */
#include "misc.h"
#include "sdrplay.h"

#define MODES_RSP_BUF_SIZE   (256*1024)   /**< 256k, same as MODES_ASYNC_BUF_SIZE */
#define MODES_RSP_BUFFERS     16          /**< Must be power of 2 */

#define RSP_MIN_GAIN_THRESH   512         /**< Increase gain if peaks below this */
#define RSP_MAX_GAIN_THRESH  1024         /**< Decrease gain if peaks above this */
#define RSP_ACC_SHIFT          13         /**< Sets time constant of averaging filter */
#define MODES_RSP_INITIAL_GR   20
#define USE_8BIT_SAMPLES        1

#if USE_8BIT_SAMPLES
  #define SAMPLE_TYPE      uint8_t
  #define SAMPLE_TYPE_STR "uint8_t"
#else
  #define SAMPLE_TYPE      uint16_t
  #define SAMPLE_TYPE_STR "uint16_t"
#endif

/**
 * \def CALL_FUNC()
 *
 * Call *all* SDRPlay functions via this macro. This is for tracing and
 * to ensure any error gets stored to `sdr.last_err`.
 */
#define CALL_FUNC(func, ...)                                     \
        do {                                                     \
          sdrplay_api_ErrT rc = (*sdr.func) (__VA_ARGS__);       \
          if (rc != sdrplay_api_Success)                         \
          {                                                      \
            sdrplay_store_error (rc);                            \
            TRACE ( "%s(): %d / %s\n", #func, rc, sdr.last_err); \
          }                                                      \
          else                                                   \
          {                                                      \
            sdrplay_clear_error();                               \
            TRACE ( "%s(): OKAY\n", #func);                      \
          }                                                      \
        } while (0)

/**
 * \typedef struct sdrplay_priv
 *
 * Data private for SDRPlay.
 */
typedef struct sdrplay_priv {
        mg_file_path                   dll_name;
        HANDLE                         handle;
        float                          version;
        bool                           API_locked;
        bool                           master_initialised;
        bool                           slave_uninitialised;
        bool                           slave_attached;
        volatile bool                  cancelling;
        volatile bool                  uninit_done;

        sdrplay_api_DeviceT            devices [4];     /**< 4 should be enough for `sdr::sdrplay_api_GetDevices()`. */
        sdrplay_api_DeviceT           *chosen_dev;      /**< `sdrplay_select()` sets this to one of the above */
        unsigned int                   num_devices;
        char                           last_err [256];
        int                            last_rc;
        int                            max_sig, max_sig_acc;
        sdrplay_api_CallbackFnsT       callbacks;
        sdrplay_api_DeviceParamsT     *dev_params;
        sdrplay_api_RxChannelParamsT  *ch_params;
        uint16_t                      *rx_data;
        uint32_t                       rx_data_idx;
        sdrplay_cb                     rx_callback;
        void                          *rx_context;
        uint64_t                       rx_num_callbacks;

        sdrplay_api_Open_t             sdrplay_api_Open;
        sdrplay_api_Close_t            sdrplay_api_Close;
        sdrplay_api_Init_t             sdrplay_api_Init;
        sdrplay_api_Uninit_t           sdrplay_api_Uninit;
        sdrplay_api_ApiVersion_t       sdrplay_api_ApiVersion;
        sdrplay_api_DebugEnable_t      sdrplay_api_DebugEnable;
        sdrplay_api_LockDeviceApi_t    sdrplay_api_LockDeviceApi;
        sdrplay_api_UnlockDeviceApi_t  sdrplay_api_UnlockDeviceApi;
        sdrplay_api_GetDevices_t       sdrplay_api_GetDevices;
        sdrplay_api_GetDeviceParams_t  sdrplay_api_GetDeviceParams;
        sdrplay_api_SelectDevice_t     sdrplay_api_SelectDevice;
        sdrplay_api_ReleaseDevice_t    sdrplay_api_ReleaseDevice;
        sdrplay_api_Update_t           sdrplay_api_Update;
        sdrplay_api_GetErrorString_t   sdrplay_api_GetErrorString;

        /**
         * An SDRPlay API ver 3.14. function; hence it can be NULL when running
         * a ver. < 3.14 service. Then the below `error_timestamp`
         * and `error_info` are always empty.
         */
        sdrplay_api_GetLastErrorByType_t sdrplay_api_GetLastErrorByType;
        uint64_t                         error_timestamp;
        sdrplay_api_ErrorInfoT           error_info;
      } sdrplay_priv;

static sdrplay_priv sdr;

/* 4 - 44 dB
 */
static int gain_table [10] = { 40, 100, 150, 170, 210, 260, 310, 350, 390, 440 };

/**
 * Load and use the SDRPlay-API dynamically.
 */
#define ADD_FUNC(opt, func)  { opt, NULL, NULL, #func, (void**) &sdr.func }

static struct dyn_struct sdrplay_funcs [] = {
              ADD_FUNC (false, sdrplay_api_Open),
              ADD_FUNC (false, sdrplay_api_Close),
              ADD_FUNC (false, sdrplay_api_Init),
              ADD_FUNC (false, sdrplay_api_Uninit),
              ADD_FUNC (false, sdrplay_api_ApiVersion),
              ADD_FUNC (false, sdrplay_api_DebugEnable),
              ADD_FUNC (false, sdrplay_api_LockDeviceApi),
              ADD_FUNC (false, sdrplay_api_UnlockDeviceApi),
              ADD_FUNC (false, sdrplay_api_GetDevices),
              ADD_FUNC (false, sdrplay_api_GetDeviceParams),
              ADD_FUNC (false, sdrplay_api_SelectDevice),
              ADD_FUNC (false, sdrplay_api_ReleaseDevice),
              ADD_FUNC (false, sdrplay_api_Update),
              ADD_FUNC (false, sdrplay_api_GetErrorString),
              ADD_FUNC (true,  sdrplay_api_GetLastErrorByType)  /* optional, added in API ver 3.14 */
          };

#undef ADD_FUNC

/**
 * Load the `sdrplay_api.dll` from a specific location or let `LoadLibraryA()`
 * search along the `%PATH%`.
 *
 * The `sdrplay-dll` value in `dump1090.cfg` is empty by default and
 * `Modes.sdrplay.dll_name` equals `sdrplay_api.dll`. Hence `LoadLibraryA()`
 * will search along the `%PATH%`.
 *
 * But the config-callback `sdrplay_set_dll_name()` could have set another
 * custom value.
 */
static bool sdrplay_load_funcs (void)
{
  mg_file_path full_name;
  size_t       i, num;

  for (i = 0; i < DIM(sdrplay_funcs); i++)
      sdrplay_funcs[i].mod_name = Modes.sdrplay.dll_name;

  SetLastError (0);

  num = load_dynamic_table (sdrplay_funcs, DIM(sdrplay_funcs));
  if (num < DIM(sdrplay_funcs) - 1 || !sdrplay_funcs[0].mod_handle)
  {
    DWORD err = GetLastError();

    /**
     * The `LoadLibraryA()` above will fail with `err == ERROR_BAD_EXE_FORMAT` (193)
     * if we're running a 32-bit Dump1090 and we loaded a 64-bit "sdrplay_api.dll".
     * And vice-versa.
     */
    if (err == ERROR_BAD_EXE_FORMAT)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "\"%s\" is not a %d bit DLL", Modes.sdrplay.dll_name, 8*(int)sizeof(void*));
    else if (err == ERROR_MOD_NOT_FOUND)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "\"%s\" not found on PATH", Modes.sdrplay.dll_name);
    else snprintf (sdr.last_err, sizeof(sdr.last_err), "Failed to load \"%s\"; %s", Modes.sdrplay.dll_name, win_strerror(err));
    return (false);
  }

  if (!GetModuleFileNameA(sdrplay_funcs[0].mod_handle, full_name, sizeof(full_name)))
     strcpy (full_name, "?");

  /* These 2 names better be the same
   */
  TRACE ("full_name: '%s'\n", full_name);
  TRACE ("dll_name:  '%s'\n", Modes.sdrplay.dll_name);
  return (true);
}

/**
 * Store the last error-details and timestamp by `type`.
 *
 * The `sdrplay_api_ErrorInfoT` structure contains this:
 * ```
 * typedef struct {
 *     char file [256];
 *     char function [256];
 *     int  line;
 *     char message [1024];
 *  } sdrplay_api_ErrorInfoT;
 * ```
 *
 * And the `type` means:
 *  - 0:  DLL message
 *  - 1:  DLL device message
 *  - 2:  Service message
 *  - 3:  Service device message
 */
static void sdrplay_store_error_details (int type)
{
  sdrplay_api_ErrorInfoT *info;
  uint64_t                time;

  sdr.error_timestamp = 0ULL;
  memset (&sdr.error_info, '\0', sizeof(sdr.error_info));
  info = (*sdr.sdrplay_api_GetLastErrorByType) (sdr.handle, type, &time);

  /* Can it return NULL?
   */
  if (info)
  {
    sdr.error_timestamp = time;
    sdr.error_info = *info;
  }
}

/**
 * Store the last error-code and error-text from the
 * last failed `CALL_FUNC()` macro call.
 */
static void sdrplay_store_error (sdrplay_api_ErrT rc)
{
  sdr.last_rc = rc;

  if (sdr.sdrplay_api_GetErrorString)
       strcpy_s (sdr.last_err, sizeof(sdr.last_err), (*sdr.sdrplay_api_GetErrorString)(rc));
  else if (rc == sdrplay_api_NotInitialised)
       strcpy_s (sdr.last_err, sizeof(sdr.last_err), "SDRplay API not initialised");
  else sdr.last_err[0] = '\0';

  if (sdr.sdrplay_api_GetLastErrorByType)
     sdrplay_store_error_details (0);   /* should use corect type */
}

/**
 * Clear any last error-codes and error-text from the
 * last successful `CALL_FUNC()` macro call.
 */
static void sdrplay_clear_error (void)
{
  sdr.last_rc = sdrplay_api_Success;
  strcpy (sdr.last_err, "none");
  sdr.error_timestamp = 0ULL;
  memset (&sdr.error_info, '\0', sizeof(sdr.error_info));
}

/**
 * Return some sensible names for some SDRPlay values.
 * For tracing.
 */
static const char *sdrplay_tuner_name (sdrplay_api_TunerSelectT tuner)
{
  return (tuner == sdrplay_api_Tuner_Neither ? "Tuner_Neither" :
          tuner == sdrplay_api_Tuner_A       ? "Tuner_A"       :
          tuner == sdrplay_api_Tuner_B       ? "Tuner B"       :
          tuner == sdrplay_api_Tuner_Both    ? "Both tuners"   : "??");
}

static const char *sdrplay_duo_event (sdrplay_api_RspDuoModeCbEventIdT duo)
{
  return (duo == sdrplay_api_MasterInitialised    ? "MasterInitialised"    :
          duo == sdrplay_api_SlaveAttached        ? "SlaveAttached"        :
          duo == sdrplay_api_SlaveDetached        ? "SlaveDetached"        :
          duo == sdrplay_api_SlaveInitialised     ? "SlaveInitialised"     :
          duo == sdrplay_api_SlaveUninitialised   ? "SlaveUninitialised"   :
          duo == sdrplay_api_MasterDllDisappeared ? "MasterDllDisappeared" :
          duo == sdrplay_api_SlaveDllDisappeared  ? "SlaveDllDisappeared"  : "??");
}

static const char *sdrplay_adsb_mode (sdrplay_api_AdsbModeT mode)
{
  return (mode == sdrplay_api_ADSB_DECIMATION                  ? "ADSB_DECIMATION"                  :
          mode == sdrplay_api_ADSB_NO_DECIMATION_LOWPASS       ? "ADSB_NO_DECIMATION_LOWPASS"       :
          mode == sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ ? "ADSB_NO_DECIMATION_BANDPASS_2MHZ" :
          mode == sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ ? "ADSB_NO_DECIMATION_BANDPASS_3MHZ" : "??");
}

static const char *sdrplay_overload_name (sdrplay_api_PowerOverloadCbEventIdT ovr)
{
  return (ovr == sdrplay_api_Overload_Detected ? "Overload Detected" : "Overload Corrected");
}

/**
 * The SDRplay event callback.
 *
 * 16-bit data is received from RSP at 2MHz. It is interleaved into a circular buffer.
 * Each time the pointer passes a multiple of `MODES_RSP_BUF_SIZE`, that segment of
 * buffer is handed off to the callback-routine `rx_callback()` in `dump1090.c`.
 *
 * For each packet from the RSP, the maximum `I` signal value is recorded.
 * This is entered into a slow, exponentially decaying filter. The output from this filter
 * is occasionally checked and a decision made whether to step the RSP gain by
 * plus or minus 1 dB.
 */
static void sdrplay_event_callback (sdrplay_api_EventT        event_id,
                                    sdrplay_api_TunerSelectT  tuner,
                                    sdrplay_api_EventParamsT *params,
                                    void                     *cb_context)
{
  if (sdr.cancelling || Modes.exit)
     return;

  EnterCriticalSection (&Modes.print_mutex);

  switch (event_id)
  {
    case sdrplay_api_PowerOverloadChange:
         TRACE ("sdrplay_api_PowerOverloadChange: sdrplay_api_AgcEvent, tuner=%s powerOverloadChangeType=%s\n",
                sdrplay_tuner_name(tuner), sdrplay_overload_name(params->powerOverloadParams.powerOverloadChangeType));

         CALL_FUNC (sdrplay_api_Update, sdr.handle, tuner,
                    sdrplay_api_Update_Ctrl_OverloadMsgAck,
                    sdrplay_api_Update_Ext1_None);
         break;

    case sdrplay_api_RspDuoModeChange:
         TRACE ("sdrplay_api_RspDuoModeChange, tuner=%s modeChangeType=%s\n",
                sdrplay_tuner_name(tuner), sdrplay_duo_event(params->rspDuoModeParams.modeChangeType));

         if (params->rspDuoModeParams.modeChangeType == sdrplay_api_MasterInitialised)
         {
           sdr.master_initialised = true;
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveUninitialised)
         {
           sdr.slave_uninitialised = true;
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveAttached)
         {
           sdr.slave_attached = true;
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveDetached)
         {
           sdr.slave_attached = false;
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_MasterDllDisappeared)
         {
           sdrplay_exit (sdr.chosen_dev);
           LOG_STDERR ("\nThe master stream no longer exists.\n"
                       "This application will now exit.\n");
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveDllDisappeared)
         {
           sdr.slave_attached = false;
         }
         break;

    case sdrplay_api_GainChange:
         TRACE ("sdrplay_api_GainChange, tuner=%s gRdB=%d lnaGRdB=%d systemGain=%.2f\n",
                sdrplay_tuner_name(tuner),
                params->gainParams.gRdB,
                params->gainParams.lnaGRdB,
                params->gainParams.currGain);
         break;

    case sdrplay_api_DeviceRemoved:
         TRACE ("sdrplay_api_DeviceRemoved\n");
         break;

    case sdrplay_api_DeviceFailure:
         TRACE ("sdrplay_api_DeviceFailure\n");
         break;

    default:
         TRACE ("unknown event %d\n", event_id);
         break;
  }

  LeaveCriticalSection (&Modes.print_mutex);
  MODES_NOTUSED (cb_context);
}

/**
 * The main SDRplay stream callback.
 */
static void sdrplay_callback_A (short                       *xi,
                                short                       *xq,
                                sdrplay_api_StreamCbParamsT *params,
                                unsigned int                 num_samples,
                                unsigned int                 reset,
                                void                        *cb_context)
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
  count2 = end - (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS);
  if (count2 < 0)
     count2 = 0;            /* count2 is samples wrapping around to start of buf */

  count1 = (num_samples << 1) - count2;   /* count1 is samples fitting before the end of buf */

  /* Flag is set if this packet takes us past a multiple of MODES_RSP_BUF_SIZE
   */
  new_buf_flag = ((rx_data_idx & (MODES_RSP_BUF_SIZE-1)) < (end & (MODES_RSP_BUF_SIZE-1))) ? false : true;

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
  max_sig = max_sig_acc >> RSP_ACC_SHIFT;
  max_sig_acc -= max_sig;

  /* This code is triggered as we reach the end of our circular buffer
   */
  if (rx_data_idx >= (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS))
  {
    rx_data_idx = 0;  /* pointer back to start of buffer */

    EnterCriticalSection (&Modes.print_mutex);

    /* Adjust gain if required
     */
    if (max_sig > RSP_MAX_GAIN_THRESH)
    {
      sdr.ch_params->tunerParams.gain.gRdB++;
      if (sdr.ch_params->tunerParams.gain.gRdB > 59)
         sdr.ch_params->tunerParams.gain.gRdB = 59;

      CALL_FUNC (sdrplay_api_Update, sdr.handle, sdr.chosen_dev->tuner,
                 sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }
    else if (max_sig < RSP_MIN_GAIN_THRESH)
    {
      sdr.ch_params->tunerParams.gain.gRdB--;
      if (sdr.ch_params->tunerParams.gain.gRdB < 0)
         sdr.ch_params->tunerParams.gain.gRdB = 0;

      CALL_FUNC (sdrplay_api_Update, sdr.handle, sdr.chosen_dev->tuner,
                 sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }

    LeaveCriticalSection (&Modes.print_mutex);
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
    end = rx_data_idx + MODES_RSP_BUF_SIZE * (MODES_RSP_BUFFERS-1);
    end &= (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS) - 1;
    end &= ~(MODES_RSP_BUF_SIZE - 1);

    sdr.rx_num_callbacks++;
    (*sdr.rx_callback) ((uint8_t*)sdr.rx_data + end, MODES_RSP_BUF_SIZE, sdr.rx_context);
  }

  /* Stash static values in `sdr` struct
   */
  sdr.max_sig     = max_sig_acc;
  sdr.rx_data_idx = rx_data_idx;

  MODES_NOTUSED (params);
  MODES_NOTUSED (reset);
  MODES_NOTUSED (cb_context);
}

/**
 * The secondary (?) SDRplay stream callback.
 *
 * Not used for anything.
 */
static void sdrplay_callback_B (short                       *xi,
                                short                       *xq,
                                sdrplay_api_StreamCbParamsT *params,
                                unsigned int                 num_samples,
                                unsigned int                 reset,
                                void                        *cb_context)
{
  MODES_NOTUSED (xi);
  MODES_NOTUSED (xq);
  MODES_NOTUSED (params);
  MODES_NOTUSED (num_samples);
  MODES_NOTUSED (reset);
  MODES_NOTUSED (cb_context);
}

/**
 * Select an SDRPlay device on name of index.
 */
static bool sdrplay_select (const char *wanted_name, int wanted_index)
{
  sdrplay_api_DeviceT *device;
  int     i, select_this = -1, dash_ofs = 0;
  char    current_dev [100];
  bool    select_first = true;

  /** Allow a `wanted_name` like "sdrplay-RSP1A"
   */
  if (*wanted_name == '-')
     dash_ofs = 1;

  if (wanted_index != -1 || *wanted_name)
     select_first = false;

  CALL_FUNC (sdrplay_api_LockDeviceApi);
  if (sdr.last_rc != sdrplay_api_Success)
     return (false);

  sdr.API_locked = true;

  CALL_FUNC (sdrplay_api_GetDevices, sdr.devices, &sdr.num_devices, DIM(sdr.devices));
  if (sdr.num_devices == 0)
  {
    LOG_STDERR ("No SDRplay devices found.\n");
    return (false);
  }

  device = sdr.devices + 0;  /**< start checking the 1st device returned */

  TRACE ("wanted_name: \"sdrplay%s\", wanted_index: %d. Found %d devices\n",
         wanted_name, wanted_index, sdr.num_devices);

  for (i = 0; i < (int)sdr.num_devices; i++)
  {
    if (sdr.devices[i].hwVer == SDRPLAY_RSP1_ID)
    {
      strcpy (current_dev, "RSP1");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSP1A_ID)
    {
      strcpy (current_dev, "RSP1A");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSP1B_ID)
    {
      strcpy (current_dev, "RSP1B");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSP2_ID)
    {
      strcpy (current_dev, "RSP2");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSPdx_ID)
    {
      strcpy (current_dev, "RSPdx");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSPduo_ID)
    {
      strcpy (current_dev, "RSPduo");
    }
    else
    {
      snprintf (current_dev, sizeof(current_dev), "RSP%d !!??", sdr.devices[i].hwVer);
    }

    TRACE ("Device Index %d: %s - SerialNumber = %s\n", i, current_dev, sdr.devices[i].SerNo);

    if (select_this == -1)
    {
      if (select_first)
         select_this = i;
      else if (i == wanted_index)
         select_this = i;
      else if (!stricmp(current_dev, wanted_name + dash_ofs))
         select_this = i;
    }
  }

  if (select_this == -1)
  {
    LOG_STDERR ("Wanted device \"sdrplay%s\" (at index: %d) not found.\n",
                wanted_name, wanted_index);
    return (false);
  }

  CALL_FUNC (sdrplay_api_SelectDevice, device);
  if (sdr.last_rc != sdrplay_api_Success)
     return (false);

  sdr.handle     = device->dev;
  sdr.chosen_dev = device;        /**< we only support 1 device */

  Modes.selected_dev = mg_mprintf ("sdrplay-%s", current_dev);
  return (true);
}

/**
 * \brief Reads samples from the SDRPlayAPIservice.
 *
 * This routine should be called from the main application in a separate thread.
 *
 * It enters an infinite loop only returning when the main application sets
 * the stop-condition specified in the `context`.
 *
 * \param[in] device   The device handle which is ignored. Since it's already
 *                     retured in `sdrplay_init()` (we support only one device at a time.
 *                     But check for a NULL-device just in case).
 * \param[in] callback The address of the receiver callback.
 * \param[in] context  The address of the "stop-variable".
 * \param[in] buf_num  The number of buffers to use (ignored for now).
 * \param[in] buf_len  The length of each buffer to use (ignored for now).
 */
int sdrplay_read_async (sdrplay_dev *device,
                        sdrplay_cb   callback,
                        void        *context,
                        uint32_t     buf_num,
                        uint32_t     buf_len)
{
  int tuner;

  MODES_NOTUSED (buf_num);
  MODES_NOTUSED (buf_len);

  if (!device || device != sdr.chosen_dev)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No device");
    sdr.last_rc = sdrplay_api_NotInitialised;
    return (sdr.last_rc);
  }

  sdr.ch_params = (sdr.chosen_dev->tuner == sdrplay_api_Tuner_A) ?
                  sdr.dev_params->rxChannelA : sdr.dev_params->rxChannelB;

  TRACE ("tuner: '%s', ch-A: 0x%p, ch-B: 0x%p\n",
         sdrplay_tuner_name(sdr.chosen_dev->tuner),
         sdr.dev_params->rxChannelA,
         sdr.dev_params->rxChannelB);

#if 0
  sdr.ch_params->ctrlParams.dcOffset.IQenable = 0;
  sdr.ch_params->ctrlParams.dcOffset.DCenable = 1;
#else
  sdr.ch_params->ctrlParams.dcOffset.IQenable = 1;
  sdr.ch_params->ctrlParams.dcOffset.DCenable = 0;
#endif

  sdr.callbacks.StreamACbFn = sdrplay_callback_A;
  sdr.callbacks.StreamBCbFn = sdrplay_callback_B;
  sdr.callbacks.EventCbFn   = sdrplay_event_callback;
  sdr.rx_callback           = callback;
  sdr.rx_context            = context;

  if (sdr.chosen_dev->hwVer != SDRPLAY_RSP1_ID)
     sdr.ch_params->tunerParams.gain.minGr = sdrplay_api_EXTENDED_MIN_GR;

  sdr.ch_params->tunerParams.gain.gRdB = Modes.sdrplay.gain_reduction;
  sdr.ch_params->tunerParams.gain.LNAstate = 0;

  sdr.ch_params->ctrlParams.agc.enable = Modes.dig_agc;

  sdr.ch_params->tunerParams.dcOffsetTuner.dcCal = 4;
  sdr.ch_params->tunerParams.dcOffsetTuner.speedUp = 0;
  sdr.ch_params->tunerParams.dcOffsetTuner.trackTime = 63;

  if (sdr.chosen_dev->hwVer != SDRPLAY_RSPduo_ID || sdr.chosen_dev->rspDuoMode != sdrplay_api_RspDuoMode_Slave)
     sdr.dev_params->devParams->fsFreq.fsHz = Modes.sample_rate;  // was '8 * 1E6'

  if (sdr.chosen_dev->hwVer == SDRPLAY_RSPduo_ID && (sdr.chosen_dev->rspDuoMode & sdrplay_api_RspDuoMode_Slave))
  {
    if ((uint32_t)sdr.chosen_dev->rspDuoSampleFreq != Modes.sample_rate)
    {
      strcpy_s (sdr.last_err, sizeof(sdr.last_err), "RSPduo Master tuner in use and is not running in ADS-B compatible mode");
      LOG_STDERR ("Error: %s.\n"
                  "Set the Master tuner to ADS-B compatible mode and restart %s.\n",
                  sdr.last_err, Modes.who_am_I);
      return (sdrplay_api_InvalidParam);
    }
  }

  if (sdr.chosen_dev->hwVer == SDRPLAY_RSP1A_ID || sdr.chosen_dev->hwVer == SDRPLAY_RSP1B_ID)
  {
    sdr.ch_params->rsp1aTunerParams.biasTEnable             = Modes.bias_tee;
    sdr.dev_params->devParams->rsp1aParams.rfNotchEnable    = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.dev_params->devParams->rsp1aParams.rfDabNotchEnable = (1 - Modes.sdrplay.disable_DAB_notch);
  }
  else if (sdr.chosen_dev->hwVer == SDRPLAY_RSP2_ID)
  {
    sdr.ch_params->rsp2TunerParams.biasTEnable   = Modes.bias_tee;
    sdr.ch_params->rsp2TunerParams.rfNotchEnable = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.ch_params->rsp2TunerParams.amPortSel     = sdrplay_api_Rsp2_AMPORT_2;
    sdr.ch_params->rsp2TunerParams.antennaSel    = Modes.sdrplay.antenna_port;
  }
  else if (sdr.chosen_dev->hwVer == SDRPLAY_RSPdx_ID)
  {
    sdr.dev_params->devParams->rspDxParams.biasTEnable      = Modes.bias_tee;
    sdr.dev_params->devParams->rspDxParams.rfNotchEnable    = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.dev_params->devParams->rspDxParams.antennaSel       = Modes.sdrplay.DX_antenna_port;
    sdr.dev_params->devParams->rspDxParams.rfDabNotchEnable = (1 - Modes.sdrplay.disable_DAB_notch);
  }
  else if (sdr.chosen_dev->hwVer == SDRPLAY_RSPduo_ID)
  {
    sdr.ch_params->rspDuoTunerParams.biasTEnable      = Modes.bias_tee;
    sdr.ch_params->rspDuoTunerParams.rfNotchEnable    = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.ch_params->rspDuoTunerParams.rfDabNotchEnable = (1 - Modes.sdrplay.disable_DAB_notch);
  }

  sdr.ch_params->ctrlParams.adsbMode = Modes.sdrplay.ADSB_mode;

  if (Modes.sdrplay.if_mode == false)  /* Zero-IF mode */
  {
    if (!Modes.sdrplay.over_sample)
    {
      sdr.ch_params->ctrlParams.decimation.enable = 1;
      sdr.ch_params->ctrlParams.decimation.decimationFactor = 4;
    }
    else
    {
      sdr.ch_params->ctrlParams.adsbMode = sdrplay_api_ADSB_DECIMATION;
      sdr.ch_params->ctrlParams.decimation.enable = 0;
      sdr.ch_params->ctrlParams.decimation.decimationFactor = 1;
    }
  }

  if (Modes.sdrplay.USB_bulk_mode)
  {
    TRACE ("Using USB bulk mode\n");
    sdr.dev_params->devParams->mode = Modes.sdrplay.USB_bulk_mode;
  }
  else
  {
    TRACE ("Using USB isochronous mode (default)\n");
    sdr.dev_params->devParams->mode = sdrplay_api_ISOCH;
  }

  if (sdr.ch_params == sdr.dev_params->rxChannelA)
       tuner = 'A';
  else if (sdr.ch_params == sdr.dev_params->rxChannelB)
       tuner = 'B';
  else tuner = '?';

  TRACE ("'Tuner_%c': sample-rate: %.0f MS/s, adsbMode: %s.\n"
         "                           decimation-enable: %d, decimation-factor: %d, SAMPLE_TYPE: %s\n",
         tuner, sdr.dev_params->devParams->fsFreq.fsHz / 1E6,
         sdrplay_adsb_mode(sdr.ch_params->ctrlParams.adsbMode),
         sdr.ch_params->ctrlParams.decimation.enable,
         sdr.ch_params->ctrlParams.decimation.decimationFactor,
         SAMPLE_TYPE_STR);

  CALL_FUNC (sdrplay_api_Init, sdr.handle, &sdr.callbacks, NULL);
  if (sdr.last_rc != sdrplay_api_Success)
     return (sdr.last_rc);

  sdr.ch_params->tunerParams.rfFreq.rfHz = Modes.freq;

  CALL_FUNC (sdrplay_api_Update, sdr.handle, sdr.chosen_dev->tuner,
             sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);

  if (sdr.last_rc != sdrplay_api_Success)
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
int sdrplay_set_gain (sdrplay_dev *device, int gain)
{
  LOG_FILEONLY ("gain: %.1f dB\n", (double)gain / 10);
  MODES_NOTUSED (device);
  return (0);
}

/**
 * \brief Cancels the callbacks from the SDRPlayAPIservice.
 *
 * Force the `sdrplay_read_async()` to stop and return from it's loop.
 * Called in the `SIGINT` handler in dump1090.c.
 */
int sdrplay_cancel_async (sdrplay_dev *device)
{
  if (device != sdr.chosen_dev)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No device");
    sdr.last_rc = sdrplay_api_NotInitialised;
  }
  else if (sdr.cancelling)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "Cancelling");
    sdr.last_rc = sdrplay_api_StopPending;
  }
  else if (!sdr.uninit_done)
  {
    CALL_FUNC (sdrplay_api_Uninit, sdr.chosen_dev->dev);
    sdr.cancelling = sdr.uninit_done = true;
  }
  return (sdr.last_rc);
}

/**
 * Returns the last error set.
 * Called from the outside only.
 */
const char *sdrplay_strerror (int rc)
{
  if (sdr.last_rc == -1)
     return ("<unknown>");
  if (rc == 0 || !sdr.last_err[0])
     return ("<success>");
  return (sdr.last_err);
}

/**
 * Load all needed SDRplay functions dynamically
 * from `Modes.sdrplay.dll_name`.
 */
int sdrplay_init (const char *name, int index, sdrplay_dev **device)
{
  *device = NULL;

  TRACE ("name: '%s', index: %d\n", name, index);

  sdr.chosen_dev = NULL;
  sdr.last_rc    = -1;  /* no idea yet */

  sdr.cancelling = false;
  sdr.API_locked = false;

  Modes.sdrplay.gain_reduction = MODES_RSP_INITIAL_GR;
  Modes.sdrplay.disable_broadcast_notch = true;
  Modes.sdrplay.disable_DAB_notch       = true;

  Modes.sdrplay.antenna_port    = sdrplay_api_Rsp2_ANTENNA_B;
  Modes.sdrplay.DX_antenna_port = sdrplay_api_RspDx_ANTENNA_B;
  Modes.sdrplay.tuner           = sdrplay_api_Tuner_B;           /* RSPduo default */
  Modes.sdrplay.mode            = sdrplay_api_RspDuoMode_Master; /* RSPduo default */
  Modes.sdrplay.BW_mode         = 1;  /* 5 MHz */
  Modes.sdrplay.over_sample     = true;

  sdr.rx_data = malloc (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS * sizeof(short));
  if (!sdr.rx_data)
     goto nomem;

  Modes.sdrplay.gains = malloc (10 * sizeof(int));
  if (!Modes.sdrplay.gains)
      goto nomem;

  Modes.sdrplay.gain_count = 10;
  memcpy (Modes.sdrplay.gains, &gain_table, 10 * sizeof(int));

  if (!sdrplay_load_funcs())
     goto failed;

  TRACE ("Optional (ver. 3.14) function 'sdrplay_api_GetLastErrorByType()' %sfound\n",
         sdr.sdrplay_api_GetLastErrorByType ? "" : "not ");

  CALL_FUNC (sdrplay_api_Open);
  if (sdr.last_rc != sdrplay_api_Success)
  {
    fprintf (stderr, "The SDRPlay API is not responding. A service restart could help:\n");
    fprintf (stderr, "  sc.exe stop SDRplayAPIService & ping.exe -w1 -n2 0.0.0.0 > NUL & sc.exe start SDRplayAPIService\n");
    goto failed;
  }

  CALL_FUNC (sdrplay_api_ApiVersion, &sdr.version);
  if (sdr.last_rc != sdrplay_api_Success)
     goto failed;

  TRACE ("sdrplay_api_ApiVersion(): '%.2f', min_version: '%.2f', build version: '%.2f'\n",
         sdr.version, Modes.sdrplay.min_version, SDRPLAY_API_VERSION);

  if (sdr.version == 3.10F && SDRPLAY_API_VERSION == 3.11F)
     TRACE ("ver 3.10 and ver 3.11 should be compatible\n");

  else if (sdr.version < Modes.sdrplay.min_version)
  {
    snprintf (sdr.last_err, sizeof(sdr.last_err),
              "Wrong sdrplay_api_ApiVersion(): '%.2f', minimum version: '%.2f'.\n",
              sdr.version, Modes.sdrplay.min_version);
    goto failed;
  }

  if (!sdrplay_select(name + 7, index))
     goto failed;

  if (Modes.debug & DEBUG_GENERAL)
     CALL_FUNC (sdrplay_api_DebugEnable, sdr.handle, sdrplay_api_DbgLvl_Verbose);

  CALL_FUNC (sdrplay_api_GetDeviceParams, sdr.handle, &sdr.dev_params);

  if (sdr.last_rc != sdrplay_api_Success || !sdr.dev_params)
  {
    TRACE ("sdrplay_api_GetDeviceParams() failed: '%s'\n", sdr.last_err);
    goto failed;
  }

  TRACE ("device: 0x%p, ch-A: 0x%p, ch-B: 0x%p\n",
         sdr.chosen_dev, sdr.dev_params->rxChannelA, sdr.dev_params->rxChannelB);

  *device = sdr.chosen_dev;

  return (sdrplay_api_Success);

nomem:
  strcpy_s (sdr.last_err, sizeof(sdr.last_err), "Insufficient memory");
  sdr.last_rc = ENOMEM;       /* fall through to 'failed' */

failed:
  LOG_STDERR ("%s\n", sdr.last_err);
  sdrplay_exit (NULL);
  return (sdrplay_api_Fail);  /* A better error-code? */
}

/**
 * Free the API and the device.
 */
static int sdrplay_release (sdrplay_dev *device)
{
  if (device != sdr.chosen_dev)    /* support only 1 device */
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No device");
    sdr.last_rc = sdrplay_api_NotInitialised;
  }
  else
  {
    if (!sdr.API_locked)
       CALL_FUNC (sdrplay_api_LockDeviceApi);

    if (!sdr.cancelling)
    {
      CALL_FUNC (sdrplay_api_Uninit, sdr.chosen_dev->dev);
      sdr.uninit_done = true;
    }

    CALL_FUNC (sdrplay_api_ReleaseDevice, sdr.chosen_dev);

    if (sdr.API_locked)
       CALL_FUNC (sdrplay_api_UnlockDeviceApi);
  }

  sdr.API_locked = false;
  sdr.chosen_dev = NULL;
  return (sdr.last_rc);
}

/**
 * Exit-function for this module:
 *  \li Release the device.
 *  \li Unload the handle of `Modes.sdrplay.dll_name`.
 */
int sdrplay_exit (sdrplay_dev *device)
{
  if (device)
     sdrplay_release (device);

  free (sdr.rx_data);
  sdr.rx_data = NULL;

  if (!sdrplay_funcs[0].mod_handle)
  {
    strcpy_s (sdr.last_err, sizeof(sdr.last_err), "No DLL loaded");
    sdr.last_rc = sdrplay_api_NotInitialised;
  }
  else
  {
    CALL_FUNC (sdrplay_api_Close);
    unload_dynamic_table (sdrplay_funcs, DIM(sdrplay_funcs));
  }
  sdr.chosen_dev = NULL;
  return (sdr.last_rc);
}

/**
 * Config-parser callback; <br>
 * parses "adsb-mode" and sets `Modes.sdrplay.ADSB_mode`.
 */
bool sdrplay_set_adsb_mode (const char *arg)
{
  char   *end;
  sdrplay_api_AdsbModeT mode = strtod (arg, &end);

  if (end == arg || *end != '\0')  /* A non-numeric value */
     goto fail;

  if (mode == sdrplay_api_ADSB_DECIMATION ||
      mode == sdrplay_api_ADSB_NO_DECIMATION_LOWPASS ||
      mode == sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ ||
      mode == sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ)
  {
    Modes.sdrplay.ADSB_mode = mode;
    return (true);
  }

fail:
  LOG_STDERR ("\nIllegal 'adsb-mode = %s'.\n", arg);
  return (false);
}

/**
 * Config-parser callback; <br>
 * parses "sdrplay-dll" and sets `Modes.sdrplay.dll_name`.
 */
bool sdrplay_set_dll_name (const char *arg)
{
  mg_file_path dll = "?";
  DWORD        len, attr;

  if (!strpbrk(arg, "/\\"))  /* not absolute or relative; assume on PATH */
  {
    strcpy_s (Modes.sdrplay.dll_name, sizeof(Modes.sdrplay.dll_name), arg);
    return (true);
  }

  attr = INVALID_FILE_ATTRIBUTES;
  len  = GetFullPathName (arg, sizeof(dll), dll, NULL);

  TRACE ("dll: '%s', len: %lu, attr: 0x%08lx\n", dll, len, attr);
  if (len > 0)
     attr = GetFileAttributes (dll);
  else if (attr != FILE_ATTRIBUTE_NORMAL)
  {
    LOG_STDERR ("\nThe \"sdrplay-dll = %s\" was not found. "
                "Using the default \"%s\"\n", arg, Modes.sdrplay.dll_name);
    return (false);
  }
  strcpy_s (Modes.sdrplay.dll_name, sizeof(Modes.sdrplay.dll_name), dll);
  return (true);
}

/**
 * Config-parser callback; <br>
 * parses "sdrplay-minver" and sets `Modes.sdrplay.min_version`.
 */
bool sdrplay_set_minver (const char *arg)
{
  char *end;
  float ver = strtof (arg, &end);

  if (end == arg || *end != '\0')
  {
    LOG_STDERR ("\nIllegal 'sdrplay-minver = %s'.\n", arg);
    return (false);
  }
  Modes.sdrplay.min_version = ver;
  return (true);
}
