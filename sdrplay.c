/**
 * \file    sdrplay.c
 * \ingroup Main
 * \brief   The interface for SDRplay devices.
 *
 * Load all needed functions from "sdrplay_api.dll" dynamically.
 */
#if defined(USE_RTLSDR_EMUL)
#error "Do not compile this file when 'USE_RTLSDR_EMUL' is defined."
#endif

#include <assert.h>
#include "sdrplay.h"
#include "misc.h"

#define MODES_RSP_BUF_SIZE  (16*16384)   /* 256k, same as MODES_DATA_LEN  */
#define MODES_RSP_BUFFERS    16          /* Must be power of 2 */

#define RSP_MIN_GAIN_THRESH  512         /* Increase gain if peaks below this */
#define RSP_MAX_GAIN_THRESH 1024         /* Decrease gain if peaks above this */
#define RSP_ACC_SHIFT         13         /* Sets time constant of averaging filter */
#define MODES_RSP_INITIAL_GR  20

#define TRACE(fmt, ...) do {                                                \
                          if (Modes.debug & DEBUG_GENERAL)                  \
                             modeS_flogf (stdout, "%s(%u): " fmt,           \
                                          __FILE__, __LINE__, __VA_ARGS__); \
                        } while (0)

/**
 * \def LOAD_FUNC(func)
 *   A `GetProcAddress()` helper.
 *   \param func  the name of the function (without any `"`).
 */
#define LOAD_FUNC(func)                                                    \
        do {                                                               \
          sdr.func = (func ## _t) GetProcAddress (sdr.dll_hnd, #func);     \
          if (!sdr.func)                                                   \
          {                                                                \
            snprintf (sdr.last_err, sizeof(sdr.last_err),                  \
                      "Failed to find '%s()' in %s", #func, sdr.dll_name); \
            goto failed;                                                   \
          }                                                                \
          TRACE ("Function: %-30s -> 0x%p.\n", #func, sdr.func);           \
        } while (0)

#define CALL_FUNC(func, ...)                                      \
        do {                                                      \
          sdrplay_api_ErrT rc;                                    \
          if (!sdr.func)                                          \
               rc = sdrplay_api_NotInitialised;                   \
          else rc = (*sdr.func) (__VA_ARGS__);                    \
          if (rc != sdrplay_api_Success)                          \
          {                                                       \
            sdrplay_store_error (rc);                             \
            TRACE ( "%s(): %d / %s.\n", #func, rc, sdr.last_err); \
          }                                                       \
          else                                                    \
            TRACE ( "%s(): OKAY.\n", #func);                      \
        } while (0)

struct sdrplay_priv {
       const char                    *dll_name;
       HANDLE                         dll_hnd;
       char                           full_dll_name [MG_PATH_MAX];  /**< The full name of the `sdrplay_api.dll`. */

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

static struct sdrplay_priv sdr = { "sdrplay_api.dll" };

/**
 * We support only 1 device at a time.
 */
static struct sdrplay_priv *g_sdr_device;
static HANDLE               g_sdr_handle;

/* 4 - 44 dB
 */
static int gain_table[10] = { 40, 100, 150, 170, 210, 260, 310, 350, 390, 440 };

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
  if (g_sdr_device->cancelling)
     return;

  EnterCriticalSection (&Modes.print_mutex);

  switch (event_id)
  {
    case sdrplay_api_PowerOverloadChange:
         TRACE ("%s(): sdrplay_api_PowerOverloadChange: sdrplay_api_AgcEvent, tuner=%s powerOverloadChangeType=%s\n",
                __FUNCTION__, sdrplay_tuner_name(tuner), sdrplay_overload_name(params->powerOverloadParams.powerOverloadChangeType));

         CALL_FUNC (sdrplay_api_Update, g_sdr_handle, tuner,
                    sdrplay_api_Update_Ctrl_OverloadMsgAck,
                    sdrplay_api_Update_Ext1_None);
         break;

    case sdrplay_api_RspDuoModeChange:
         TRACE ("%s(): sdrplay_api_RspDuoModeChange, tuner=%s modeChangeType=%s\n",
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
         TRACE ("%s(): sdrplay_api_GainChange, tuner=%s gRdB=%d lnaGRdB=%d systemGain=%.2f\n",
                __FUNCTION__, sdrplay_tuner_name(tuner),
                params->gainParams.gRdB,
                params->gainParams.lnaGRdB,
                params->gainParams.currGain);
         break;

    case sdrplay_api_DeviceRemoved:
         TRACE ("%s(): sdrplay_api_DeviceRemoved.\n", __FUNCTION__);
         break;

    case sdrplay_api_DeviceFailure:
         TRACE ("%s(): sdrplay_api_DeviceFailure.\n", __FUNCTION__);
         break;

    default:
         TRACE ("%s(): unknown event %d\n", __FUNCTION__, event_id);
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
  int      sig_I, sig_Q, max_sig;
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
static bool sdrplay_select (const char *wanted_name, int wanted_index)
{
  sdrplay_api_DeviceT *device;
  int     i, select_this = -1;
  char    current_dev [100];
  bool    select_first = true;

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

  device = sdr.devices + 0;

  TRACE ("wanted_name: \"sdrplay%s\", wanted_index: %d. Found %d devices.\n",
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
    else if (sdr.devices[i].hwVer == SDRPLAY_RSP2_ID)
    {
      strcpy (current_dev, "RSP2");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSPdx_ID)
    {
      strcpy (current_dev, "RSP2");
    }
    else if (sdr.devices[i].hwVer == SDRPLAY_RSPduo_ID)
    {
      strcpy (current_dev, "RSPduo");
    }
    else
    {
      snprintf (current_dev, sizeof(current_dev), "RSP%d !!??", sdr.devices[i].hwVer);
    }

    TRACE ("Device Index %d: %s   - SerialNumber = %s\n", i, current_dev, sdr.devices[i].SerNo);

    if (select_this == -1)
    {
      if (select_first)
         select_this = i;
      else if (i == wanted_index)
         select_this = i;
      else if (!stricmp(current_dev, wanted_name))
         select_this = i;
    }
  }

  if (select_this == -1)
  {
    LOG_STDERR ("Wanted device \"sdrplay%s\" (at index: %d) not found.\n", wanted_name, wanted_index);
    return (false);
  }

  CALL_FUNC (sdrplay_api_SelectDevice, device);
  if (sdr.last_rc == sdrplay_api_Success)
  {
    sdr.dev = device;
    g_sdr_device = &sdr;
    g_sdr_handle = device->dev;
    Modes.selected_dev = mg_mprintf ("sdrplay-%s", current_dev);
    return (true);
  }
  return (false);
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

  sdr.chParams = (sdr.dev->tuner == sdrplay_api_Tuner_A) ? sdr.deviceParams->rxChannelA :
                                                           sdr.deviceParams->rxChannelB;

#if 0
  sdr.chParams->ctrlParams.dcOffset.IQenable = 0;
  sdr.chParams->ctrlParams.dcOffset.DCenable = 1;
#else
  sdr.chParams->ctrlParams.dcOffset.IQenable = 1;
  sdr.chParams->ctrlParams.dcOffset.DCenable = 0;
#endif

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
     sdr.deviceParams->devParams->fsFreq.fsHz = Modes.sample_rate;  // was '8 * 1E6'

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

  if (Modes.sdrplay.if_mode == false)  /* Zero-IF mode */
  {
    if (!Modes.sdrplay.over_sample)
    {
      sdr.chParams->ctrlParams.decimation.enable = 1;
      sdr.chParams->ctrlParams.decimation.decimationFactor = 4;
    }
    else
    {
      sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_DECIMATION;
      sdr.chParams->ctrlParams.decimation.enable = 0;
      sdr.chParams->ctrlParams.decimation.decimationFactor = 1;
    }
  }

  {
    int tuner;

    if (sdr.chParams == sdr.deviceParams->rxChannelA)
         tuner = 'A';
    else if (sdr.chParams == sdr.deviceParams->rxChannelB)
         tuner = 'B';
    else tuner = '?';

    TRACE ("Tuner %c: sample-rate: %.0f MS/s, adsbMode: %s.\n"
           "                decimation-enable: %d, decimation-factor: %d.\n",
           tuner, sdr.deviceParams->devParams->fsFreq.fsHz / 1E6,
           sdrplay_adsb_mode(sdr.chParams->ctrlParams.adsbMode),
           sdr.chParams->ctrlParams.decimation.enable,
           sdr.chParams->ctrlParams.decimation.decimationFactor);
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
    Sleep (1000);
    if (*(volatile bool*) sdr.rx_context)
    {
      TRACE ("'exit' was set.\n");
      break;
    }

    TRACE ("rx_num_callbacks: %llu, sdr.max_sig: %6d, sdr.rx_data_idx: %6u.\n",
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
int sdrplay_init (const char *name, int index, sdrplay_dev **device)
{
  *device = NULL;

  TRACE ("name: '%s', index: %d\n", name, index);
  assert (strnicmp(name, "sdrplay", 7) == 0);

  g_sdr_device = NULL;
  g_sdr_handle = NULL;

  Modes.sdrplay.priv = calloc (sizeof(*Modes.sdrplay.priv), 1);
  if (!Modes.sdrplay.priv)
     goto nomem;

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
  Modes.sdrplay.over_sample     = true;

  sdr.rx_data = malloc (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS * sizeof(short));
  if (!sdr.rx_data)
     goto nomem;

  sdr.dll_hnd = LoadLibraryA (sdr.dll_name);
  if (!sdr.dll_hnd)
  {
    DWORD err = GetLastError();

    /* The 'LoadLibraryA()' will fail with 'GetLastError() ==  ERROR_BAD_EXE_FORMAT' (193)
     * if we're running a 32-bit Dump1090 and loaded a 64-bit "sdrplay_api.dll".
     * And vice-versa.
     */
    if (err == ERROR_BAD_EXE_FORMAT)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "%s is not a %d bit version", sdr.dll_name, 8*(int)sizeof(void*));
    if (err == ERROR_MOD_NOT_FOUND)
         snprintf (sdr.last_err, sizeof(sdr.last_err), "%s not found on PATH", sdr.dll_name);
    else snprintf (sdr.last_err, sizeof(sdr.last_err), "Failed to load %s; %lu", sdr.dll_name, GetLastError());
    goto failed;
  }

  if (!GetModuleFileNameA(sdr.dll_hnd, sdr.full_dll_name, sizeof(sdr.full_dll_name)))
     strcpy (sdr.full_dll_name, "?");
  TRACE ("sdrplay DLL: '%s'.\n", sdr.full_dll_name);

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
  if (sdr.last_rc != sdrplay_api_Success)
  {
    fprintf (stderr, "The SDRPlay API is not responding. A service restart could help:\n");
    fprintf (stderr, "  sc stop SDRplayAPIService & ping -w1 -n2 0.0.0.0 > NUL & sc start SDRplayAPIService\n");
    goto failed;
  }

  CALL_FUNC (sdrplay_api_ApiVersion, &sdr.version);
  if (sdr.last_rc != sdrplay_api_Success)
     goto failed;

  TRACE ("sdrplay_api_ApiVersion(): '%.2f', build version: '%.2f'.\n", sdr.version, SDRPLAY_API_VERSION);

  if (sdr.version == 3.10F && SDRPLAY_API_VERSION == 3.11F)
     TRACE ("ver 3.10 and ver 3.11 should be compatible.\n");

  else if (sdr.version != SDRPLAY_API_VERSION || sdr.version < 3.06F)
  {
    snprintf (sdr.last_err, sizeof(sdr.last_err), "Wrong sdrplay_api_ApiVersion(): '%.2f', build version: '%.2f'.\n",
              sdr.version, SDRPLAY_API_VERSION);
    goto failed;
  }

  if (!sdrplay_select(name+7, index))
     goto failed;

  if (Modes.debug & DEBUG_GENERAL)
     CALL_FUNC (sdrplay_api_DebugEnable, g_sdr_handle, sdrplay_api_DbgLvl_Verbose);

  CALL_FUNC (sdrplay_api_GetDeviceParams, g_sdr_handle, &sdr.deviceParams);
  if (sdr.last_rc != sdrplay_api_Success)
     goto failed;

  if (!sdr.deviceParams)
  {
    TRACE ("sdrplay_api_GetDeviceParams() failed: %s'.\n", sdr.last_err);
    goto failed;
  }

  *device = g_sdr_device;

  /* A fixed test
   */
  Modes.sdrplay.gains = malloc (10 * sizeof(int));
  if (!Modes.sdrplay.gains)
      goto nomem;

  Modes.sdrplay.gain_count = 10;
  memcpy (Modes.sdrplay.gains, &gain_table, 10 * sizeof(int));

  return (sdrplay_api_Success);

nomem:
  strncpy (sdr.last_err, "Insufficient memory", sizeof(sdr.last_err));

failed:
  LOG_STDERR ("%s.\n", sdr.last_err);
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
  if (device)
     sdrplay_release (device);

  free (sdr.rx_data);
  free (Modes.sdrplay.priv);
  sdr.rx_data        = NULL;
  Modes.sdrplay.priv = NULL;

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
