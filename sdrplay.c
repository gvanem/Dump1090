/*
 * To avoid a warning if this compilation unit is empty
 */
static int __dummy;

#ifdef USE_SDRPLAY   /* Rest of file */
/**
 * \file    sdrplay.c
 * \ingroup Main
 * \brief   The interface for SDRplay devices.
 */
#include <stdio.h>
#include <stdbool.h>
#include <windows.h>
#include <sdrplay_api.h>

#include "misc.h"
#include "sdrplay.h"

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
#define LOAD_FUNC(func)                                                    \
        do {                                                               \
          sdr.func = (func ## _t) GetProcAddress (sdr.dll_hnd, #func);     \
          if (!sdr.func)                                                   \
          {                                                                \
            snprintf (sdr.last_err, sizeof(sdr.last_err),                  \
                      "Failed to find '%s()' in %s", #func, sdr.dll_name); \
            goto failed;                                                   \
          }                                                                \
       /* TRACE (DEBUG_GENERAL2, "Function: %-30s -> 0x%p.\n", #func, sdr.func); */ \
        } while (0)

#define CALL_FUNC(func, ...)                                          \
        do {                                                          \
          sdrplay_api_ErrT rc;                                        \
          if (!sdr.func)                                              \
               rc = sdrplay_api_NotInitialised;                       \
          else rc = (*sdr.func) (__VA_ARGS__);                        \
          if (rc != sdrplay_api_Success)                              \
          {                                                           \
            sdrplay_store_error (rc);                                 \
            LOG_STDERR ("%s(): %d / %s.\n", #func, rc, sdr.last_err); \
          }                                                           \
        } while (0)

struct SDRplay_info {
       const char                    *dll_name;
       HANDLE                         dll_hnd;
       float                          version;
       bool                           API_locked;
       bool                           master_initialised;
       bool                           slave_uninitialised;
       bool                           slave_attached;

       sdrplay_api_DeviceT           *device;
       sdrplay_api_DeviceT            devices [4];
       unsigned int                   num_devices;
       char                           last_err [256];
       sdrplay_api_ErrT               last_rc;
       int                            max_sig;
       sdrplay_api_CallbackFnsT       cbFns;
       sdrplay_api_DeviceParamsT     *deviceParams;
       sdrplay_api_RxChannelParamsT  *chParams;
       uint16_t                      *rx_data;
       unsigned int                   rx_data_idx;
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

static struct modes {
       bool                             ifMode;
       bool                             oversample;
       bool                             enable_biasT;
       bool                             disableBroadcastNotch;
       bool                             disableDabNotch;
       int                              gain_reduction;
       int                              adsbMode;
       int                              bwMode;
       sdrplay_api_Rsp2_AntennaSelectT  antenna_port;
       sdrplay_api_RspDx_AntennaSelectT Dxantenna_port;
       sdrplay_api_TunerSelectT         tuner;
       sdrplay_api_RspDuoModeT          mode;
     } Modes;

/**
 * Store the last error-code and error-text from the
 * last `CALL_FUNC()` macro call.
 */
static void sdrplay_store_error (sdrplay_api_ErrT rc)
{
  sdr.last_rc = rc;

  if (sdr.sdrplay_api_GetErrorString)
       strncpy (sdr.last_err, (*sdr.sdrplay_api_GetErrorString)(rc), sizeof(sdr.last_err));
  else sdr.last_err[0] = '\0';
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
  switch (event_id)
  {
    case sdrplay_api_PowerOverloadChange:
         LOG_STDERR ("sdrplay_api_PowerOverloadChange: %s, tuner=%s powerOverloadChangeType=%s\n",
                     "sdrplay_api_AgcEvent",
                     (tuner == sdrplay_api_Tuner_A) ? "sdrplay_api_Tuner_A" : "sdrplay_api_Tuner_B",
                     (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected) ?
                     "sdrplay_api_Overload_Detected": "sdrplay_api_Overload_Corrected");
         CALL_FUNC (sdrplay_api_Update, sdr.device->dev, tuner,
                                        sdrplay_api_Update_Ctrl_OverloadMsgAck,
                                        sdrplay_api_Update_Ext1_None);
         break;

    case sdrplay_api_RspDuoModeChange:
         LOG_STDERR ("sdrplay_api_EventCb: %s, tuner=%s modeChangeType=%s\n",
                     "sdrplay_api_RspDuoModeChange",
                     (tuner == sdrplay_api_Tuner_A) ? "sdrplay_api_Tuner_A" : "sdrplay_api_Tuner_B",
                     (params->rspDuoModeParams.modeChangeType == sdrplay_api_MasterInitialised) ?
                     "sdrplay_api_MasterInitialised" :
                       (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveUninitialised) ?
                        "sdrplay_api_SlaveUninitialised" :
                        (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveAttached) ?
                        "sdrplay_api_SlaveAttached" :
                         (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveDetached) ?
                         "sdrplay_api_SlaveDetached" : "unknown type");

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
           sdrplay_exit (sdr.device);
           LOG_STDERR ("\nThe master stream no longer exists, which means that this slave stream "
                       "has been\nstopped and the slave application will need to be closed before "
                       "the stream can\nbe restarted. This has probably occurred because the master application has\n"
                       "either been closed or has crashed. Please always ensure that the slave\n"
                       "application is closed before closing down or killing the master application.\n\n"
                       "This application will now exit\n");
         }
         else if (params->rspDuoModeParams.modeChangeType == sdrplay_api_SlaveDllDisappeared)
              sdr.slave_attached = false;
         break;

    case sdrplay_api_GainChange:
         LOG_STDERR ("sdrplay_api_EventCb: %s, tuner=%s gRdB=%d lnaGRdB=%d systemGain=%.2f\n", "sdrplay_api_GainChange",
                     (tuner == sdrplay_api_Tuner_A) ? "sdrplay_api_Tuner_A": "sdrplay_api_Tuner_B",
                     params->gainParams.gRdB,
                     params->gainParams.lnaGRdB,
                     params->gainParams.currGain);
         break;

    case sdrplay_api_DeviceRemoved:
         LOG_STDERR ("sdrplay_api_EventCb: %s\n", "sdrplay_api_DeviceRemoved");
         break;

    default:
         LOG_STDERR ("sdrplay_api_EventCb: %d, unknown event\n", event_id);
         break;
  }
  MODES_NOTUSED (cb_context);
}

/**
 * The main SDRplay stream callback.
 */
static void sdrplay_callback_A (short *xi, short *xq,
                                sdrplay_api_StreamCbParamsT *params,
                                unsigned int num_samples,
                                unsigned int reset,
                                void        *cb_context)
{
  int  i, count1, count2, new_buf_flag;
  int  sig_i, sig_q, max_sig;
  unsigned int end, input_index;

  // Initialise heavily-used locals from 'sdr' struct
  uint16_t *dptr = (uint16_t*) sdr.rx_data;

  int max_sig_acc = sdr.max_sig;
  unsigned int rx_data_idx = sdr.rx_data_idx;

  /* count1 is lesser of input samples and samples to end of buffer */
  /* count2 is the remainder, generally zero */

  end = rx_data_idx + (num_samples << 1);
  count2 = end - (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS);
  if (count2 < 0)
     count2 = 0;            /* count2 is samples wrapping around to start of buf */

  count1 = (num_samples << 1) - count2;   /* count1 is samples fitting before the end of buf */

  /* Flag is set if this packet takes us past a multiple of MODES_RSP_BUF_SIZE
   */
  new_buf_flag = ((rx_data_idx & (MODES_RSP_BUF_SIZE-1)) < (end & (MODES_RSP_BUF_SIZE-1))) ? 0 : 1;

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

  /* apply slowly decaying filter to max signal value
   */
  max_sig -= 127;
  max_sig_acc += max_sig;
  max_sig = max_sig_acc >> RSP_ACC_SHIFT;
  max_sig_acc -= max_sig;

  /* this code is triggered as we reach the end of our circular buffer
   */
  if (rx_data_idx >= (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS))
  {
    rx_data_idx = 0;  // pointer back to start of buffer */

    /* adjust gain if required
     */
    if (max_sig > RSP_MAX_GAIN_THRESH)
    {
      sdr.chParams->tunerParams.gain.gRdB += 1;
      if (sdr.chParams->tunerParams.gain.gRdB > 59)
         sdr.chParams->tunerParams.gain.gRdB = 59;
      CALL_FUNC (sdrplay_api_Update, sdr.device->dev, sdr.device->tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }
    if (max_sig < RSP_MIN_GAIN_THRESH)
    {
      sdr.chParams->tunerParams.gain.gRdB -= 1;
      if (sdr.chParams->tunerParams.gain.gRdB < 0)
         sdr.chParams->tunerParams.gain.gRdB = 0;
      CALL_FUNC (sdrplay_api_Update, sdr.device->dev, sdr.device->tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }
  }

  /* insert any remaining signal at start of buffer
   */

  for (i = (count2 >> 1) - 1; i >= 0; i--)
  {
    sig_i = xi[input_index];
    dptr [rx_data_idx++] = sig_i;

    sig_q = xq[input_index++];
    dptr [rx_data_idx++] = sig_q;
  }

  /* send buffer downstream if enough available
   */
  if (new_buf_flag)
  {
    /* go back by one buffer length, then round down further to start of buffer
     */
    end = rx_data_idx + MODES_RSP_BUF_SIZE * (MODES_RSP_BUFFERS-1);
    end &= MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS - 1;
    end &= ~(MODES_RSP_BUF_SIZE-1);

    sdr.rx_num_callbacks++;
    (*sdr.rx_callback) ((uint8_t*)sdr.rx_data + end, MODES_RSP_BUF_SIZE, sdr.rx_context);
  }

  /* stash static values in Modes struct */
  sdr.max_sig = max_sig_acc;
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
static sdrplay_api_DeviceT *sdrplay_select (const char *name)
{
  unsigned i;

  CALL_FUNC (sdrplay_api_LockDeviceApi);
  if (sdr.last_rc != sdrplay_api_Success)
     return (NULL);

  sdr.API_locked = true;

  CALL_FUNC (sdrplay_api_GetDevices, sdr.devices, &sdr.num_devices, DIM(sdr.devices));
  if (sdr.num_devices == 0)
  {
    LOG_STDERR ("No SDRplay devices found.\n");
    return (NULL);
  }

  sdrplay_api_DeviceT *device = sdr.devices + 0;

  for (i = 0; i < sdr.num_devices; i++)
  {
    if (sdr.devices[i].hwVer == SDRPLAY_RSP1A_ID)
          LOG_STDERR ("Device Index %d: RSP1A  - SerialNumber = %s\n", i, sdr.devices[i].SerNo);
    else if (sdr.devices[i].hwVer == SDRPLAY_RSPduo_ID)
         LOG_STDERR ("Device Index %d: RSPduo - SerialNumber = %s\n", i, sdr.devices[i].SerNo);
    else LOG_STDERR ("Device Index %d: RSP%d   - SerialNumber = %s\n", i, sdr.devices[i].hwVer, sdr.devices[i].SerNo);
  }

  CALL_FUNC (sdrplay_api_SelectDevice, device);
  if (sdr.last_rc != sdrplay_api_Success)
     return (NULL);
  return (device);
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
int sdrplay_read_async (sdrplay_dev device,
                        sdrplay_cb  callback,
                        void       *context,
                        uint32_t    buf_num,
                        uint32_t    buf_len)
{
  sdr.chParams = (sdr.device->tuner == sdrplay_api_Tuner_A) ? sdr.deviceParams->rxChannelA: sdr.deviceParams->rxChannelB;

  sdr.chParams->ctrlParams.dcOffset.IQenable = 0;
  sdr.chParams->ctrlParams.dcOffset.DCenable = 1;

  sdr.cbFns.StreamACbFn = sdrplay_callback_A;
  sdr.cbFns.StreamBCbFn = sdrplay_callback_B;
  sdr.cbFns.EventCbFn   = sdrplay_event_callback;
  sdr.rx_callback       = callback;
  sdr.rx_context        = context;

  if (sdr.device->hwVer != SDRPLAY_RSP1_ID)
     sdr.chParams->tunerParams.gain.minGr = sdrplay_api_EXTENDED_MIN_GR;

  sdr.chParams->tunerParams.gain.gRdB = Modes.gain_reduction;
  sdr.chParams->tunerParams.gain.LNAstate = 0;

  sdr.chParams->ctrlParams.agc.enable = 0;

  sdr.chParams->tunerParams.dcOffsetTuner.dcCal = 4;
  sdr.chParams->tunerParams.dcOffsetTuner.speedUp = 0;
  sdr.chParams->tunerParams.dcOffsetTuner.trackTime = 63;

  if (sdr.device->hwVer != SDRPLAY_RSPduo_ID || sdr.device->rspDuoMode != sdrplay_api_RspDuoMode_Slave)
     sdr.deviceParams->devParams->fsFreq.fsHz = 8.0 * 1E6;

  if (sdr.device->hwVer == SDRPLAY_RSPduo_ID && (sdr.device->rspDuoMode & sdrplay_api_RspDuoMode_Slave))
  {
    if (sdr.device->rspDuoSampleFreq != 8000000.0)
    {
      LOG_STDERR ("Error: RSPduo Master tuner in use and is not running in ADS-B compatible mode.\n"
                  "Set the Master tuner to ADS-B compatible mode and restart dump1090\n");
      return (1);
    }
  }

  if (sdr.device->hwVer == SDRPLAY_RSP1A_ID)
  {
    sdr.chParams->rsp1aTunerParams.biasTEnable                = Modes.enable_biasT;
    sdr.deviceParams->devParams->rsp1aParams.rfNotchEnable    = (1 - Modes.disableBroadcastNotch);
    sdr.deviceParams->devParams->rsp1aParams.rfDabNotchEnable = (1 - Modes.disableDabNotch);
  }
  else if (sdr.device->hwVer == SDRPLAY_RSP2_ID)
  {
    sdr.chParams->rsp2TunerParams.biasTEnable   = Modes.enable_biasT;
    sdr.chParams->rsp2TunerParams.rfNotchEnable = (1 - Modes.disableBroadcastNotch);
    sdr.chParams->rsp2TunerParams.amPortSel     = sdrplay_api_Rsp2_AMPORT_2;
    sdr.chParams->rsp2TunerParams.antennaSel    = Modes.antenna_port;
  }
  else if (sdr.device->hwVer == SDRPLAY_RSPdx_ID)
  {
    sdr.deviceParams->devParams->rspDxParams.biasTEnable      = Modes.enable_biasT;
    sdr.deviceParams->devParams->rspDxParams.rfNotchEnable    = (1 - Modes.disableBroadcastNotch);
    sdr.deviceParams->devParams->rspDxParams.antennaSel       = Modes.Dxantenna_port;
    sdr.deviceParams->devParams->rspDxParams.rfDabNotchEnable = (1 - Modes.disableDabNotch);
  }
  else if (sdr.device->hwVer == SDRPLAY_RSPduo_ID)
  {
    sdr.chParams->rspDuoTunerParams.biasTEnable      = Modes.enable_biasT;
    sdr.chParams->rspDuoTunerParams.rfNotchEnable    = (1 - Modes.disableBroadcastNotch);
    sdr.chParams->rspDuoTunerParams.rfDabNotchEnable = (1 - Modes.disableDabNotch);
  }

  switch (Modes.adsbMode)
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

  if (Modes.ifMode == 0)
  {
    if (Modes.oversample == 0)
    {
      sdr.chParams->ctrlParams.decimation.enable = 1;
      sdr.chParams->ctrlParams.decimation.decimationFactor = 4;
    }
    else
      sdr.chParams->ctrlParams.adsbMode = sdrplay_api_ADSB_DECIMATION;
  }

  CALL_FUNC (sdrplay_api_Init, sdr.device->dev, &sdr.cbFns, NULL);
  if (sdr.last_rc != sdrplay_api_Success)
     return (sdr.last_rc);

  sdr.chParams->tunerParams.rfFreq.rfHz = 1090.0 * 1E6;
  CALL_FUNC (sdrplay_api_Update, sdr.device->dev, sdr.device->tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
  if (sdr.last_rc != sdrplay_api_Success)
     return (sdr.last_rc);

  while (1)
  {
    volatile int *exit = (volatile int*) context;

    if (context && *exit)
       break;
    Sleep (1000);
    LOG_STDERR ("rx_num_callbacks: %llu, sdr.max_sig: %d, sdr.rx_data_idx: %u.\n",
                sdr.rx_num_callbacks, sdr.max_sig, sdr.rx_data_idx);
  }
  return (0);
}

int sdrplay_cancel_async (sdrplay_dev device)
{
  if (device != sdr.device)
  {
    strncpy (sdr.last_err, "No device", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_NotInitialised;
  }
  else
    CALL_FUNC (sdrplay_api_Uninit, sdr.device->dev);
  return (sdr.last_rc);
}

/**
 *
 */
const char *sdrplay_error (int rc)
{
  if (!sdr.last_err[0])
     return ("<none>");
  return (sdr.last_err);
}

/**
 * Load all need SDRplay functions dynamically.
 */
int sdrplay_init (const char *name, sdrplay_dev *device)
{
  *device = NULL;
  sdr.device = NULL;

  Modes.gain_reduction = MODES_RSP_INITIAL_GR;
  Modes.enable_biasT          = false;
  Modes.disableBroadcastNotch = true;
  Modes.disableDabNotch       = true;

  Modes.antenna_port   = sdrplay_api_Rsp2_ANTENNA_B;
  Modes.Dxantenna_port = sdrplay_api_RspDx_ANTENNA_B;
  Modes.tuner          = sdrplay_api_Tuner_B;           // RSPduo default
  Modes.mode           = sdrplay_api_RspDuoMode_Master; // RSPduo default
  Modes.bwMode         = 1;  // 5 MHz
  Modes.adsbMode       = 1;  // for Zero-IF

  sdr.rx_data = malloc (MODES_RSP_BUF_SIZE * MODES_RSP_BUFFERS * sizeof(short));
  if (!sdr.rx_data)
  {
    strncpy (sdr.last_err, "Insufficient memory for buffers", sizeof(sdr.last_err));
    goto failed;
  }

  sdr.dll_hnd = LoadLibrary (sdr.dll_name);
  if (!sdr.dll_hnd)
  {
    snprintf (sdr.last_err, sizeof(sdr.last_err), "Failed to load %s; %lu", sdr.dll_name, GetLastError());
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
  CALL_FUNC (sdrplay_api_DebugEnable, NULL, 1);
  CALL_FUNC (sdrplay_api_ApiVersion, &sdr.version);
  if (sdr.last_rc != sdrplay_api_Success)
     goto failed;

  LOG_STDERR ("sdrplay_api_ApiVersion(): '%.2f', build version: '%.2f'.\n", sdr.version, SDRPLAY_API_VERSION);
  if (sdr.version != SDRPLAY_API_VERSION || sdr.version < 3.06F)
     goto failed;

  sdr.device = sdrplay_select (name);
  if (!sdr.device)
     goto failed;

  CALL_FUNC (sdrplay_api_GetDeviceParams, sdr.device->dev, &sdr.deviceParams);
  if (sdr.last_rc != sdrplay_api_Success || !sdr.deviceParams)
     goto failed;

  *device = sdr.device;
  return (sdrplay_api_Success);

failed:
  LOG_STDERR ("%s.\n", sdr.last_err);
  sdrplay_exit (NULL);
  return (sdrplay_api_Fail);  /* A better error-code? */
}

/**
 *
 */
static int sdrplay_release (sdrplay_dev device)
{
  if (device != sdr.device || !sdr.device) /* support only 1 device */
  {
    strncpy (sdr.last_err, "No device", sizeof(sdr.last_err));
    sdr.last_rc = sdrplay_api_NotInitialised;
    return (sdr.last_rc);
  }

  CALL_FUNC (sdrplay_api_LockDeviceApi);
  CALL_FUNC (sdrplay_api_Uninit, sdr.device->dev);
  CALL_FUNC (sdrplay_api_ReleaseDevice, sdr.device);

  sdr.device = NULL;
  return (sdr.last_rc);
}

/**
 *
 */
int sdrplay_exit (sdrplay_dev device)
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

  if (sdr.API_locked)
     CALL_FUNC (sdrplay_api_UnlockDeviceApi);

  CALL_FUNC (sdrplay_api_Close);
  FreeLibrary (sdr.dll_hnd);

  sdr.API_locked = false;
  sdr.dll_hnd = NULL;
  sdr.device = NULL;
  return (sdr.last_rc);
}

#endif /* USE_SDRPLAY  */

