/**
 * \file    sdrplay.c
 * \ingroup Main
 * \brief   The interface for SDRplay devices.
 *
 * Load all needed functions from "sdrplay_api.dll" dynamically.
 */
#include "sdrplay.h"
#include "misc.h"

#define MODES_RSP_BUF_SIZE  (16*16384)   /* 256k, same as MODES_DATA_LEN  */
#define MODES_RSP_BUFFERS    16          /* Must be power of 2 */

#define RSP_MIN_GAIN_THRESH  512         /* Increase gain if peaks below this */
#define RSP_MAX_GAIN_THRESH 1024         /* Decrease gain if peaks above this */
#define RSP_ACC_SHIFT         13         /* Sets time constant of averaging filter */
#define MODES_RSP_INITIAL_GR  20

/**
 * \def LOAD_FUNC(func)
 *   A `GetProcAddress()` helper.
 *   \param func  the name of the function (without any `"`).
 */
#define LOAD_FUNC(func)                                                          \
        do {                                                                     \
          sdr.func = (func ## _t) GetProcAddress (sdr.dll_hnd, #func);           \
          if (!sdr.func)                                                         \
          {                                                                      \
            snprintf (sdr.last_err, sizeof(sdr.last_err),                        \
                      "Failed to find '%s()' in %s", #func, sdr.dll_name);       \
            goto failed;                                                         \
          }                                                                      \
          TRACE (DEBUG_GENERAL2, "Function: %-30s -> 0x%p.\n", #func, sdr.func); \
        } while (0)

#ifdef __clang__
  #define SDR_TRACE(func, ...)  TRACE (DEBUG_GENERAL2, "%s(%s);\n", #func, #__VA_ARGS__)
#else
  #define SDR_TRACE(func, ...)  ((void) 0)   /* MSVC cannot do the above */
#endif

#define CALL_FUNC(func, ...)                                                    \
        do {                                                                    \
          sdrplay_api_ErrT rc;                                                  \
          SDR_TRACE (func, __VA_ARGS__);                                        \
          if (!sdr.func)                                                        \
               rc = sdrplay_api_NotInitialised;                                 \
          else rc = (*sdr.func) (__VA_ARGS__);                                  \
          if (rc != sdrplay_api_Success)                                        \
          {                                                                     \
            sdrplay_store_error (rc);                                           \
            TRACE (DEBUG_GENERAL, "%s(): %d / %s.\n", #func, rc, sdr.last_err); \
          }                                                                     \
        } while (0)

struct SDRplay_info {
       const char                    *dll_name;
       HANDLE                         dll_hnd;
       float                          version;
       bool                           API_locked;
       bool                           master_initialised;
       bool                           slave_uninitialised;
       bool                           slave_attached;
       bool                           cancelling;

       sdrplay_api_DeviceT           *dev;
       sdrplay_api_DeviceT            devices [4];
       unsigned int                   num_devices;
       char                           last_err [256];
       sdrplay_api_ErrT               last_rc;
       int                            max_sig, max_sig_acc;
       sdrplay_api_CallbackFnsT       cbFns;
       sdrplay_api_DeviceParamsT     *deviceParams;
       sdrplay_api_RxChannelParamsT  *chParams;
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
     };

static struct SDRplay_info sdr = { "sdrplay_api.dll" };

/**
 * We support only 1 device at a time.
 */
static sdrplay_dev *g_sdr_device;
static HANDLE       g_sdr_handle;

/**
 * Store the last error-code and error-text from the
 * last `CALL_FUNC()` macro call.
 */
static void sdrplay_store_error (sdrplay_api_ErrT rc)
{
  sdr.last_rc = rc;

  if (sdr.sdrplay_api_GetErrorString)
       strncpy (sdr.last_err, (*sdr.sdrplay_api_GetErrorString)(rc), sizeof(sdr.last_err));
  else if (rc == sdrplay_api_NotInitialised)
       strncpy (sdr.last_err, "SDRplay API not initialised", sizeof(sdr.last_err));
  else sdr.last_err[0] = '\0';
}

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
  if (g_sdr_device->cancelling)
     return;

  EnterCriticalSection (&Modes.print_mutex);

  switch (event_id)
  {
    case sdrplay_api_PowerOverloadChange:
         TRACE (DEBUG_GENERAL2, "%s(): sdrplay_api_PowerOverloadChange: sdrplay_api_AgcEvent, tuner=%s powerOverloadChangeType=%s\n",
                __FUNCTION__, sdrplay_tuner_name(tuner), sdrplay_overload_name(params->powerOverloadParams.powerOverloadChangeType));

         CALL_FUNC (sdrplay_api_Update, g_sdr_handle, tuner,
                    sdrplay_api_Update_Ctrl_OverloadMsgAck,
                    sdrplay_api_Update_Ext1_None);
         break;

    case sdrplay_api_RspDuoModeChange:
         TRACE (DEBUG_GENERAL2, "%s(): sdrplay_api_RspDuoModeChange, tuner=%s modeChangeType=%s\n",
                __FUNCTION__, sdrplay_tuner_name(tuner), sdrplay_duo_event(params->rspDuoModeParams.modeChangeType));

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
           sdrplay_exit (g_sdr_device);
           LOG_STDERR ("\nThe master stream no longer exists.\n"
                       "This application will now exit.\n");
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveDllDisappeared)
              sdr.slave_attached = false;
         break;

    case sdrplay_api_GainChange:
         TRACE (DEBUG_GENERAL2, "%s(): sdrplay_api_GainChange, tuner=%s gRdB=%d lnaGRdB=%d systemGain=%.2f\n",
                __FUNCTION__, sdrplay_tuner_name(tuner),
                params->gainParams.gRdB,
                params->gainParams.lnaGRdB,
                params->gainParams.currGain);
         break;

    case sdrplay_api_DeviceRemoved:
         TRACE (DEBUG_GENERAL2, "%s(): sdrplay_api_DeviceRemoved.\n", __FUNCTION__);
         break;

    default:
         TRACE (DEBUG_GENERAL2, "%s(): unknown event %d\n", __FUNCTION__, event_id);
         break;
  }

  LeaveCriticalSection (&Modes.print_mutex);
  MODES_NOTUSED (cb_context);
}

/**
 * The main SDRplay stream callback.
 */
#define USE_8BIT_SAMPLES 1

static void sdrplay_callback_A (short *xi, short *xq,
                                sdrplay_api_StreamCbParamsT *params,
                                unsigned int                 num_samples,
                                unsigned int                 reset,
                                void                        *cb_context)
{
  int      i, count1, count2;
  int      sig_i, sig_q, max_sig;
  bool     new_buf_flag;
  uint32_t end, input_index;
  uint32_t rx_data_idx = sdr.rx_data_idx;
  int      max_sig_acc = sdr.max_sig;

#if USE_8BIT_SAMPLES
  uint8_t  *dptr = (uint8_t*) sdr.rx_data;
#else
  uint16_t *dptr = (uint16_t*) sdr.rx_data;
#endif

  /* 'count1' is lesser of input samples and samples to end of buffer.
   * 'count2' is the remainder, generally zero
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
    sig_i = xi [input_index];
    dptr [rx_data_idx++] = sig_i;

    sig_q = xq [input_index++];
    dptr [rx_data_idx++] = sig_q;

    if (sig_i > max_sig)
       max_sig = sig_i;
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
      sdr.chParams->tunerParams.gain.gRdB += 1;
      if (sdr.chParams->tunerParams.gain.gRdB > 59)
         sdr.chParams->tunerParams.gain.gRdB = 59;
      CALL_FUNC (sdrplay_api_Update, g_sdr_handle, sdr.dev->tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }
    else if (max_sig < RSP_MIN_GAIN_THRESH)
    {
      sdr.chParams->tunerParams.gain.gRdB -= 1;
      if (sdr.chParams->tunerParams.gain.gRdB < 0)
         sdr.chParams->tunerParams.gain.gRdB = 0;
      CALL_FUNC (sdrplay_api_Update, g_sdr_handle, sdr.dev->tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }

    LeaveCriticalSection (&Modes.print_mutex);
  }

  /* Insert any remaining signal at start of buffer
   */
  for (i = (count2 >> 1) - 1; i >= 0; i--)
  {
    sig_i = xi [input_index];
    dptr [rx_data_idx++] = sig_i;

    sig_q = xq [input_index++];
    dptr [rx_data_idx++] = sig_q;
  }

  /* Send buffer downstream if enough available
   */
  if (new_buf_flag)
  {
    /* Go back by one buffer length, then round down further to start of buffer
     */
    end = rx_data_idx + MODES_RSP_BUF_SIZE * (MODES_RSP_BUFFERS-1);
    end &= MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS - 1;
    end &= ~(MODES_RSP_BUF_SIZE-1);

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
static void sdrplay_callback_B (short *xi, short *xq,
                                sdrplay_api_StreamCbParamsT *params,
                                unsigned int num_samples,
                                unsigned int reset,
                                void *cb_context)
{
  MODES_NOTUSED (xi);
  MODES_NOTUSED (xq);
  MODES_NOTUSED (params);
  MODES_NOTUSED (num_samples);
  MODES_NOTUSED (reset);
  MODES_NOTUSED (cb_context);
}

/**
 *
 */
static void sdrplay_select (const char *name)
{
  unsigned i;
  sdrplay_api_DeviceT *device;

  CALL_FUNC (sdrplay_api_LockDeviceApi);
  if (sdr.last_rc != sdrplay_api_Success)
     return;

  sdr.API_locked = true;

  CALL_FUNC (sdrplay_api_GetDevices, sdr.devices, &sdr.num_devices, DIM(sdr.devices));
  if (sdr.num_devices == 0)
  {
    LOG_STDERR ("No SDRplay devices found.\n");
    return;
  }

  device = sdr.devices + 0;

  TRACE (DEBUG_GENERAL, "wanted name: \"%s\".\n", name);

  for (i = 0; i < sdr.num_devices; i++)
  {
    if (sdr.devices[i].hwVer == SDRPLAY_RSP1A_ID)
          TRACE (DEBUG_GENERAL, "Device Index %d: RSP1A  - SerialNumber = %s\n", i, sdr.devices[i].SerNo);
    else if (sdr.devices[i].hwVer == SDRPLAY_RSPduo_ID)
         TRACE (DEBUG_GENERAL, "Device Index %d: RSPduo - SerialNumber = %s\n", i, sdr.devices[i].SerNo);
    else TRACE (DEBUG_GENERAL, "Device Index %d: RSP%d   - SerialNumber = %s\n", i, sdr.devices[i].hwVer, sdr.devices[i].SerNo);
  }

  CALL_FUNC (sdrplay_api_SelectDevice, device);
  if (sdr.last_rc == sdrplay_api_Success)
  {
    sdr.dev = device;
    g_sdr_device = &sdr;
    g_sdr_handle = device->dev;
  }
}

/**
 * This routine should be called from the main application in a separate thread.
 *
 * It enters an infinite loop only returning when the main application sets
 * the stop-condition specified in the `context`.
 *
 * \param[in] device   The device handle which is ignored. Since it's already
 *                     retured in `sdrplay_init()` (we support only one device at a time).
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
  MODES_NOTUSED (buf_num);
  MODES_NOTUSED (buf_len);

  if (device != g_sdr_device)
  {
    strncpy (sdr.last_err, "No device", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_NotInitialised;
    return (sdr.last_rc);
  }

  sdr.chParams = (sdr.dev->tuner == sdrplay_api_Tuner_A) ? sdr.deviceParams->rxChannelA: sdr.deviceParams->rxChannelB;

  sdr.chParams->ctrlParams.dcOffset.IQenable = 0;
  sdr.chParams->ctrlParams.dcOffset.DCenable = 1;

  sdr.cbFns.StreamACbFn = sdrplay_callback_A;
  sdr.cbFns.StreamBCbFn = sdrplay_callback_B;
  sdr.cbFns.EventCbFn   = sdrplay_event_callback;
  sdr.rx_callback       = callback;
  sdr.rx_context        = context;

  if (sdr.dev->hwVer != SDRPLAY_RSP1_ID)
     sdr.chParams->tunerParams.gain.minGr = sdrplay_api_EXTENDED_MIN_GR;

  sdr.chParams->tunerParams.gain.gRdB = Modes.sdrplay.gain_reduction;
  sdr.chParams->tunerParams.gain.LNAstate = 0;

  sdr.chParams->ctrlParams.agc.enable = Modes.dig_agc;

  sdr.chParams->tunerParams.dcOffsetTuner.dcCal = 4;
  sdr.chParams->tunerParams.dcOffsetTuner.speedUp = 0;
  sdr.chParams->tunerParams.dcOffsetTuner.trackTime = 63;

  if (sdr.dev->hwVer != SDRPLAY_RSPduo_ID || sdr.dev->rspDuoMode != sdrplay_api_RspDuoMode_Slave)
     sdr.deviceParams->devParams->fsFreq.fsHz = Modes.sample_rate;

  if (sdr.dev->hwVer == SDRPLAY_RSPduo_ID && (sdr.dev->rspDuoMode & sdrplay_api_RspDuoMode_Slave))
  {
    if ((uint32_t)sdr.dev->rspDuoSampleFreq != Modes.sample_rate)
    {
      strncpy (sdr.last_err, "RSPduo Master tuner in use and is not running in ADS-B compatible mode", sizeof(sdr.last_err));
      LOG_STDERR ("Error: %s.\n"
                  "Set the Master tuner to ADS-B compatible mode and restart %s.\n",
                  sdr.last_err, Modes.who_am_I);
      return (sdrplay_api_InvalidParam);
    }
  }

  if (sdr.dev->hwVer == SDRPLAY_RSP1A_ID)
  {
    sdr.chParams->rsp1aTunerParams.biasTEnable                = Modes.bias_tee;
    sdr.deviceParams->devParams->rsp1aParams.rfNotchEnable    = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.deviceParams->devParams->rsp1aParams.rfDabNotchEnable = (1 - Modes.sdrplay.disable_DAB_notch);
  }
  else if (sdr.dev->hwVer == SDRPLAY_RSP2_ID)
  {
    sdr.chParams->rsp2TunerParams.biasTEnable   = Modes.bias_tee;
    sdr.chParams->rsp2TunerParams.rfNotchEnable = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.chParams->rsp2TunerParams.amPortSel     = sdrplay_api_Rsp2_AMPORT_2;
    sdr.chParams->rsp2TunerParams.antennaSel    = Modes.sdrplay.antenna_port;
  }
  else if (sdr.dev->hwVer == SDRPLAY_RSPdx_ID)
  {
    sdr.deviceParams->devParams->rspDxParams.biasTEnable      = Modes.bias_tee;
    sdr.deviceParams->devParams->rspDxParams.rfNotchEnable    = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.deviceParams->devParams->rspDxParams.antennaSel       = Modes.sdrplay.DX_antenna_port;
    sdr.deviceParams->devParams->rspDxParams.rfDabNotchEnable = (1 - Modes.sdrplay.disable_DAB_notch);
  }
  else if (sdr.dev->hwVer == SDRPLAY_RSPduo_ID)
  {
    sdr.chParams->rspDuoTunerParams.biasTEnable      = Modes.bias_tee;
    sdr.chParams->rspDuoTunerParams.rfNotchEnable    = (1 - Modes.sdrplay.disable_broadcast_notch);
    sdr.chParams->rspDuoTunerParams.rfDabNotchEnable = (1 - Modes.sdrplay.disable_DAB_notch);
  }

  switch (Modes.sdrplay.ADSB_mode)
  {
    case 0:
         sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_DECIMATION;
         break;
    case 1:
         sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_NO_DECIMATION_LOWPASS;
         break;
    case 2:
         sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ;
         break;
    case 3:
         sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ;
         break;
  }

  if (!Modes.sdrplay.if_mode)
  {
    if (!Modes.sdrplay.over_sample)
    {
      sdr.chParams->ctrlParams.decimation.enable = 1;
      sdr.chParams->ctrlParams.decimation.decimationFactor = 4;
    }
    else
      sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_DECIMATION;
  }

  CALL_FUNC (sdrplay_api_Init, g_sdr_handle, &sdr.cbFns, NULL);
  if (sdr.last_rc != sdrplay_api_Success)
     return (sdr.last_rc);

  sdr.chParams->tunerParams.rfFreq.rfHz = Modes.freq;
  CALL_FUNC (sdrplay_api_Update, g_sdr_handle, sdr.dev->tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
  if (sdr.last_rc != sdrplay_api_Success)
     return (sdr.last_rc);

  while (1)
  {
    volatile int *exit = (volatile int*) context;

    if (context && *exit)
       break;
    Sleep (1000);
    TRACE (DEBUG_GENERAL, "rx_num_callbacks: %llu, sdr.max_sig: %d, sdr.rx_data_idx: %u.\n",
           sdr.rx_num_callbacks, sdr.max_sig, sdr.rx_data_idx);
  }
  return (0);
}

/**
 *
 */
int sdrplay_cancel_async (sdrplay_dev *device)
{
  if (device != g_sdr_device)
  {
    strncpy (sdr.last_err, "No device", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_NotInitialised;
  }
  else if (g_sdr_device->cancelling)
  {
    strncpy (sdr.last_err, "Cancelling", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_StopPending;
  }
  else
  {
    g_sdr_device->cancelling = true;
    CALL_FUNC (sdrplay_api_Uninit, g_sdr_handle);
  }
  return (sdr.last_rc);
}

/**
 *
 */
const char *sdrplay_strerror (int rc)
{
  if (rc == 0 || !sdr.last_err[0])
     return ("<none>");
  return (sdr.last_err);
}

/**
 * Load all needed SDRplay functions dynamically.
 */
int sdrplay_init (const char *name, sdrplay_dev **device)
{
  *device = NULL;

  g_sdr_device = NULL;
  g_sdr_handle = NULL;

  sdr.cancelling = false;
  sdr.API_locked = false;
  sdr.dev        = NULL;

  Modes.sdrplay.gain_reduction = MODES_RSP_INITIAL_GR;
  Modes.sdrplay.disable_broadcast_notch = true;
  Modes.sdrplay.disable_DAB_notch       = true;

  Modes.sdrplay.antenna_port    = sdrplay_api_Rsp2_ANTENNA_B;
  Modes.sdrplay.DX_antenna_port = sdrplay_api_RspDx_ANTENNA_B;
  Modes.sdrplay.tuner           = sdrplay_api_Tuner_B;           /* RSPduo default */
  Modes.sdrplay.mode            = sdrplay_api_RspDuoMode_Master; /* RSPduo default */
  Modes.sdrplay.BW_mode         = 1;  /* 5 MHz */
  Modes.sdrplay.ADSB_mode       = 1;  /* for Zero-IF */

  sdr.rx_data = malloc (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS * sizeof(short));
  if (!sdr.rx_data)
  {
    strncpy (sdr.last_err, "Insufficient memory for buffers", sizeof(sdr.last_err));
    goto failed;
  }

  sdr.dll_hnd = LoadLibrary (sdr.dll_name);
  if (!sdr.dll_hnd)
  {
    DWORD err = GetLastError();

    /* The 'LoadLibrary()' will fail with 'GetLastError() ==  ERROR_BAD_EXE_FORMAT' (193)
     * if we're running a 32-bit Dump1090 and loaded a 64-bit "sdrplay_api.dll".
     * And vice-versa.
     */
    if (err == ERROR_BAD_EXE_FORMAT)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "%s is not a %d bit version", sdr.dll_name, 8*(int)sizeof(void*));
    else snprintf (sdr.last_err, sizeof(sdr.last_err), "Failed to load %s; %lu", sdr.dll_name, GetLastError());
    goto failed;
  }

  LOAD_FUNC (sdrplay_api_Open);
  LOAD_FUNC (sdrplay_api_Close);
  LOAD_FUNC (sdrplay_api_Init);
  LOAD_FUNC (sdrplay_api_Uninit);
  LOAD_FUNC (sdrplay_api_ApiVersion);
  LOAD_FUNC (sdrplay_api_DebugEnable);
  LOAD_FUNC (sdrplay_api_LockDeviceApi);
  LOAD_FUNC (sdrplay_api_UnlockDeviceApi);
  LOAD_FUNC (sdrplay_api_GetDevices);
  LOAD_FUNC (sdrplay_api_GetDeviceParams);
  LOAD_FUNC (sdrplay_api_SelectDevice);
  LOAD_FUNC (sdrplay_api_ReleaseDevice);
  LOAD_FUNC (sdrplay_api_Update);
  LOAD_FUNC (sdrplay_api_GetErrorString);

  CALL_FUNC (sdrplay_api_Open);
  CALL_FUNC (sdrplay_api_ApiVersion, &sdr.version);
  if (sdr.last_rc != sdrplay_api_Success)
     goto failed;

  TRACE (DEBUG_GENERAL, "sdrplay_api_ApiVersion(): '%.2f', build version: '%.2f'.\n", sdr.version, SDRPLAY_API_VERSION);
  if (sdr.version != SDRPLAY_API_VERSION || sdr.version < 3.06F)
     goto failed;

  sdrplay_select (name);
  if (!g_sdr_device)
     goto failed;

  if (Modes.debug & DEBUG_GENERAL)
     CALL_FUNC (sdrplay_api_DebugEnable, g_sdr_handle, sdrplay_api_DbgLvl_Verbose);

  CALL_FUNC (sdrplay_api_GetDeviceParams, g_sdr_handle, &sdr.deviceParams);
  if (sdr.last_rc != sdrplay_api_Success)
     goto failed;

  if (!sdr.deviceParams)
  {
    TRACE (DEBUG_GENERAL, "sdrplay_api_GetDeviceParams() failed: %s'.\n", sdr.last_err);
    goto failed;
  }

  *device = g_sdr_device;
  return (sdrplay_api_Success);

failed:
  TRACE (DEBUG_GENERAL, "%s.\n", sdr.last_err);
  sdrplay_exit (NULL);
  return (sdrplay_api_Fail);  /* A better error-code? */
}

/**
 * Free the API and the device.
 */
static int sdrplay_release (sdrplay_dev *device)
{
  if (device != g_sdr_device || !g_sdr_device) /* support only 1 device */
  {
    strncpy (sdr.last_err, "No device", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_NotInitialised;
  }
  else
  {
    if (!sdr.API_locked)
       CALL_FUNC (sdrplay_api_LockDeviceApi);

    if (!g_sdr_device->cancelling)
       CALL_FUNC (sdrplay_api_Uninit, g_sdr_handle);

    CALL_FUNC (sdrplay_api_ReleaseDevice, sdr.dev);

    if (sdr.API_locked)
       CALL_FUNC (sdrplay_api_UnlockDeviceApi);
  }

  sdr.API_locked = false;
  g_sdr_device   = NULL;
  g_sdr_handle   = NULL;
  return (sdr.last_rc);
}

/**
 *
 */
int sdrplay_exit (sdrplay_dev *device)
{
  sdrplay_api_ErrT rc = sdrplay_api_Success;  /* Assume success */

  if (device)
     sdrplay_release (device);

  free (sdr.rx_data);
  sdr.rx_data = NULL;

  if (!sdr.dll_hnd)
  {
    strncpy (sdr.last_err, "No DLL loaded", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_NotInitialised;
    return (sdr.last_rc);
  }

  CALL_FUNC (sdrplay_api_Close);
  FreeLibrary (sdr.dll_hnd);

  sdr.dll_hnd = NULL;
  g_sdr_device = NULL;
  g_sdr_handle = NULL;
  return (sdr.last_rc);
}
