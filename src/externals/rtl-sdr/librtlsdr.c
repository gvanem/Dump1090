/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012-2014 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copied from the original librtlsdr.c at:
 *   https://github.com/old-dab/rtlsdr/blob/master/src/librtlsdr.c
 *
 * and:
 *  1. Removed all non-WIN32 code.
 *  2. Faking some Pthread calls using Win-SDK function.
 *  3. Changed the code-style using Astyle.
 *
 * Hence nothing here depends on the libusb library.
 */
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <process.h>
#include <malloc.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "rtl-sdr.h"
#include "tuner_e4k.h"
#include "tuner_fc001x.h"
#include "tuner_fc2580.h"
#include "tuner_r82xx.h"
#include "version.h"
#include "trace.h"

#define WINUSB_REQUEST_TYPE_VENDOR (0x02 << 5)
#define WINUSB_ENDPOINT_IN          0x80
#define WINUSB_ENDPOINT_OUT         0x00

#define CTRL_IN     (WINUSB_REQUEST_TYPE_VENDOR | WINUSB_ENDPOINT_IN)
#define CTRL_OUT    (WINUSB_REQUEST_TYPE_VENDOR | WINUSB_ENDPOINT_OUT)

/* two raised to the power of n
 */
#define TWO_POW(n)  ((double)(1ULL<<(n)))
#define EP_RX       0x81

#define rtlsdr_read_array(dev, index, addr, array, len) \
        usb_control_transfer (dev, CTRL_IN, addr, index, array, len, __LINE__)

#define rtlsdr_write_array(dev, index, addr, array, len) \
        usb_control_transfer (dev, CTRL_OUT, addr, index | 0x10, array, len, __LINE__)

typedef struct rtlsdr_tuner_iface {
        /* tuner interface */
        int (*init) (void *);
        int (*exit) (void *);

        int (*set_freq) (void *, uint32_t freq /* Hz */);

        int (*set_bw) (void     *dev,
                       int       bw          /* Hz */,
                       uint32_t *applied_bw  /* configured bw in Hz */,
                       int      apply        /* 1 == configure it!, 0 == deliver applied_bw */);

        int (*set_gain_index) (void *, unsigned int index);
        int (*set_if_gain) (void *, int stage, int gain /* tenth dB */);
        int (*set_gain_mode) (void *, int manual);
        int (*set_i2c_register) (void *,
                                 unsigned i2c_register,
                                 unsigned data /* byte */,
                                 unsigned mask /* byte */);

        int (*get_i2c_register) (void *, uint8_t *data, int *len, int *strength);
        int (*set_sideband) (void *, int sideband);
        const int * (*get_gains) (int *len);
      } rtlsdr_tuner_iface_t;

enum rtlsdr_async_status {
     RTLSDR_INACTIVE = 0,
     RTLSDR_CANCELING,
     RTLSDR_RUNNING
   };

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define FIR_LEN 16

/*
 * FIR coefficients.
 *
 * The filter is running at XTal frequency. It is symmetric filter with 32
 * coefficients. Only first 16 coefficients are specified, the other 16
 * use the same values but in reversed order. The first coefficient in
 * the array is the outer one, the last is the inner one.
 * First 8 coefficients are 8 bit signed integers, the next 8 coefficients
 * are 12 bit signed integers. All coefficients have the same weight.
 *
 * Default FIR coefficients used for DAB/FM by the Windows driver,
 * the DVB driver uses different ones
 */
static const int fir_default [][FIR_LEN] = {
  /* 1.2 MHz */
  {
    -54, -36, -41, -40, -32, -14,  14, 53,  /* 8 bit signed */
    101, 156, 215, 273, 327, 372, 404, 421  /* 12 bit signed */
  },
  /* 770 kHz */
  {
    -44, -30, -12,  10,  35,  62,  91, 121,
    151, 181, 208, 232, 252, 268, 279, 285
  },
};

static const int fir_bw [] = { 2400, 1500, 300 };

static int cal_imr = 0;

static bool got_device_usb_product [10];

enum softagc_mode {
  SOFTAGC_OFF = 0,  /* off */
  SOFTAGC_ON        /* operate full time - attenuate and gain */
};

struct softagc_state {
  uintptr_t         command_thread;
  volatile int      exit_command_thread;
  volatile int      command_newGain;
  volatile int      command_changeGain;
  enum softagc_mode softAgcMode;
};

struct rtlsdr_dev {
  WINUSB_INTERFACE_HANDLE  usbHandle;
  HANDLE                   deviceHandle;
  enum rtlsdr_async_status async_status;
  int                      async_cancel;
  uint32_t                 index;

  /* rtl demod context */
  uint32_t              rate;     /* Hz */
  uint32_t              rtl_xtal; /* Hz */
  int                   fir;
  int                   direct_sampling;

  /* tuner context */
  enum rtlsdr_tuner     tuner_type;
  rtlsdr_tuner_iface_t *tuner;
  uint32_t              tun_xtal;   /* Hz */
  uint32_t              freq;       /* Hz */
  uint32_t              bw;
  uint32_t              offs_freq;  /* Hz */
  int                   corr;       /* ppb */
  int                   gain;       /* tenth dB */

  unsigned int          gain_index;
  unsigned int          gain_count;
  int                   gain_mode;
  int                   gains [24];

  enum rtlsdr_ds_mode   direct_sampling_mode;
  uint32_t              direct_sampling_threshold; /* Hz */
  struct e4k_state      e4k_s;
  struct r82xx_config   r82xx_c;
  struct r82xx_priv     r82xx_p;
  enum rtlsdr_demod     slave_demod;

  struct softagc_state  softagc;

  /* Concurrent lock for the periodic reading of I2C registers
   */
  CRITICAL_SECTION  cs_mutex;

  /* status */
  int  rc_active;
  int  opening;
  int  verbose;
  int  agc_mode;
  char manufact [256];
  char product [256];

  /* transfer buffers */
  uint8_t    **xfer_buf;
  OVERLAPPED **overlapped;
  uint32_t     num_xfer_buf;
};

static int rtlsdr_update_ds (rtlsdr_dev_t *dev, uint32_t freq);
static int rtlsdr_set_spectrum_inversion (rtlsdr_dev_t *dev, int sideband);
static void softagc_init (rtlsdr_dev_t *dev);
static void softagc_close (rtlsdr_dev_t *dev);
static void softagc (rtlsdr_dev_t *dev, uint8_t *buf, int len);

static const char *async_status_name (enum rtlsdr_async_status status)
{
  return (status == RTLSDR_INACTIVE  ? "RTLSDR_INACTIVE"  :
          status == RTLSDR_CANCELING ? "RTLSDR_CANCELING" :
          status == RTLSDR_RUNNING   ? "RTLSDR_RUNNING"   : "?");
}

/* generic tuner interface functions, shall be moved to the tuner implementations
 */
int e4000_init (void *dev)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;

  devt->e4k_s.i2c_addr = E4K_I2C_ADDR;
  rtlsdr_get_xtal_freq (devt, NULL, &devt->e4k_s.vco.fosc);
  devt->e4k_s.rtl_dev = dev;
  return e4k_init (&devt->e4k_s);
}

int e4000_exit (void *dev)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_standby (&devt->e4k_s, 1);
}

int e4000_set_freq (void *dev, uint32_t freq)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_tune_freq (&devt->e4k_s, freq);
}

int e4000_set_bw (void *dev, int bw, uint32_t *applied_bw, int apply)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;

  if (!apply)
     return (0);
  return e4k_set_bandwidth (&devt->e4k_s, bw, applied_bw, apply);
}

int e4000_set_gain_index (void *dev, unsigned int index)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_set_gain_index (&devt->e4k_s, index);
}

int e4000_set_if_gain (void *dev, int stage, int gain)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_if_gain_set (&devt->e4k_s, (uint8_t) stage, (int8_t) (gain / 10));
}

int e4000_set_gain_mode (void *dev, int manual)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_enable_manual_gain (&devt->e4k_s, manual);
}

int e4000_set_i2c_register (void *dev, unsigned i2c_register, unsigned data, unsigned mask)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_set_i2c_register (&devt->e4k_s, i2c_register, data, mask);
}

int e4000_get_i2c_register (void *dev, uint8_t *data, int *len, int *strength)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return e4k_get_i2c_register (&devt->e4k_s, data, len, strength);
}

int r820t_init (void *dev)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  devt->r82xx_p.rtl_dev = dev;

  if (devt->tuner_type == RTLSDR_TUNER_R828D)
  {
    devt->r82xx_c.i2c_addr = R828D_I2C_ADDR;
    devt->r82xx_c.rafael_chip = CHIP_R828D;
  }
  else
  {
    devt->r82xx_c.i2c_addr = R820T_I2C_ADDR;
    devt->r82xx_c.rafael_chip = CHIP_R820T;
  }

  rtlsdr_get_xtal_freq (devt, NULL, &devt->r82xx_c.xtal);
  devt->r82xx_c.use_predetect = 0;
  devt->r82xx_c.cal_imr = cal_imr;
  devt->r82xx_p.cfg = &devt->r82xx_c;
  return r82xx_init (&devt->r82xx_p);
}

int r820t_exit (void *dev)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return r82xx_standby (&devt->r82xx_p);
}

int r820t_set_freq (void *dev, uint32_t freq)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return r82xx_set_freq (&devt->r82xx_p, freq);
}

int r820t_set_bw (void *dev, int bw, uint32_t *applied_bw, int apply)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  int           r = r82xx_set_bandwidth (&devt->r82xx_p, bw, applied_bw, apply);

  if (!apply)
     return (0);

  if (r < 0)
     return (r);

  r = rtlsdr_set_if_freq (devt, r);
  if (r)
     return (r);
  return rtlsdr_set_center_freq (devt, devt->freq);
}

int r820t_set_gain_index (void *dev, unsigned int index)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return r82xx_set_gain_index (&devt->r82xx_p, index);
}

int r820t_set_gain_mode (void *dev, int manual)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return r82xx_set_gain_mode (&devt->r82xx_p, manual);
}

int r820t_set_i2c_register (void *dev, unsigned i2c_register, unsigned data, unsigned mask)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return r82xx_set_i2c_register (&devt->r82xx_p, i2c_register, data, mask);
}

int r820t_get_i2c_register (void *dev, uint8_t *data, int *len, int *strength)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  return r82xx_get_i2c_register (&devt->r82xx_p, data, len, strength);
}

int r820t_set_sideband (void *dev, int sideband)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;
  int           r = r82xx_set_sideband (&devt->r82xx_p, sideband);

  if (r < 0)
     return (r);

  r = rtlsdr_set_spectrum_inversion (devt, sideband);
  if (r)
     return (r);

  return rtlsdr_set_center_freq (devt, devt->freq);
}

/* definition order must match enum rtlsdr_tuner
 */
static rtlsdr_tuner_iface_t tuners [] = {
  {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL /* dummy for unknown tuners */
  },
  {
    e4000_init, e4000_exit,
    e4000_set_freq, e4000_set_bw, e4000_set_gain_index, e4000_set_if_gain,
    e4000_set_gain_mode, e4000_set_i2c_register,
    e4000_get_i2c_register, NULL, e4k_get_gains
  },
  {
    fc0012_init, fc0012_exit,
    fc0012_set_freq, fc001x_set_bw, fc0012_set_gain_index, NULL,
    fc001x_set_gain_mode, fc001x_set_i2c_register,
    fc0012_get_i2c_register, NULL, fc001x_get_gains
  },
  {
    fc0013_init, fc0013_exit,
    fc0013_set_freq, fc001x_set_bw, fc0013_set_gain_index, NULL,
    fc001x_set_gain_mode, fc001x_set_i2c_register,
    fc0013_get_i2c_register, NULL, fc001x_get_gains
  },
  {
    fc2580_init, fc2580_exit,
    fc2580_set_freq, fc2580_set_bw, fc2580_set_gain_index, NULL,
    fc2580_set_gain_mode, fc2580_set_i2c_register,
    fc2580_get_i2c_register, NULL, fc2580_get_gains
  },
  {
    r820t_init, r820t_exit,
    r820t_set_freq, r820t_set_bw, r820t_set_gain_index, NULL,
    r820t_set_gain_mode, r820t_set_i2c_register,
    r820t_get_i2c_register, r820t_set_sideband, r82xx_get_gains
  },
  {
    r820t_init, r820t_exit,
    r820t_set_freq, r820t_set_bw, r820t_set_gain_index, NULL,
    r820t_set_gain_mode, r820t_set_i2c_register,
    r820t_get_i2c_register, r820t_set_sideband, r82xx_get_gains
  },
};

typedef struct rtlsdr_dongle {
        const uint16_t vid;
        const uint16_t pid;
        const char    *name;
      } rtlsdr_dongle_t;

typedef struct found_device {
        uint16_t vid;
        uint16_t pid;
        char     serial [80];
        char     mfg [80];
        char     DevicePath [256];
      } found_device;

/*
 * Please add your device here and send a patch to osmocom-sdr@lists.osmocom.org
 */
static const rtlsdr_dongle_t known_devices [] =
{
  { 0x0bda, 0x2832, "Generic RTL2832U" },
  { 0x0bda, 0x2838, "Generic RTL2832U OEM" },
  { 0x0413, 0x6680, "DigitalNow Quad DVB-T PCI-E card" },
  { 0x0413, 0x6f0f, "Leadtek WinFast DTV Dongle mini D" },
  { 0x0458, 0x707f, "Genius TVGo DVB-T03 USB dongle (Ver. B)" },
  { 0x0ccd, 0x00a9, "Terratec Cinergy T Stick Black (rev 1)" },
  { 0x0ccd, 0x00b3, "Terratec NOXON DAB/DAB+ USB dongle (rev 1)" },
  { 0x0ccd, 0x00b4, "Terratec Deutschlandradio DAB Stick" },
  { 0x0ccd, 0x00b5, "Terratec NOXON DAB Stick - Radio Energy" },
  { 0x0ccd, 0x00b7, "Terratec Media Broadcast DAB Stick" },
  { 0x0ccd, 0x00b8, "Terratec BR DAB Stick" },
  { 0x0ccd, 0x00b9, "Terratec WDR DAB Stick" },
  { 0x0ccd, 0x00c0, "Terratec MuellerVerlag DAB Stick" },
  { 0x0ccd, 0x00c6, "Terratec Fraunhofer DAB Stick" },
  { 0x0ccd, 0x00d3, "Terratec Cinergy T Stick RC (Rev.3)" },
  { 0x0ccd, 0x00d7, "Terratec T Stick PLUS" },
  { 0x0ccd, 0x00e0, "Terratec NOXON DAB/DAB+ USB dongle (rev 2)" },
  { 0x1209, 0x2832, "Generic RTL2832U" },
  { 0x1554, 0x5020, "PixelView PV-DT235U(RN)" },
  { 0x15f4, 0x0131, "Astrometa DVB-T/DVB-T2" },
  { 0x15f4, 0x0133, "HanfTek DAB+FM+DVB-T" },
  { 0x185b, 0x0620, "Compro Videomate U620F"},
  { 0x185b, 0x0650, "Compro Videomate U650F"},
  { 0x185b, 0x0680, "Compro Videomate U680F"},
  { 0x1b80, 0xd393, "GIGABYTE GT-U7300" },
  { 0x1b80, 0xd394, "DIKOM USB-DVBT HD" },
  { 0x1b80, 0xd395, "Peak 102569AGPK" },
  { 0x1b80, 0xd397, "KWorld KW-UB450-T USB DVB-T Pico TV" },
  { 0x1b80, 0xd398, "Zaapa ZT-MINDVBZP" },
  { 0x1b80, 0xd39d, "SVEON STV20 DVB-T USB & FM" },
  { 0x1b80, 0xd3a4, "Twintech UT-40" },
  { 0x1b80, 0xd3a8, "ASUS U3100MINI_PLUS_V2" },
  { 0x1b80, 0xd3af, "SVEON STV27 DVB-T USB & FM" },
  { 0x1b80, 0xd3b0, "SVEON STV21 DVB-T USB & FM" },
  { 0x1d19, 0x1101, "Dexatek DK DVB-T Dongle (Logilink VG0002A)" },
  { 0x1d19, 0x1102, "Dexatek DK DVB-T Dongle (MSI DigiVox mini II V3.0)" },
  { 0x1d19, 0x1103, "Dexatek Technology Ltd. DK 5217 DVB-T Dongle" },
  { 0x1d19, 0x1104, "MSI DigiVox Micro HD" },
  { 0x1f4d, 0xa803, "Sweex DVB-T USB" },
  { 0x1f4d, 0xb803, "GTek T803" },
  { 0x1f4d, 0xc803, "Lifeview LV5TDeluxe" },
  { 0x1f4d, 0xd286, "MyGica TD312" },
  { 0x1f4d, 0xd803, "PROlectrix DV107669" },
};

#define DEFAULT_BUF_NUMBER  15
#define DEFAULT_BUF_LENGTH  (64 * 512)

/* buf_len:
 * must be multiple of 512 - else it will be overwritten
 * in rtlsdr_read_async() in librtlsdr.c with DEFAULT_BUF_LENGTH (= 16*32 *512 = 512 *512)
 *
 * -> 512*512 -> 1048 ms @ 250 kS  or  81.92 ms @ 3.2 MS (internal default)
 * ->  32*512 ->   65 ms @ 250 kS  or   5.12 ms @ 3.2 MS (new default)
 */

#define DEF_RTL_XTAL_FREQ 28800000
#define MIN_RTL_XTAL_FREQ (DEF_RTL_XTAL_FREQ - 1000)
#define MAX_RTL_XTAL_FREQ (DEF_RTL_XTAL_FREQ + 1000)

#define EEPROM_ADDR         0xa0
#define RTL2832_DEMOD_ADDR  0x20
#define DUMMY_PAGE          0x0a
#define DUMMY_ADDR          0x01


/*
 * memory map
 *
 * 0x0000 DEMOD : demodulator
 * 0x2000 USB   : SIE, USB endpoint, debug, DMA
 * 0x3000 SYS   : system
 * 0xfc00 RC    : remote controller (not RTL2831U)
 */

enum usb_reg {
  /* SIE Control Registers */
  USB_SYSCTL       = 0x2000,   /* USB system control */
  USB_IRQSTAT      = 0x2008,   /* SIE interrupt status */
  USB_IRQEN        = 0x200C,   /* SIE interrupt enable */
  USB_CTRL         = 0x2010,   /* USB control */
  USB_STAT         = 0x2014,   /* USB status */
  USB_DEVADDR      = 0x2018,   /* USB device address */
  USB_TEST         = 0x201C,   /* USB test mode */
  USB_FRAME_NUMBER = 0x2020,   /* frame number */
  USB_FIFO_ADDR    = 0x2028,   /* address of SIE FIFO RAM */
  USB_FIFO_CMD     = 0x202A,   /* SIE FIFO RAM access command */
  USB_FIFO_DATA    = 0x2030,   /* SIE FIFO RAM data */

  /* Endpoint Registers */
  EP0_SETUPA        = 0x20F8,  /* EP 0 setup packet lower byte */
  EP0_SETUPB        = 0x20FC,  /* EP 0 setup packet higher byte */
  USB_EP0_CFG       = 0x2104,  /* EP 0 configure */
  USB_EP0_CTL       = 0x2108,  /* EP 0 control */
  USB_EP0_STAT      = 0x210C,  /* EP 0 status */
  USB_EP0_IRQSTAT   = 0x2110,  /* EP 0 interrupt status */
  USB_EP0_IRQEN     = 0x2114,  /* EP 0 interrupt enable */
  USB_EP0_MAXPKT    = 0x2118,  /* EP 0 max packet size */
  USB_EP0_BC        = 0x2120,  /* EP 0 FIFO byte counter */
  USB_EPA_CFG       = 0x2144,  /* EP A configure */
  USB_EPA_CTL       = 0x2148,  /* EP A control */
  USB_EPA_STAT      = 0x214C,  /* EP A status */
  USB_EPA_IRQSTAT   = 0x2150,  /* EP A interrupt status */
  USB_EPA_IRQEN     = 0x2154,  /* EP A interrupt enable */
  USB_EPA_MAXPKT    = 0x2158,  /* EP A max packet size */
  USB_EPA_FIFO_CFG  = 0x2160,  /* EP A FIFO configure */

  /* Debug Registers */
  USB_PHYTSTDIS     = 0x2F04,  /* PHY test disable */
  USB_TOUT_VAL      = 0x2F08,  /* USB time-out time */
  USB_VDRCTRL       = 0x2F10,  /* UTMI vendor signal control */
  USB_VSTAIN        = 0x2F14,  /* UTMI vendor signal status in */
  USB_VLOADM        = 0x2F18,  /* UTMI load vendor signal status in */
  USB_VSTAOUT       = 0x2F1C,  /* UTMI vendor signal status out */
  USB_UTMI_TST      = 0x2F80,  /* UTMI test */
  USB_UTMI_STATUS   = 0x2F84,  /* UTMI status */
  USB_TSTCTL        = 0x2F88,  /* test control */
  USB_TSTCTL2       = 0x2F8C,  /* test control 2 */
  USB_PID_FORCE     = 0x2F90,  /* force PID */
  USB_PKTERR_CNT    = 0x2F94,  /* packet error counter */
  USB_RXERR_CNT     = 0x2F98,  /* RX error counter */
  USB_MEM_BIST      = 0x2F9C,  /* MEM BIST test */
  USB_SLBBIST       = 0x2FA0,  /* self-loop-back BIST */
  USB_CNTTEST       = 0x2FA4,  /* counter test */
  USB_PHYTST        = 0x2FC0,  /* USB PHY test */
  USB_DBGIDX        = 0x2FF0,  /* select individual block debug signal */
  USB_DBGMUX        = 0x2FF4   /* debug signal module mux */
};

enum sys_reg {
  /* demod control registers */
  DEMOD_CTL  = 0x3000,  /* control register for DVB-T demodulator */
  GPO        = 0x3001,  /* output value of GPIO */
  GPI        = 0x3002,  /* input value of GPIO */
  GPOE       = 0x3003,  /* output enable of GPIO */
  GPD        = 0x3004,  /* direction control for GPIO */
  SYSINTE    = 0x3005,  /* system interrupt enable */
  SYSINTS    = 0x3006,  /* system interrupt status */
  GP_CFG0    = 0x3007,  /* PAD configuration for GPIO0-GPIO3 */
  GP_CFG1    = 0x3008,  /* PAD configuration for GPIO4 */
  SYSINTE_1  = 0x3009,
  SYSINTS_1  = 0x300A,
  DEMOD_CTL1 = 0x300B,
  IR_SUSPEND = 0x300C,

  /* I2C master registers */
  I2CCR      = 0x3040,  /* I2C clock */
  I2CMCR     = 0x3044,  /* I2C master control */
  I2CMSTR    = 0x3048,  /* I2C master SCL timing */
  I2CMSR     = 0x304C,  /* I2C master status */
  I2CMFR     = 0x3050   /* I2C master FIFO */
};

enum ir_reg {
  /* IR registers */
  IR_RX_BUF         = 0xFC00,
  IR_RX_IE          = 0xFD00,
  IR_RX_IF          = 0xFD01,
  IR_RX_CTRL        = 0xFD02,
  IR_RX_CFG         = 0xFD03,
  IR_MAX_DURATION0  = 0xFD04,
  IR_MAX_DURATION1  = 0xFD05,
  IR_IDLE_LEN0      = 0xFD06,
  IR_IDLE_LEN1      = 0xFD07,
  IR_GLITCH_LEN     = 0xFD08,
  IR_RX_BUF_CTRL    = 0xFD09,
  IR_RX_BUF_DATA    = 0xFD0A,
  IR_RX_BC          = 0xFD0B,
  IR_RX_CLK         = 0xFD0C,
  IR_RX_C_COUNT_L   = 0xFD0D,
  IR_RX_C_COUNT_H   = 0xFD0E,
  IR_SUSPEND_CTRL   = 0xFD10,
  IR_ERR_TOL_CTRL   = 0xFD11,
  IR_UNIT_LEN       = 0xFD12,
  IR_ERR_TOL_LEN    = 0xFD13,
  IR_MAX_H_TOL_LEN  = 0xFD14,
  IR_MAX_L_TOL_LEN  = 0xFD15,
  IR_MASK_CTRL      = 0xFD16,
  IR_MASK_DATA      = 0xFD17,
  IR_RES_MASK_ADDR  = 0xFD18,
  IR_RES_MASK_T_LEN = 0xFD19
};

enum blocks {
  DEMODB = 0x0000,
  USBB   = 0x0100,
  SYSB   = 0x0200,
  IRB    = 0x0201,
  TUNB   = 0x0300,
  ROMB   = 0x0400,
  IICB   = 0x0600
};

static uint32_t last_error;

uint32_t rtlsdr_last_error (void)
{
  return (last_error);
}

/*
 * Some demodulator registers, not described in datasheet:
 *
 * Page Reg Bitmap  Description
 * ---------------------------------------------------------------
 * 0    0x09        Gain before the ADC
 *           [1:0]  I, in steps of 2.5 dB
 *           [3:2]  Q, in steps of 2.5 dB
 *           [6:4]  I and Q, in steps of 0.7 dB
 * ----------------------------------------------------------------
 * 0    0x17        Gain, when DAGC is on. 0x01=min, 0xff=max
 * ---------------------------------------------------------------
 * 0    0x18        ADC gain, when DAGC is off. 0x00=min, 0x7f=max
 * ----------------------------------------------------------------
 * 0    0x19 [0]    SDR mode	0: off, 1: on
 * 0    0x19 [1]    Test mode	0: off, 1: on
 * 0    0x19 [2]    3rd FIR switch 0: on, 1: off
 * 0    0x19 [4:3]  DAGC speed 00 = slowest, 11 = fastest
 * 0    0x19 [5]    DAGC 0: on, 1: off
 * ----------------------------------------------------------------
 * 0    0x1a        Coefficient 6 of 3rd FIR
 * 0    0x1b        Coefficient 5 of 3rd FIR
 * 0    0x1c        Coefficient 4 of 3rd FIR
 * 0    0x1d        Coefficient 3 of 3rd FIR
 * 0    0x1e        Coefficient 2 of 3rd FIR
 * 0    0x1f        Coefficient 1 of 3rd FIR
 * ----------------------------------------------------------------
 * 1    0x01 [2]    Demodulator software reset 0: off, 1: on
 * ----------------------------------------------------------------
 * 3    0x01        RSSI
 * ---------------------------------------------------------------
 */

static const rtlsdr_dongle_t *find_known_device (uint16_t vid, uint16_t pid, unsigned called_from)
{
  const rtlsdr_dongle_t *device = NULL;
  size_t i;

  for (i = 0; i < ARRAY_SIZE(known_devices); i++)
  {
    if (known_devices[i].vid == vid && known_devices[i].pid == pid)
    {
      RTL_TRACE (1, "Found VID: 0x%04X PID: 0x%04X -> \"%s\" (line: %u)\n",
                 vid, pid, known_devices[i].name, called_from);
      device = (known_devices + i);
      break;
    }
  }
  return (device);
}

static int List_Devices (int index, found_device *found)
{
  SP_DEVINFO_DATA DeviceInfoData;
  char            DeviceID [256];
  char            DeviceID2 [256];
  char            devInterfaceGuidArray [256];
  char            Mfg [80];
  char            Service [32]; /* Driver service name. */
  HKEY            hkeyDevInfo;
  DWORD           length;
  int             vid, pid;
  int             DeviceIndex = 0;
  int             count = 0;
  HDEVINFO        DeviceInfoSet;

  if (found)
     memset (found, '\0', sizeof(*found));

  DeviceInfoSet = SetupDiGetClassDevsA (NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (DeviceInfoSet == INVALID_HANDLE_VALUE)
  {
    last_error = GetLastError();
    RTL_TRACE (1, "SetupDiGetClassDevs() failed: %s\n", trace_strerror(last_error));
    return (-1);
  }

  memset (&DeviceInfoData, '\0', sizeof(SP_DEVINFO_DATA));
  DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

  while (SetupDiEnumDeviceInfo(DeviceInfoSet, DeviceIndex, &DeviceInfoData))
  {
    /* Get the Device Instance ID
     */
    if (!SetupDiGetDeviceInstanceIdA(DeviceInfoSet, &DeviceInfoData, DeviceID, sizeof(DeviceID), NULL))
    {
      last_error = GetLastError();
      RTL_TRACE_WINUSB ("SetupDiGetDeviceInstanceId", last_error);
      DeviceIndex++;
      continue;
    }

    RTL_TRACE (2, "%d: Found device: '%s'\n", DeviceIndex, DeviceID);

    DeviceIndex++;

    /* We are only interested in *usb device* instances; not root hubs (or anything else)
     */
    if (_strnicmp(DeviceID, "USB\\VID_", 8))
       continue;

    if (!_strnicmp(DeviceID + 22, "MI_01", 5))
       continue;

    sscanf (DeviceID + 8, "%04X", &vid);
    sscanf (DeviceID + 17, "%04X", &pid);

    if (!find_known_device(vid, pid, __LINE__))
       continue;

    if (!_strnicmp(DeviceID + 22, "MI_", 3))
    {
      /* This is a composite device.
       * The 'SerialNumber' will come from the parent device.
       */
      DWORD hParentInst;

      if (CM_Get_Parent(&hParentInst, DeviceInfoData.DevInst, 0) == ERROR_SUCCESS)
         CM_Get_Device_IDA (hParentInst, DeviceID2, sizeof(DeviceID2) - 1, 0);
    }

    /* Get SPDRP_SERVICE */
    if (!SetupDiGetDeviceRegistryPropertyA(DeviceInfoSet, &DeviceInfoData, SPDRP_SERVICE, NULL, (BYTE*)Service, sizeof(Service) - 1, NULL))
    {
      last_error = GetLastError();
      RTL_TRACE_WINUSB ("SetupDiGetDeviceRegistryProperty", last_error);
      continue;
    }

    if (_strnicmp(Service, "WinUSB", 6))
       continue;

    if (!SetupDiGetDeviceRegistryPropertyA(DeviceInfoSet, &DeviceInfoData, SPDRP_MFG, NULL, (BYTE*)Mfg, sizeof(Mfg) - 1, NULL))
    {
      last_error = GetLastError();
      RTL_TRACE_WINUSB ("SetupDiGetDeviceRegistryProperty", last_error);
      continue;
    }
    if (found)
       strncpy (found->mfg, Mfg, sizeof(found->mfg));

    hkeyDevInfo = SetupDiOpenDevRegKey (DeviceInfoSet, &DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    if (hkeyDevInfo == INVALID_HANDLE_VALUE)
    {
      last_error = GetLastError();
      RTL_TRACE_WINUSB ("SetupDiOpenDevRegKey", last_error);
      continue;
    }

    length = sizeof(devInterfaceGuidArray);
    memset (devInterfaceGuidArray, '\0', sizeof(devInterfaceGuidArray));

    if (RegQueryValueExA(hkeyDevInfo, "DeviceInterfaceGUIDs", NULL, NULL, (BYTE*)devInterfaceGuidArray, &length) != ERROR_SUCCESS)
    {
      last_error = GetLastError();
      RTL_TRACE_WINUSB ("RegQueryValueExA", last_error);
      RegCloseKey (hkeyDevInfo);
      continue;
    }

    RegCloseKey (hkeyDevInfo);

    RTL_TRACE (2, "%d: %04X:%04X %s %s\n", count, vid, pid, Mfg, &devInterfaceGuidArray[0]);

    if (found && index == count)  /* not when 'index == -1' */
    {
      char  DevicePath [256];
      char *backslash;

      found->vid = vid;
      found->pid = pid;

      strcpy (DevicePath, "\\\\?\\");
      strcat (DevicePath, DeviceID);
      strcat (DevicePath, "#");
      strcat (DevicePath, &devInterfaceGuidArray[0]);
      DevicePath[7] = '#';
      backslash = strchr (DevicePath + 8, '\\');

      if (backslash)
         *backslash = '#';

      strcpy (found->DevicePath, DevicePath);
      RTL_TRACE (2, "count: %d, found->mfg = %s\n"
                    "                      DevicePath = %s\n", count, found->mfg, DevicePath);
      break;
    }
    count++;
  }

  if (DeviceInfoSet)
     SetupDiDestroyDeviceInfoList (DeviceInfoSet);

  return (count);
}

static void Close_Device (rtlsdr_dev_t *dev)
{
  RTL_TRACE (2, "Calling 'WinUsb_Free (0x%p)'\n", dev->usbHandle);

  if (dev->usbHandle && dev->usbHandle != INVALID_HANDLE_VALUE)
     WinUsb_Free (dev->usbHandle);

  if (dev->deviceHandle && dev->deviceHandle != INVALID_HANDLE_VALUE)
     CloseHandle (dev->deviceHandle);

  dev->usbHandle    = INVALID_HANDLE_VALUE;
  dev->deviceHandle = INVALID_HANDLE_VALUE;
}

static BOOL Open_Device (rtlsdr_dev_t *dev, const char *DevicePath, int *err)
{
  BOOL rc = TRUE;

  RTL_TRACE (2, "Calling 'CreateFileA (\"%s\")'\n", DevicePath);

  dev->usbHandle = INVALID_HANDLE_VALUE;

  dev->deviceHandle = CreateFileA (DevicePath, GENERIC_WRITE | GENERIC_READ,
                                   FILE_SHARE_WRITE | FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                   NULL);

  if (dev->deviceHandle == INVALID_HANDLE_VALUE)
  {
    last_error = GetLastError();
    *err = -1 * last_error;
    RTL_TRACE (1, "CreateFileA(\"%s\") failed: %s\n", DevicePath, trace_strerror(last_error));
    rc = FALSE;
  }
  else if (!WinUsb_Initialize(dev->deviceHandle, &dev->usbHandle))
  {
    last_error = GetLastError();
    *err = -1 * last_error;
    RTL_TRACE_WINUSB ("WinUsb_Initialize", last_error);
    Close_Device (dev);
    rc = FALSE;
  }

  RTL_TRACE (1, "dev->deviceHandle: 0x%p, dev->usbHandle: 0x%p\n", dev->deviceHandle, dev->usbHandle);
  *err = 0;
  return (rc);
}

static int usb_control_transfer (rtlsdr_dev_t *dev,      /* the active device */
                                 uint8_t       type,     /* CTRL_IN or CTRL_OUT */
                                 uint16_t      wValue,   /* register value */
                                 uint16_t      wIndex,   /* register block-index */
                                 uint8_t      *data,     /* read or write control-data */
                                 uint16_t      wLength,  /* length of control-data */
                                 unsigned      line)     /* called from line */
{
  WINUSB_SETUP_PACKET setupPacket;
  ULONG  written;
  BOOL   rc;

  if (!dev)
  {
    last_error = ERROR_INVALID_PARAMETER;
    RTL_TRACE (0, "FATAL: %s() called from %u with 'dev == NULL'!\n", __FUNCTION__, line);
    return (-1);
  }
  if (!dev->usbHandle)
  {
    last_error = ERROR_INVALID_PARAMETER;
    RTL_TRACE (0, "FATAL: %s() called from %u with 'dev->usbHandle == NULL'!\n", __FUNCTION__, line);
    return (-1);
  }

  /* Setup packets are always 8 bytes (64 bits)
   */
  *(__int64*) &setupPacket = 0;

  /* Fill the setup packet
   */
  setupPacket.RequestType = type;
  setupPacket.Value       = wValue;
  setupPacket.Index       = wIndex;
  setupPacket.Length      = wLength;

  RTL_TRACE (2, "%u: type: %s data: 0x%p, wLength: %u\n",
             line,
             type == CTRL_IN  ? "CTRL_IN, " :
             type == CTRL_OUT ? "CTRL_OUT," : "?,",
             data, wLength);

  written = 0;
  rc = WinUsb_ControlTransfer (dev->usbHandle, setupPacket, data, wLength, &written, NULL);
  if (!rc || written != wLength)
  {
    last_error = GetLastError();
    RTL_TRACE (1, "%u: WinUsb_ControlTransfer() wrote only %lu bytes: %s\n",
               line, written, trace_strerror(last_error));
    return (-1);
  }
  return (int)written;
}

static uint8_t rtlsdr_read_reg (rtlsdr_dev_t *dev, uint16_t index, uint16_t addr)
{
  uint8_t data;
  int r = rtlsdr_read_array (dev, index, addr, &data, 1);

  if (r != 1)
     RTL_TRACE (1, "%s failed with %d\n", __FUNCTION__, r);

  return data;
}

static int rtlsdr_write_reg (rtlsdr_dev_t *dev, uint16_t index, uint16_t addr, uint16_t val, uint8_t len)
{
  uint8_t data [2];
  int     r;

  assert (len == 1 || len == 2);

  if (len == 1)
     data[0] = val & 0xff;
  else
  {
    data[0] = val >> 8;
    data[1] = val & 0xff;
  }

  r = rtlsdr_write_array (dev, index, addr, data, len);
  if (r < 0)
     RTL_TRACE (1, "%s failed with %d\n", __FUNCTION__, r);

  return (r);
}

static int rtlsdr_write_reg_mask (rtlsdr_dev_t *dev, uint16_t index, uint16_t addr, uint8_t val, uint8_t mask)
{
  uint8_t tmp = rtlsdr_read_reg (dev, index, addr);

  val = (tmp & ~mask) | (val & mask);
  if (tmp == val)
     return (0);
  return rtlsdr_write_reg (dev, index, addr, (uint16_t)val, 1);
}

static uint8_t check_tuner (rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg)
{
  uint8_t data = 0;

  if (rtlsdr_read_array(dev, TUNB, reg << 8 | i2c_addr, &data, 1) != 1)
     return (0xFF);  /* signal a read-error since the address is unsupported for this tuner */
  return (data);
}

int rtlsdr_i2c_write_fn (void *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len)
{
  int wr_len;

  if (!dev)
     return (-1);

  wr_len = rtlsdr_write_array ((rtlsdr_dev_t*)dev, TUNB, reg << 8 | addr, buf, len);
  RTL_TRACE (2, "I2C-bus addr: 0x%02X, reg: 0x%02X, wr_len: %d\n", addr, reg, wr_len);
  return (wr_len);
}

int rtlsdr_i2c_read_fn (void *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len)
{
  int rd_len;

  if (!dev)
     return (-1);

  rd_len = rtlsdr_read_array ((rtlsdr_dev_t*)dev, TUNB, reg << 8 | addr, buf, len);
  RTL_TRACE (2, "I2C-bus addr: 0x%02X, reg: 0x%02X, rd_len: %d\n", addr, reg, rd_len);
  return (rd_len);
}

uint16_t rtlsdr_demod_read_reg (rtlsdr_dev_t *dev, uint16_t page, uint16_t addr, uint8_t len)
{
  uint8_t data [2];
  int     r = rtlsdr_read_array (dev, page, (addr << 8) | RTL2832_DEMOD_ADDR, data, len);

  if (r != len)
     RTL_TRACE (1, "%s failed with %d\n", __FUNCTION__, r);

  if (len == 1)
     return (data[0]);
  return (data[0] << 8) | data[1];
}

int rtlsdr_demod_write_reg (rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
  uint8_t data [2];
  int     r;

  assert (len == 1 || len == 2);

  addr = (addr << 8) | RTL2832_DEMOD_ADDR;
  if (len == 1)
     data[0] = val & 0xff;
  else
  {
    data[0] = val >> 8;
    data[1] = val & 0xff;
  }

  r = rtlsdr_write_array (dev, page, addr, data, len);
  if (r != len)
    RTL_TRACE (1, "%s failed with %d\n", __FUNCTION__, r);

  rtlsdr_demod_read_reg (dev, DUMMY_PAGE, DUMMY_ADDR, 1);
  return (r == len) ? 0 : -1;
}

static int rtlsdr_demod_write_reg_mask (rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint8_t val, uint8_t mask)
{
  uint8_t tmp = rtlsdr_demod_read_reg (dev, page, addr, 1);

  val = (tmp & ~mask) | (val & mask);
  if (tmp == val)
     return (0);
  return rtlsdr_demod_write_reg (dev, page, addr, (uint16_t) val, 1);
}

void rtlsdr_set_gpio_bit (rtlsdr_dev_t *dev, uint8_t gpio, int val)
{
  rtlsdr_write_reg_mask (dev, SYSB, GPO, val << gpio, 1 << gpio);
}

static void rtlsdr_set_gpio_output (rtlsdr_dev_t *dev, uint8_t gpio)
{
  gpio = 1 << gpio;
  rtlsdr_write_reg_mask (dev, SYSB, GPD, ~gpio, gpio);
  rtlsdr_write_reg_mask (dev, SYSB, GPOE, gpio, gpio);
}

static int rtlsdr_set_i2c_repeater (rtlsdr_dev_t *dev, int on)
{
  int r;

  if (on)
     EnterCriticalSection (&dev->cs_mutex);

  r = rtlsdr_demod_write_reg_mask (dev, 1, 0x01, on ? 0x08 : 0x00, 0x08);

  if (!on)
     LeaveCriticalSection (&dev->cs_mutex);

  return (r);
}

static const char *dump_fir_values (const uint8_t *values, size_t size)
{
  static char ret [4 * FIR_LEN + 10];
  char  *p, *q = ret;
  size_t i;

  for (i = 0; i < size && q < ret + sizeof(ret) - 2; i++)
  {
    _itoa (values[i], q, 10);
    p = strchr (q, '\0');
    *p++ = ',';
    q = p;
  }
  q[-1] = '\0';
  return (ret);
}

static const char FM_coe[][6] = {
  {  8, -7, 5,  3, -18, 80 }, // 800kHz
  { -1,  1, 6, 13,  22, 27 }, // 150kHz
};

static int Set_3rd_FIR (void *dev, int table)
{
  uint16_t addr = 0x1f;
  int      i, rst = 0;
  const char *fir_table = FM_coe[table];

  for (i = 0; i < 6; i++)
      rst |= rtlsdr_demod_write_reg(dev, 0, addr--, fir_table[i], 1);
  return rst;
}

static int rtlsdr_set_fir (rtlsdr_dev_t *dev, int table)
{
  const int *fir_table;
  uint8_t    fir [20];
  int        i, r = 0;

  if (dev->fir == table || table > 2) // no change
     return (0);

  /* 3rd FIR-Filter */
  if (rtlsdr_demod_write_reg_mask(dev, 0, 0x19, table ? 0x00 : 0x04, 0x04))
     return (-1);

  RTL_TRACE (1, "FIR Filter %d kHz\n", fir_bw[table]);
  dev->fir = table;
  if (table)
  {
    Set_3rd_FIR (dev, table-1);

    /* Bandwidth of 3rd FIR filter depends on output bitrate
     */
    RTL_TRACE (1, "FIR Filter %d kHz\n", fir_bw[table] * (dev->rate/1000)/2048);
  }
  else
    RTL_TRACE (1, "FIR Filter %d kHz\n", fir_bw[table]);

  if (table == 2)
     table = 1;

  fir_table = fir_default[table];

  /* format: int8_t[8] */
  for (i = 0; i < 8; i++)
  {
    const int val = fir_table [i];

    if (val < -128 || val > 127)
       goto fail;

    fir [i] = val;
  }

  /* format: int12_t[8] */
  for (i = 0; i < 8; i += 2)
  {
    const int val0 = fir_table [8 + i];
    const int val1 = fir_table [8 + i + 1];

    if (val0 < -2048 || val0 > 2047 || val1 < -2048 || val1 > 2047)
       goto fail;

    fir [8 + i * 3 / 2] = val0 >> 4;
    fir [8 + i * 3 / 2 + 1] = (val0 << 4) | ((val1 >> 8) & 0x0f);
    fir [8 + i * 3 / 2 + 2] = val1;
  }

  for (i = 0; i < (int)sizeof(fir); i++)
  {
    r = rtlsdr_demod_write_reg (dev, 1, 0x1c + i, fir[i], 1);
    if (r)
       goto fail;
  }

  RTL_TRACE (1, "FIR Filter %d kHz: FIR-coeff from 'fir_default[%d]':\n"
                "                   %s\n",
                fir_bw[table], table, dump_fir_values (fir, sizeof(fir)));
  return (0);

fail:
  RTL_TRACE (1, "FIR Filter %d kHz, r: %d, wrong FIR-coeff at 'fir_default[%d][%d]':\n",
                "                   %s\n",
                fir_bw[table], r, table, i, dump_fir_values (fir, i));
  return (-1);
}

int rtlsdr_get_agc_val (void *dev, int *slave_demod)
{
  rtlsdr_dev_t *devt = (rtlsdr_dev_t*) dev;

  *slave_demod = devt->slave_demod;
  return rtlsdr_demod_read_reg (dev, 3, 0x59, 2);
}

int16_t interpolate (int16_t freq, int size, const int16_t *freqs, const int16_t *gains)
{
  int16_t gain = 0;
  int     i;

  if (freq < freqs [0])
     freq = freqs [0];

  if (freq >= freqs [size-1])
     gain = gains [size-1];
  else
  {
    for (i = 0; i < size - 1; i++)
      if (freq < freqs [i+1])
      {
        gain = gains [i] + ((gains[i+1] - gains[i]) * (freq - freqs[i])) / (freqs[i+1] - freqs[i]);
        break;
      }
  }
  return (gain);
}

int rtlsdr_reset_demod (rtlsdr_dev_t *dev)
{
  /* reset demod (bit 2, soft_rst)
   */
  int r = rtlsdr_demod_write_reg_mask (dev, 1, 0x01, 0x04, 0x04);

  r |= rtlsdr_demod_write_reg_mask (dev, 1, 0x01, 0x00, 0x04);
  return (r);
}

static void rtlsdr_init_baseband (rtlsdr_dev_t *dev)
{
  unsigned int i;

  /* initialize USB */
  rtlsdr_write_reg (dev, USBB, USB_SYSCTL, 0x09, 1);
  rtlsdr_write_reg (dev, USBB, USB_EPA_MAXPKT, 0x0002, 2);
  rtlsdr_write_reg (dev, USBB, USB_EPA_CTL, 0x1002, 2);

  /* disable IR interrupts in order to avoid SDR sample loss */
  rtlsdr_write_reg (dev, IRB, IR_RX_IE, 0x00, 1);

  /* poweron demod */
  rtlsdr_write_reg (dev, SYSB, DEMOD_CTL1, 0x22, 1);
  rtlsdr_write_reg (dev, SYSB, DEMOD_CTL, 0xe8, 1);
  rtlsdr_reset_demod (dev);

  /* disable spectrum inversion and adjacent channel rejection */
  rtlsdr_demod_write_reg (dev, 1, 0x15, 0x00, 1);

  /* clear both DDC shift and IF frequency registers  */
  for (i = 0; i < 6; i++)
     rtlsdr_demod_write_reg (dev, 1, 0x16 + i, 0x00, 1);

  dev->fir = -1;
  rtlsdr_set_fir (dev, 0);

  /* enable SDR mode, disable DAGC (bit 5) */
  rtlsdr_demod_write_reg (dev, 0, 0x19, 0x05, 1);

  /* init FSM state-holding register */
  rtlsdr_demod_write_reg (dev, 1, 0x92, 0x00, 1);
  rtlsdr_demod_write_reg (dev, 1, 0x93, 0xf0, 1);
  rtlsdr_demod_write_reg (dev, 1, 0x94, 0x0f, 1);

  /* disable PID filter (enable_PID = 0) */
  rtlsdr_demod_write_reg (dev, 0, 0x61, 0x60, 1);

  /* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
  rtlsdr_demod_write_reg (dev, 0, 0x06, 0x80, 1);

  //dab dagc_target;     (S,8,7f) when dagc on
  rtlsdr_demod_write_reg (dev, 0, 0x17, 0x11, 1);

  //dagc_gain_set;  (S,8,1f) when dagc off
  rtlsdr_demod_write_reg (dev, 0, 0x18, 0x10, 1);

  /* Enable Zero-IF mode (en_bbin bit), DC cancellation (en_dc_est),
   * IQ estimation/compensation (en_iq_comp, en_iq_est)
   */
  rtlsdr_demod_write_reg (dev, 1, 0xb1, 0x1b, 1);

  /* enable In-phase + Quadrature ADC input */
  rtlsdr_demod_write_reg (dev, 0, 0x08, 0xcd, 1);

  /* disable 4.096 MHz clock output on pin TP_CK0 */
  rtlsdr_demod_write_reg (dev, 0, 0x0d, 0x83, 1);
}

static int rtlsdr_deinit_baseband (rtlsdr_dev_t *dev)
{
  int r = 0;

  if (!dev)
     return (-1);

  if (dev->tuner && dev->tuner->exit)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->exit) (dev); /* deinitialize tuner */
    rtlsdr_set_i2c_repeater (dev, 0);
  }

  /* poweroff demodulator and ADCs */
  rtlsdr_write_reg (dev, SYSB, DEMOD_CTL, 0x20, 1);
  RTL_TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
  return (r);
}

#if defined(DEBUG) || defined(rtlsdr_DEBUG)
void print_demod_register (rtlsdr_dev_t *dev, uint8_t page)
{
  int i, j;
  int reg = 0;

  printf ("Page %d\n", page);
  printf ("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
  for (i = 0; i < 16; i++)
  {
    printf ("%02x: ", reg);
    for (j = 0; j < 16; j++)
        printf ("%02x ", rtlsdr_demod_read_reg(dev, page, reg++, 1));
    printf ("\n");
  }
}

void print_rom (rtlsdr_dev_t *dev)
{
  uint8_t data [64];
  FILE   *pFile;
  int     i, r, addr = 0, len = sizeof(data);

  printf ("writing file: 'rtl2832.bin'\n");
  pFile = fopen ("rtl2832.bin", "wb");
  if (pFile)
  {
    for (i = 0; i < 1024; i++)
    {
      r = rtlsdr_read_array (dev, ROMB, addr, data, len);
      if (r)
      {
        printf ("Error reading ROM, r: %d.\n", r);
        break;
      }
      fwrite (data, 1, sizeof(data), pFile);
      addr += sizeof(data);
    }
    fclose (pFile);
  }
}

void print_usb_register (rtlsdr_dev_t *dev, uint16_t addr)
{
  uint8_t data [16];
  int     i, j, index, len = sizeof(data);

  printf ("       0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

  if (addr < 0x2000)
     index = ROMB;
  else if (addr < 0x3000)
     index = USBB;
  else if (addr < 0xfc00)
     index = SYSB;
  else
     index = IRB;

  for (i = 0; i < 16; i++)
  {
    printf ("%04x: ", addr);
    rtlsdr_read_array (dev, index, addr, data, len);

    for (j = 0; j < 16; j++)
        printf ("%02x ", data[j]);
    addr += sizeof(data);
    printf ("\n");
  }
}
#endif  /* DEBUG || rtlsdr_DEBUG */

int rtlsdr_set_if_freq (rtlsdr_dev_t *dev, int32_t freq)
{
  uint32_t rtl_xtal;
  int32_t  if_freq;
  uint8_t  tmp;
  int      r;

  if (!dev)
     return (-1);

  /* read corrected clock value */
  if (rtlsdr_get_xtal_freq (dev, &rtl_xtal, NULL))
  {
    RTL_TRACE (1, "%s (%.3f MHz): r: %d\n", __FUNCTION__, (double)freq / 1E6, -2);
    return (-2);
  }

  if_freq = ((freq * TWO_POW (22)) / rtl_xtal) * (-1);
  tmp = (if_freq >> 16) & 0x3f;
  r = rtlsdr_demod_write_reg (dev, 1, 0x19, tmp, 1);

  tmp = (if_freq >> 8) & 0xff;
  r |= rtlsdr_demod_write_reg (dev, 1, 0x1a, tmp, 1);

  tmp = if_freq & 0xff;
  r |= rtlsdr_demod_write_reg (dev, 1, 0x1b, tmp, 1);

  RTL_TRACE (2, "%s (%.3f MHz): IF-freq: %.3f MHz, XTAL: %.3f MHz, r: %d\n",
             __FUNCTION__, (double)freq / 1E6, (double)if_freq / 1E6, (double)rtl_xtal / 1E6, r);
  return (r);
}

static inline int rtlsdr_set_spectrum_inversion (rtlsdr_dev_t *dev, int sideband)
{
  return rtlsdr_demod_write_reg_mask (dev, 1, 0x15, sideband ? 0x00 : 0x01, 0x01);
}

static int rtlsdr_set_sample_freq_correction (rtlsdr_dev_t *dev, int ppb)
{
  int     r = 0;
  int16_t offs = ppb * (-1) * TWO_POW (24) / 1000000000;

  r |= rtlsdr_demod_write_reg (dev, 1, 0x3e, (offs >> 8) & 0x3f, 1);
  r |= rtlsdr_demod_write_reg (dev, 1, 0x3f, offs & 0xff, 1);
  RTL_TRACE (1, "%s(): ppb: %d, r: %d\n", __FUNCTION__, ppb, r);
  return (r);
}

int rtlsdr_set_xtal_freq (rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
  int r = 0;

  if (!dev)
     return (-1);

  if (rtl_freq > 0 &&
      (rtl_freq < MIN_RTL_XTAL_FREQ || rtl_freq > MAX_RTL_XTAL_FREQ))
     return (-2);

  if (rtl_freq > 0 && dev->rtl_xtal != rtl_freq)
  {
    dev->rtl_xtal = rtl_freq;

    /* update xtal-dependent settings */
    if (dev->rate)
       r = rtlsdr_set_sample_rate (dev, dev->rate);
  }

  if (dev->tun_xtal != tuner_freq)
  {
    if (tuner_freq == 0)
         dev->tun_xtal = dev->rtl_xtal;
    else dev->tun_xtal = tuner_freq;

    /* read corrected clock value into e4k and r82xx structure
     */
    if (rtlsdr_get_xtal_freq (dev, NULL, &dev->e4k_s.vco.fosc) ||
        rtlsdr_get_xtal_freq (dev, NULL, &dev->r82xx_c.xtal))
       return (-3);

    /* update xtal-dependent settings */
    if (dev->freq)
       r = rtlsdr_set_center_freq (dev, dev->freq);
  }
  RTL_TRACE (1, "%s (%.3f MHz): r: %d\n", __FUNCTION__, (double)tuner_freq / 1E6, r);
  return (r);
}

int rtlsdr_get_xtal_freq (rtlsdr_dev_t *dev, uint32_t *rtl_freq, double *tuner_freq)
{
  if (!dev)
     return (-1);

#define APPLY_PPM_CORR(val, ppb) (((val) * (1.0 + (ppb) / 1e9)))

  if (rtl_freq)
     *rtl_freq = (uint32_t) APPLY_PPM_CORR (dev->rtl_xtal, dev->corr);

  if (tuner_freq)
     *tuner_freq = APPLY_PPM_CORR (dev->tun_xtal, dev->corr);

  return (0);
}

int rtlsdr_write_eeprom (rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
  uint8_t cmd [2];
  int     i, r = 0;

  if (!dev)
     return (-1);

  if (len + offset > 256)
  {
    r = -2;
    goto quit;
  }

  for (i = 0; i < len; i++)
  {
    cmd [0] = i + offset;
    r = rtlsdr_write_array (dev, IICB, EEPROM_ADDR, cmd, 1);
    r = rtlsdr_read_array (dev, IICB, EEPROM_ADDR, &cmd[1], 1);

    /* only write the byte if it differs */
    if (cmd[1] == data[i])
      continue;

    cmd[1] = data[i];
    r = rtlsdr_write_array (dev, IICB, EEPROM_ADDR, cmd, 2);

    if (r != sizeof(cmd))
    {
      r = -3;
      goto quit;
    }

    /* for some EEPROMs (e.g. ATC 240LC02) we need a delay
     * between write operations, otherwise they will fail
     */
    Sleep (5);
  }

  r = 0;
quit:
  RTL_TRACE (2, "%s(): r: %d\n", __FUNCTION__, r);
  return (r);
}

int rtlsdr_read_eeprom (rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
  int r;

  if (!dev)
     r = -1;
  else if (len + offset > 256)
     r = -2;
  else
     r = rtlsdr_read_array (dev, TUNB, offset << 8 | EEPROM_ADDR, data, len);

  if (r < 0)
     r = -3;

  RTL_TRACE (2, "%s(): r: %d\n", __FUNCTION__, r);
  return (r);
}

int rtlsdr_set_center_freq (rtlsdr_dev_t *dev, uint32_t freq)
{
  int r = -1;

  if (!dev || !dev->tuner)
     return (-1);

  if (dev->direct_sampling_mode > RTLSDR_DS_Q)
    rtlsdr_update_ds (dev, freq);

  if (dev->direct_sampling)
    r = rtlsdr_set_if_freq (dev, freq);

  else if (dev->tuner && dev->tuner->set_freq)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->set_freq) (dev, freq - dev->offs_freq);
    rtlsdr_set_i2c_repeater (dev, 0);
  }

  if (!r)
    dev->freq = freq;
  else
    dev->freq = 0;

  RTL_TRACE (1, "%s (%.3f MHz): direct_sampling: %d, direct_sampling_mode: %d, r: %d\n",
             __FUNCTION__, (double)freq / 1E6, dev->direct_sampling, dev->direct_sampling_mode, r);
  return (r);
}

uint32_t rtlsdr_get_center_freq (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (0);
  return (dev->freq);
}

int rtlsdr_set_freq_correction_ppb (rtlsdr_dev_t *dev, int ppb)
{
  int r = 0;

  if (!dev)
     return (-1);

  if (dev->corr == ppb)
     return (0);

  dev->corr = ppb;
  r |= rtlsdr_set_sample_freq_correction (dev, ppb);

  /* read corrected clock value into e4k and r82xx structure
   */
  if (rtlsdr_get_xtal_freq (dev, NULL, &dev->e4k_s.vco.fosc) ||
      rtlsdr_get_xtal_freq (dev, NULL, &dev->r82xx_c.xtal))
     return (-3);

  if (dev->freq) /* retune to apply new correction value */
     r |= rtlsdr_set_center_freq (dev, dev->freq);

  RTL_TRACE (1, "%s (%d): r: %d\n", __FUNCTION__, rtlsdr_get_freq_correction(dev), r);
  return (r);
}

int rtlsdr_set_freq_correction (rtlsdr_dev_t *dev, int ppm)
{
  return rtlsdr_set_freq_correction_ppb (dev, ppm * 1000);
}

int rtlsdr_get_freq_correction (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (-1);
  return (dev->corr / 1000);
}

int rtlsdr_get_freq_correction_ppb (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (-1);
  return (dev->corr);
}

enum rtlsdr_tuner rtlsdr_get_tuner_type (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (RTLSDR_TUNER_UNKNOWN);
  return (dev->tuner_type);
}

int rtlsdr_get_tuner_gains (rtlsdr_dev_t *dev, int *gains)
{
  const int  unknown_gains[] = { 0 /* no gain values */ };
  const int *ptr = unknown_gains;
  int        len = sizeof(unknown_gains);

  if (!dev)
     return (-1);

  if (dev->tuner->get_gains)
     ptr = (*dev->tuner->get_gains) (&len);

  if (gains)   /* if no buffer provided, just return the count */
     memcpy (gains, ptr, len);

  return (len / sizeof(int));
}

int rtlsdr_set_and_get_tuner_bandwidth (rtlsdr_dev_t *dev, uint32_t bw, uint32_t *applied_bw, int apply_bw)
{
  int r2, r = 0;

  RTL_TRACE (1, "%s(): bw: %u, apply_bw: %d\n", __FUNCTION__, bw, apply_bw);

  *applied_bw = 0;    /* unknown */

  if (!dev || !dev->tuner)
     return (-1);

  if (!apply_bw)
  {
    if (dev->tuner->set_bw)
       r = (*dev->tuner->set_bw) (dev, bw > 0 ? bw : dev->rate, applied_bw, apply_bw);

    RTL_TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
    return (r);
  }

  if (dev->tuner->set_bw)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->set_bw) (dev, bw > 0 ? bw : dev->rate, applied_bw, apply_bw);
    rtlsdr_set_i2c_repeater (dev, 0);

    if (r)
       return (r);

    dev->bw = bw;
  }

  if (bw == 0)
  {
    r2 = rtlsdr_set_fir (dev, 0); // 2.4 MHz
    RTL_TRACE (1, "%s(): r2: %d\n", __FUNCTION__, r2);
  }
  else
  {
    if (bw <= 300000)
       r2 = rtlsdr_set_fir (dev, 2); // 0.3 MHz
    else if (bw <= 1500000 && *applied_bw >= 2000000)
       r2 = rtlsdr_set_fir (dev, 1); // 1.2 MHz
    else
       r2 = rtlsdr_set_fir (dev, 0); // 2.4 MHz

    RTL_TRACE (1, "%s(): r2: %d\n", __FUNCTION__, r2);
  }
  return (r);
}

int rtlsdr_set_tuner_bandwidth (rtlsdr_dev_t *dev, uint32_t bw)
{
  uint32_t applied_bw = 0;
  int      r = rtlsdr_set_and_get_tuner_bandwidth (dev, bw, &applied_bw, 1 /* =apply_bw */);

  RTL_TRACE (1, "%s (%.3f MHz): applied_bw: %.3f, r: %d\n", __FUNCTION__, (double)bw / 1E6, applied_bw / 1E6, r);
  return (r);
}

int rtlsdr_set_tuner_gain_index (rtlsdr_dev_t *dev, unsigned int index)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return (-1);

  if (index >= dev->gain_count - 1)
     index = dev->gain_count - 1;

  if (dev->gain_mode == 0) /* hardware mode */
     return (0);

  if (dev->tuner->set_gain_index)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->set_gain_index) (dev, index);
    rtlsdr_set_i2c_repeater (dev, 0);
  }

  if (!r)
       dev->gain_index = index;
  else dev->gain_index = 0;
  return (r);
}

int rtlsdr_set_tuner_gain (rtlsdr_dev_t *dev, int gain)
{
  int  r = 0;
  unsigned int i;

  if (!dev || !dev->tuner)
    return (-1);

  for (i = 0; i < dev->gain_count; i++)
      if (dev->gains[i] >= gain || i + 1 == dev->gain_count)
         break;

  r = rtlsdr_set_tuner_gain_index (dev, i);
  if (!r)
       dev->gain = dev->gains[i];
  else dev->gain = 0;

  RTL_TRACE (1, "%s(): gain: %.1f dB, r: %d\n", __FUNCTION__, (float)gain / 10, r);
  return (r);
}

int rtlsdr_get_tuner_gain (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (-1);
  return (dev->gain);
}

int rtlsdr_set_tuner_if_gain (rtlsdr_dev_t *dev, int stage, int gain)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return (-1);

  if (dev->tuner->set_if_gain)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->set_if_gain) (dev, stage, gain);
    rtlsdr_set_i2c_repeater (dev, 0);
  }
  RTL_TRACE (1, "%s(): gain: %.1f dB, r: %d\n", __FUNCTION__, (float)gain / 10, r);
  return (r);
}

int rtlsdr_set_tuner_gain_mode (rtlsdr_dev_t *dev, int mode)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return (-1);

  rtlsdr_set_i2c_repeater (dev, 1);
  r = (*dev->tuner->set_gain_mode) (dev, mode);
  rtlsdr_set_i2c_repeater (dev, 0);
  dev->gain_mode = mode;

  if (mode == 2)
  {
    r |= rtlsdr_set_tuner_gain_index (dev, 0);
    dev->softagc.softAgcMode = SOFTAGC_ON;
  }
  else
    dev->softagc.softAgcMode = SOFTAGC_OFF;

  RTL_TRACE (1, "%s(): mode: %d (%s), r: %d\n", __FUNCTION__, mode, mode == 0 ? "auto" : "manual", r);
  return (r);
}

int rtlsdr_set_tuner_sideband (rtlsdr_dev_t *dev, int sideband)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return (-1);

  if (dev->tuner->set_sideband)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->set_sideband) (dev, sideband);
    rtlsdr_set_i2c_repeater (dev, 0);
  }
  RTL_TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
  return (r);
}

int rtlsdr_set_tuner_i2c_register (rtlsdr_dev_t *dev, unsigned i2c_register, unsigned mask /* byte */, unsigned data /* byte */)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return (-1);

  if (dev->tuner->set_i2c_register)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->set_i2c_register) (dev, i2c_register, data, mask);
    rtlsdr_set_i2c_repeater (dev, 0);
  }
  RTL_TRACE (2, "%s(): r: %d\n", __FUNCTION__, r);
  return (r);
}

int rtlsdr_get_tuner_i2c_register (rtlsdr_dev_t *dev, uint8_t *data, int *len, int *strength)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return (-1);

  if (dev->tuner->get_i2c_register)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = (*dev->tuner->get_i2c_register) (dev, data, len, strength);
    rtlsdr_set_i2c_repeater (dev, 0);
  }

  if (dev->tuner_type == RTLSDR_TUNER_FC0012 ||
      dev->tuner_type == RTLSDR_TUNER_FC0013 ||
      dev->tuner_type == RTLSDR_TUNER_E4000)
  {
    if (dev->agc_mode)
       *strength -= 60;
  }

  RTL_TRACE (2, "%s(): AGC-mode: %d, strength: %d, r: %d\n", __FUNCTION__, dev->agc_mode, *strength, r);
  return (r);
}

int rtlsdr_set_dithering (rtlsdr_dev_t *dev, int dither)
{
  int r = 0;

  if (!dev || !dev->tuner)
     return -1;

  if (dev->tuner_type == RTLSDR_TUNER_R820T || dev->tuner_type == RTLSDR_TUNER_R828D)
  {
    rtlsdr_set_i2c_repeater (dev, 1);
    r = r82xx_set_dither (&dev->r82xx_p, dither);
    rtlsdr_set_i2c_repeater (dev, 0);
  }
  RTL_TRACE (2, "%s(): r: %d\n", __FUNCTION__, r);
  return (r);
}

int rtlsdr_set_sample_rate (rtlsdr_dev_t *dev, uint32_t samp_rate)
{
  int      r = 0;
  uint16_t tmp;
  uint32_t rsamp_ratio, real_rsamp_ratio;
  double   real_rate;

  if (!dev)
  {
    RTL_TRACE (1, "dev == NULL!\n");
    return (-1);
  }

  /* Check if the rate is supported by the resampler
   */
  if ((samp_rate <= 225000) || (samp_rate > 3200000) ||
       ((samp_rate > 300000) && (samp_rate <= 900000)))
  {
    RTL_TRACE (1, "Invalid sample rate: %u Hz\n", samp_rate);
    return (-EINVAL);
  }

  rsamp_ratio = (dev->rtl_xtal * TWO_POW (22)) / samp_rate;
  rsamp_ratio &= 0x0ffffffc;
  real_rsamp_ratio = rsamp_ratio | ((rsamp_ratio & 0x08000000) << 1);
  real_rate = (dev->rtl_xtal * TWO_POW (22)) / real_rsamp_ratio;

  if ((double)samp_rate != real_rate)
     RTL_TRACE (1, "Exact sample rate is: %f Hz\n", real_rate);

  dev->rate = (uint32_t) real_rate;

  tmp = (rsamp_ratio >> 16);
  r |= rtlsdr_demod_write_reg (dev, 1, 0x9f, tmp, 2);
  tmp = rsamp_ratio & 0xffff;

  r |= rtlsdr_demod_write_reg (dev, 1, 0xa1, tmp, 2);
  r |= rtlsdr_set_sample_freq_correction (dev, dev->corr);
  r |= rtlsdr_reset_demod (dev);

  /* recalculate offset frequency if offset tuning is enabled
   */
  if (dev->offs_freq)
     rtlsdr_set_offset_tuning (dev, 1);

  RTL_TRACE (1, "%s(): real_rate: %.3f MS/s, r: %d\n", __FUNCTION__, real_rate / 1E6, r);
  return (r);
}

uint32_t rtlsdr_get_sample_rate (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (0);
  return (dev->rate);
}

int rtlsdr_set_testmode (rtlsdr_dev_t *dev, int on)
{
  if (!dev)
     return (-1);

  int r = rtlsdr_demod_write_reg_mask (dev, 0, 0x19, on ? 0x02 : 0x00, 0x02);
  RTL_TRACE (1, "%s (%d): r: %d\n", __FUNCTION__, on, r);
  return (r);
}

int rtlsdr_set_agc_mode (rtlsdr_dev_t *dev, int on)
{
  if (!dev)
     return (-1);

  int r = rtlsdr_demod_write_reg_mask (dev, 0, 0x19, on ? 0x20 : 0x00, 0x20);
  dev->agc_mode = on;
  RTL_TRACE (1, "%s (%d): r: %d\n", __FUNCTION__, on, r);
  return (r);
}

int rtlsdr_set_direct_sampling (rtlsdr_dev_t *dev, int on)
{
  int r = 0;

  if (!dev)
     return (-1);

  if (on)
  {
    if (dev->tuner && dev->tuner->exit)
    {
      rtlsdr_set_i2c_repeater (dev, 1);
      r = (*dev->tuner->exit) (dev);
      rtlsdr_set_i2c_repeater (dev, 0);
    }

    /* disable Zero-IF mode */
    r |= rtlsdr_demod_write_reg (dev, 1, 0xb1, 0x1a, 1);

    /* disable spectrum inversion */
    r |= rtlsdr_demod_write_reg (dev, 1, 0x15, 0x00, 1);

    /* only enable In-phase ADC input */
    r |= rtlsdr_demod_write_reg (dev, 0, 0x08, 0x4d, 1);

    /* swap I and Q ADC, this allows to select between two inputs */
    r |= rtlsdr_demod_write_reg (dev, 0, 0x06, (on > 1) ? 0x90 : 0x80, 1);

    RTL_TRACE (1, "Enabled direct sampling mode, input %i, r: %d\n", on, r);
    dev->direct_sampling = on;
  }
  else
  {
    if (dev->tuner && dev->tuner->init)
    {
      rtlsdr_set_i2c_repeater (dev, 1);
      r |= (*dev->tuner->init) (dev);
      rtlsdr_set_i2c_repeater (dev, 0);
    }

    if (dev->tuner_type == RTLSDR_TUNER_R820T || dev->tuner_type == RTLSDR_TUNER_R828D)
    {
      r |= rtlsdr_set_if_freq (dev, R82XX_IF_FREQ);

      /* enable spectrum inversion */
      r |= rtlsdr_demod_write_reg (dev, 1, 0x15, 0x01, 1);
    }
    else
    {
      r |= rtlsdr_set_if_freq (dev, 0);

      /* enable In-phase + Quadrature ADC input */
      r |= rtlsdr_demod_write_reg (dev, 0, 0x08, 0xcd, 1);

      /* Enable Zero-IF mode */
      r |= rtlsdr_demod_write_reg (dev, 1, 0xb1, 0x1b, 1);
    }

    /* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
    // r |= rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

    RTL_TRACE (1, "Disabled direct sampling mode, r: %d\n", r);
    dev->direct_sampling = 0;
  }

  r |= rtlsdr_set_center_freq (dev, dev->freq);
  return (r);
}

int rtlsdr_get_direct_sampling (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (-1);
  return dev->direct_sampling;
}

int rtlsdr_set_ds_mode (rtlsdr_dev_t *dev, enum rtlsdr_ds_mode mode, uint32_t freq_threshold)
{
  uint32_t center_freq;

  if (!dev)
     return (-1);

  center_freq = rtlsdr_get_center_freq (dev);
  if (!center_freq)
     return (-2);

  if (!freq_threshold)
  {
    switch (dev->tuner_type)
    {
      default:
      case RTLSDR_TUNER_UNKNOWN:
           freq_threshold = 28800000;
           break; /* no idea!!! */

      case RTLSDR_TUNER_E4000:
           freq_threshold = 50 * 1000000;
           break; /* E4K_FLO_MIN_MHZ */

      case RTLSDR_TUNER_FC0012:
           freq_threshold = 28800000;
           break; /* no idea!!! */

      case RTLSDR_TUNER_FC0013:
           freq_threshold = 28800000;
           break; /* no idea!!! */

      case RTLSDR_TUNER_FC2580:
           freq_threshold = 28800000;
           break; /* no idea!!! */

      case RTLSDR_TUNER_R820T:
           freq_threshold = 24000000;
           break; /* ~ */

      case RTLSDR_TUNER_R828D:
           freq_threshold = 28800000;
           break; /* no idea!!! */
    }
  }

  dev->direct_sampling_mode = mode;
  dev->direct_sampling_threshold = freq_threshold;

  if (mode <= RTLSDR_DS_Q)
     rtlsdr_set_direct_sampling (dev, mode);

  return rtlsdr_set_center_freq (dev, center_freq);
}

static int rtlsdr_update_ds (rtlsdr_dev_t *dev, uint32_t freq)
{
  int new_ds = 0;
  int curr_ds = rtlsdr_get_direct_sampling (dev);

  if (curr_ds < 0)
     return (-1);

  switch (dev->direct_sampling_mode)
  {
    default:
    case RTLSDR_DS_IQ:
         break;

    case RTLSDR_DS_I:
         new_ds = 1;
         break;

    case RTLSDR_DS_Q:
         new_ds = 2;
         break;

    case RTLSDR_DS_I_BELOW:
         new_ds = (freq < dev->direct_sampling_threshold) ? 1 : 0;
         break;

    case RTLSDR_DS_Q_BELOW:
         new_ds = (freq < dev->direct_sampling_threshold) ? 2 : 0;
         break;
  }

  if (curr_ds != new_ds)
     return rtlsdr_set_direct_sampling (dev, new_ds);
  return (0);
}

int rtlsdr_set_offset_tuning (rtlsdr_dev_t *dev, int on)
{
  int r = 0;
  int bw;

  if (!dev)
     return (-1);

  if (dev->tuner_type == RTLSDR_TUNER_R820T || dev->tuner_type == RTLSDR_TUNER_R828D)
     return (-2);

  if (dev->direct_sampling)
     return (-3);

  if (on)
  {
    dev->offs_freq = dev->rate / 2;
    if ((dev->offs_freq < 400000) || (rtlsdr_demod_read_reg(dev, 0, 0x19, 1) & 0x04) == 0)
       dev->offs_freq = 400000;
  }
  else
    dev->offs_freq = 0;

  r |= rtlsdr_set_if_freq (dev, dev->offs_freq);

  if ((dev->tuner && dev->tuner->set_bw) && (dev->bw < (2 * dev->offs_freq)))
  {
    uint32_t applied_bw = 0;

    rtlsdr_set_i2c_repeater (dev, 1);
    if (on)
       bw = 2 * dev->offs_freq;
    else if (dev->bw > 0)
       bw = dev->bw;
    else
       bw = dev->rate;

    (*dev->tuner->set_bw) (dev, bw, &applied_bw, 1);
    rtlsdr_set_i2c_repeater (dev, 0);
  }

  if (dev->freq > dev->offs_freq)
     r |= rtlsdr_set_center_freq (dev, dev->freq);
  return (r);
}

int rtlsdr_get_offset_tuning (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (-1);
  return (dev->offs_freq) ? 1 : 0;
}

uint32_t rtlsdr_get_device_count (void)
{
  RTL_TRACE (2, "Calling 'List_Devices (-1, NULL)'\n");
  return List_Devices (-1, NULL);
}

const char *rtlsdr_get_device_name (uint32_t index)
{
  found_device found;
  const rtlsdr_dongle_t *device = NULL;

  RTL_TRACE (2, "Calling 'List_Devices (%d, &found)'\n", index);
  if (List_Devices(index, &found) < 0 || !found.DevicePath[0])
     return (NULL);

  device = find_known_device (found.vid, found.pid, __LINE__);
  if (device)
     return (device->name);
  return (NULL);
}

int rtlsdr_get_device_usb_strings (uint32_t index, char *manufact, char *product, char *serial)
{
  found_device found;
  rtlsdr_dev_t dev;
  int          r, count;

  count = List_Devices (index, &found);
  RTL_TRACE (1, "Calling 'List_Devices (%d, &found)' returned %d\n", index, count);

  if (count < 0 || !found.DevicePath[0])
     return (-1);

  memset (&dev, '\0', sizeof(dev));
  if (Open_Device(&dev, found.DevicePath, &r))
     r = rtlsdr_get_usb_strings (&dev, manufact, product, serial);

  Close_Device (&dev);
  if (r == 0 && product && index < ARRAY_SIZE(got_device_usb_product))
     got_device_usb_product[index] = true;
  return (r);
}

static int get_string_descriptor_ascii (rtlsdr_dev_t *dev, uint8_t index, char *data)
{
  ULONG   LengthTransferred;
  uint8_t buffer [255];
  int     i, di = 0;

  if (!WinUsb_GetDescriptor(dev->usbHandle, USB_STRING_DESCRIPTOR_TYPE, index, 0,
                            buffer, sizeof(buffer), &LengthTransferred))
  {
    last_error = GetLastError();
    RTL_TRACE_WINUSB ("WinUsb_GetDescriptor", last_error);
    return (-1);
  }

  for (i = 2; i < buffer[0]; i += 2)
      data [di++] = (char) buffer [i];
  data [di] = '\0';
  return (0);
}

int rtlsdr_get_usb_strings (rtlsdr_dev_t *dev, char *manufact, char *product, char *serial)
{
  int buf_max = 256;

  if (!dev || !dev->usbHandle)
     return (-1);

  if (manufact)
  {
    memset (manufact, '\0', buf_max);
    get_string_descriptor_ascii (dev, 1, manufact);
  }

  if (product)
  {
    memset (product, '\0', buf_max);
    get_string_descriptor_ascii (dev, 2, product);
  }

  if (serial)
  {
    ULONG   LengthTransferred;
    uint8_t buffer [32];

    memset (serial, '\0', buf_max);

    if (!WinUsb_GetDescriptor(dev->usbHandle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, buffer, sizeof(buffer), &LengthTransferred))
    {
      last_error = GetLastError();
      RTL_TRACE_WINUSB ("WinUsb_GetDescriptor", last_error);
      return (-1);
    }
    if (buffer[16] == 3)
       get_string_descriptor_ascii (dev, 3, serial);
  }

  RTL_TRACE (1, "rtlsdr_get_usb_strings():\n"
                "                   manufact: %s, product: %s, serial: %s\n",
             (manufact && *manufact) ? manufact : "<None>",
             (product  && *product)  ? product  : "<None>",
             (serial   && *serial)   ? serial   : "<None>");

  return (0);
}

int rtlsdr_get_index_by_serial (const char *serial)
{
  int  i, count, r;
  char str [256];

  if (!serial || serial[0] == '\0')
     return (-1);

  RTL_TRACE (2, "Calling 'List_Devices (-1, NULL)'.\n");
  count = List_Devices (-1, NULL);
  if (count < 0)
     return (-2);

  for (i = 0; i < count; i++)
  {
    r = rtlsdr_get_device_usb_strings (i, NULL, NULL, str);
    if (r == 0 && !_stricmp(serial, str))
       return (i);
  }
  return (-3);
}

/*
 * Returns true if the manufact_check and product_check strings match what is in the dongles EEPROM
 */
int rtlsdr_check_dongle_model (rtlsdr_dev_t *dev, const char *manufact, const char *product)
{
  if (!strcmp(dev->manufact, manufact) && !strcmp(dev->product, product))
     return (1);
  return (0);
}

int rtlsdr_open (rtlsdr_dev_t **out_dev, uint32_t index)
{
  int           r = -1;
  uint8_t       reg;
  found_device  found;
  rtlsdr_dev_t *dev = calloc (1, sizeof(*dev));

  if (!dev)
  {
    last_error = ERROR_NOT_ENOUGH_MEMORY;
    goto err;
  }

  InitializeCriticalSection (&dev->cs_mutex);
  dev->opening = 1;

  /* Find number of devices
   */
  RTL_TRACE (1, "%s(): Calling 'List_Devices (%d, &found)'.\n", __FUNCTION__, index);
  if (List_Devices(index, &found) < 0 || !found.DevicePath[0])
     goto err;

  /* Initialize the device
   */
  if (!Open_Device(dev, found.DevicePath, &r))
     goto err;

  /* Make it clear which device we have opended.
   */
  if (index < ARRAY_SIZE(got_device_usb_product) && !got_device_usb_product[index])
  {
    char manufact [256];
    char product [256];
    char serial [256];

    got_device_usb_product [index] = true;
    rtlsdr_get_usb_strings (dev, manufact, product, serial);
  }

  dev->rtl_xtal = DEF_RTL_XTAL_FREQ;
  RTL_TRACE (1, "Calling rtlsdr_init_baseband().\n");
  rtlsdr_init_baseband (dev);
  dev->opening = 0;

  /* Get device manufacturer and product id */
  r = rtlsdr_get_usb_strings (dev, dev->manufact, dev->product, NULL);

  /* Probe tuners */
  rtlsdr_set_i2c_repeater (dev, 1);

  reg = check_tuner (dev, E4K_I2C_ADDR, E4K_CHECK_ADDR);
  if (reg == E4K_CHECK_VAL)
  {
    RTL_TRACE (1, "Found Elonics E4000 tuner\n");
    dev->tuner_type = RTLSDR_TUNER_E4000;
    goto found;
  }

  reg = check_tuner (dev, FC001X_I2C_ADDR, FC001X_CHECK_ADDR);
  if (reg == FC0013_CHECK_VAL)
  {
    RTL_TRACE (1, "Found Fitipower FC0013 tuner\n");
    dev->tuner_type = RTLSDR_TUNER_FC0013;
    goto found;
  }

  reg = check_tuner (dev, R820T_I2C_ADDR, R82XX_CHECK_ADDR);
  if (reg == R82XX_CHECK_VAL)
  {
    RTL_TRACE (1, "Found Rafael Micro R820T tuner\n");
    dev->tuner_type = RTLSDR_TUNER_R820T;
    goto found;
  }

  reg = check_tuner (dev, R828D_I2C_ADDR, R82XX_CHECK_ADDR);
  if (reg == R82XX_CHECK_VAL)
  {
    RTL_TRACE (1, "Found Rafael Micro R828D tuner\n");
    dev->tuner_type = RTLSDR_TUNER_R828D;
    goto found;
  }

  /* initialise GPIOs */
  rtlsdr_set_gpio_output (dev, 4);

  /* reset tuner before probing */
  rtlsdr_set_gpio_bit (dev, 4, 1);
  rtlsdr_set_gpio_bit (dev, 4, 0);

  reg = check_tuner (dev, FC2580_I2C_ADDR, FC2580_CHECK_ADDR);
  if ((reg & 0x7f) == FC2580_CHECK_VAL)
  {
    RTL_TRACE (1, "Found FCI 2580 tuner\n");
    dev->tuner_type = RTLSDR_TUNER_FC2580;
    goto found;
  }

  reg = check_tuner (dev, FC001X_I2C_ADDR, FC001X_CHECK_ADDR);
  if (reg == FC0012_CHECK_VAL)
  {
    RTL_TRACE (1, "Found Fitipower FC0012 tuner\n");
    rtlsdr_set_gpio_output (dev, 6);
    dev->tuner_type = RTLSDR_TUNER_FC0012;
  }

found:
  /* use the rtl clock value by default
   */
  dev->tun_xtal = dev->rtl_xtal;
  dev->tuner = &tuners [dev->tuner_type];
  dev->index = index;

  switch (dev->tuner_type)
  {
    case RTLSDR_TUNER_FC2580:
         dev->tun_xtal = FC2580_XTAL_FREQ;
         break;

    case RTLSDR_TUNER_E4000:
         rtlsdr_demod_write_reg (dev, 1, 0x12, 0x5a, 1);   // DVBT_DAGC_TRG_VAL
         rtlsdr_demod_write_reg (dev, 1, 0x02, 0x40, 1);   // DVBT_AGC_TARG_VAL_0
         rtlsdr_demod_write_reg (dev, 1, 0x03, 0x5a, 1);   // DVBT_AGC_TARG_VAL_8_1
         rtlsdr_demod_write_reg (dev, 1, 0xc7, 0x30, 1);   // DVBT_AAGC_LOOP_GAIN
         rtlsdr_demod_write_reg (dev, 1, 0x04, 0xd0, 1);   // DVBT_LOOP_GAIN2_3_0
         rtlsdr_demod_write_reg (dev, 1, 0x05, 0xbe, 1);   // DVBT_LOOP_GAIN2_4
         rtlsdr_demod_write_reg (dev, 1, 0xc8, 0x18, 1);   // DVBT_LOOP_GAIN3
         rtlsdr_demod_write_reg (dev, 1, 0x06, 0x35, 1);   // DVBT_VTOP1
         rtlsdr_demod_write_reg (dev, 1, 0xc9, 0x21, 1);   // DVBT_VTOP2
         rtlsdr_demod_write_reg (dev, 1, 0xca, 0x21, 1);   // DVBT_VTOP3
         rtlsdr_demod_write_reg (dev, 1, 0xcb, 0x00, 1);   // DVBT_KRF1
         rtlsdr_demod_write_reg (dev, 1, 0x07, 0x40, 1);   // DVBT_KRF2
         rtlsdr_demod_write_reg (dev, 1, 0xcd, 0x10, 1);   // DVBT_KRF3
         rtlsdr_demod_write_reg (dev, 1, 0xce, 0x10, 1);   // DVBT_KRF4
         rtlsdr_demod_write_reg (dev, 0, 0x11, 0xe9d4, 2); // DVBT_AD7_SETTING
         rtlsdr_demod_write_reg (dev, 1, 0xe5, 0xf0, 1);   // DVBT_EN_GI_PGA
         rtlsdr_demod_write_reg (dev, 1, 0xd9, 0x00, 1);   // DVBT_THD_LOCK_UP
         rtlsdr_demod_write_reg (dev, 1, 0xdb, 0x00, 1);   // DVBT_THD_LOCK_DW
         rtlsdr_demod_write_reg (dev, 1, 0xdd, 0x14, 1);   // DVBT_THD_UP1
         rtlsdr_demod_write_reg (dev, 1, 0xde, 0xec, 1);   // DVBT_THD_DW1
         rtlsdr_demod_write_reg (dev, 1, 0xd8, 0x0c, 1);   // DVBT_INTER_CNT_LEN
         rtlsdr_demod_write_reg (dev, 1, 0xe6, 0x02, 1);   // DVBT_GI_PGA_STATE
         rtlsdr_demod_write_reg (dev, 1, 0xd7, 0x09, 1);   // DVBT_EN_AGC_PGA
         rtlsdr_demod_write_reg (dev, 0, 0x10, 0x49, 1);   // DVBT_REG_GPO
         rtlsdr_demod_write_reg (dev, 0, 0x0d, 0x85, 1);   // DVBT_REG_MON,DVBT_REG_MONSEL
         rtlsdr_demod_write_reg (dev, 0, 0x13, 0x02, 1);
         break;

    case RTLSDR_TUNER_FC0012:
    case RTLSDR_TUNER_FC0013:
         rtlsdr_demod_write_reg (dev, 1, 0x12, 0x5a, 1);   // DVBT_DAGC_TRG_VAL
         rtlsdr_demod_write_reg (dev, 1, 0x02, 0x40, 1);   // DVBT_AGC_TARG_VAL_0
         rtlsdr_demod_write_reg (dev, 1, 0x03, 0x5a, 1);   // DVBT_AGC_TARG_VAL_8_1
         rtlsdr_demod_write_reg (dev, 1, 0xc7, 0x2c, 1);   // DVBT_AAGC_LOOP_GAIN
         rtlsdr_demod_write_reg (dev, 1, 0x04, 0xcc, 1);   // DVBT_LOOP_GAIN2_3_0
         rtlsdr_demod_write_reg (dev, 1, 0x05, 0xbe, 1);   // DVBT_LOOP_GAIN2_4
         rtlsdr_demod_write_reg (dev, 1, 0xc8, 0x16, 1);   // DVBT_LOOP_GAIN3
         rtlsdr_demod_write_reg (dev, 1, 0x06, 0x35, 1);   // DVBT_VTOP1
         rtlsdr_demod_write_reg (dev, 1, 0xc9, 0x21, 1);   // DVBT_VTOP2
         rtlsdr_demod_write_reg (dev, 1, 0xca, 0x21, 1);   // DVBT_VTOP3
         rtlsdr_demod_write_reg (dev, 1, 0xcb, 0x00, 1);   // DVBT_KRF1
         rtlsdr_demod_write_reg (dev, 1, 0x07, 0x40, 1);   // DVBT_KRF2
         rtlsdr_demod_write_reg (dev, 1, 0xcd, 0x10, 1);   // DVBT_KRF3
         rtlsdr_demod_write_reg (dev, 1, 0xce, 0x10, 1);   // DVBT_KRF4
         rtlsdr_demod_write_reg (dev, 0, 0x11, 0xe9bf, 2); // DVBT_AD7_SETTING
         rtlsdr_demod_write_reg (dev, 1, 0xe5, 0xf0, 1);   // DVBT_EN_GI_PGA
         rtlsdr_demod_write_reg (dev, 1, 0xd9, 0x00, 1);   // DVBT_THD_LOCK_UP
         rtlsdr_demod_write_reg (dev, 1, 0xdb, 0x00, 1);   // DVBT_THD_LOCK_DW
         rtlsdr_demod_write_reg (dev, 1, 0xdd, 0x11, 1);   // DVBT_THD_UP1
         rtlsdr_demod_write_reg (dev, 1, 0xde, 0xef, 1);   // DVBT_THD_DW1
         rtlsdr_demod_write_reg (dev, 1, 0xd8, 0x0c, 1);   // DVBT_INTER_CNT_LEN
         rtlsdr_demod_write_reg (dev, 1, 0xe6, 0x02, 1);   // DVBT_GI_PGA_STATE
         rtlsdr_demod_write_reg (dev, 1, 0xd7, 0x09, 1);   // DVBT_EN_AGC_PGA
         break;

    case RTLSDR_TUNER_R828D:
         /* If NOT an RTL-SDR Blog V4, set typical R828D 16 MHz freq. Otherwise, keep at 28.8 MHz. */
         if (rtlsdr_check_dongle_model(dev, "RTLSDRBlog", "Blog V4"))
            RTL_TRACE (1, "RTL-SDR Blog V4 Detected\n");
         else
         {
           dev->tun_xtal = R828D_XTAL_FREQ;

           /* power off slave demod on GPIO0 to reset CXD2837ER */
           rtlsdr_set_gpio_bit (dev, 0, 0);
           rtlsdr_write_reg_mask (dev, SYSB, GPOE, 0x00, 0x01);
           Sleep (50);

           /* power on slave demod on GPIO0 */
           rtlsdr_set_gpio_bit (dev, 0, 1);
           rtlsdr_set_gpio_output (dev, 0);

           /* check slave answers */
           reg = check_tuner (dev, MN8847X_I2C_ADDR, MN8847X_CHECK_ADDR);
           if (reg == MN88472_CHIP_ID)
           {
             RTL_TRACE (1, "Found Panasonic MN88472 demod\n");
             dev->slave_demod = SLAVE_DEMOD_MN88472;
             goto demod_found;
           }

           if (reg == MN88473_CHIP_ID)
           {
             RTL_TRACE (1, "Found Panasonic MN88473 demod\n");
             dev->slave_demod = SLAVE_DEMOD_MN88473;
             goto demod_found;
           }

           reg = check_tuner (dev, CXD2837_I2C_ADDR, CXD2837_CHECK_ADDR);
           if (reg == CXD2837ER_CHIP_ID)
           {
             RTL_TRACE (1, "Found Sony CXD2837ER demod\n");
             dev->slave_demod = SLAVE_DEMOD_CXD2837ER;
             goto demod_found;
           }

           reg = check_tuner (dev, SI2168_I2C_ADDR, SI2168_CHECK_ADDR);
           if (reg == SI2168_CHIP_ID)
           {
             RTL_TRACE (1, "Found Silicon Labs SI2168 demod\n");
             dev->slave_demod = SLAVE_DEMOD_SI2168;
           }

demod_found:
           if (dev->slave_demod) /* switch off DVBT2 demod */
           {
             rtlsdr_write_reg (dev, SYSB, GPO, 0x88, 1);
             rtlsdr_write_reg (dev, SYSB, GPOE, 0x9d, 1);
             rtlsdr_write_reg (dev, SYSB, GPD, 0x02, 1);
           }
         }
         /* fall-through */

    case RTLSDR_TUNER_R820T:
         RTL_TRACE (1, "Writing DVBT_DAGC_TRG_VAL.\n");
         rtlsdr_demod_write_reg (dev, 1, 0x12, 0x5a, 1); // DVBT_DAGC_TRG_VAL
         rtlsdr_demod_write_reg (dev, 1, 0x02, 0x40, 1); // DVBT_AGC_TARG_VAL_0
         rtlsdr_demod_write_reg (dev, 1, 0x03, 0x80, 1); // DVBT_AGC_TARG_VAL_8_1
         rtlsdr_demod_write_reg (dev, 1, 0xc7, 0x24, 1); // DVBT_AAGC_LOOP_GAIN
         rtlsdr_demod_write_reg (dev, 1, 0x04, 0xcc, 1); // DVBT_LOOP_GAIN2_3_0
         rtlsdr_demod_write_reg (dev, 1, 0x05, 0xbe, 1); // DVBT_LOOP_GAIN2_4
         rtlsdr_demod_write_reg (dev, 1, 0xc8, 0x14, 1); // DVBT_LOOP_GAIN3
         rtlsdr_demod_write_reg (dev, 1, 0x06, 0x35, 1); // DVBT_VTOP1
         rtlsdr_demod_write_reg (dev, 1, 0xc9, 0x21, 1); // DVBT_VTOP2
         rtlsdr_demod_write_reg (dev, 1, 0xca, 0x21, 1); // DVBT_VTOP3
         rtlsdr_demod_write_reg (dev, 1, 0xcb, 0x00, 1); // DVBT_KRF1
         rtlsdr_demod_write_reg (dev, 1, 0x07, 0x40, 1); // DVBT_KRF2
         rtlsdr_demod_write_reg (dev, 1, 0xcd, 0x10, 1); // DVBT_KRF3
         rtlsdr_demod_write_reg (dev, 1, 0xce, 0x10, 1); // DVBT_KRF4
         rtlsdr_demod_write_reg (dev, 0, 0x11, 0xf4, 1); // DVBT_AD7_SETTING

         /* disable Zero-IF mode */
         rtlsdr_demod_write_reg (dev, 1, 0xb1, 0x1a, 1);

         /* only enable In-phase ADC input */
         rtlsdr_demod_write_reg (dev, 0, 0x08, 0x4d, 1);

         /* the R82XX use 3.57 MHz IF for the DVB-T 6 MHz mode */
         rtlsdr_set_if_freq (dev, R82XX_IF_FREQ);

         /* enable spectrum inversion */
         rtlsdr_demod_write_reg (dev, 1, 0x15, 0x01, 1);
         break;

    case RTLSDR_TUNER_UNKNOWN:
         RTL_TRACE (1, "No supported tuner found\n");
         rtlsdr_set_direct_sampling (dev, 1);
         break;

    default:
         break;
  }

  if (dev->tuner->init)
     r = (*dev->tuner->init) (dev);

  int r2 = rtlsdr_set_i2c_repeater (dev, 0);
  RTL_TRACE (1, "rtlsdr_set_i2c_repeater(0): r: %d\n", r2);

  softagc_init (dev);
  *out_dev = dev;
  return (r);

err:
  if (dev)
  {
    Close_Device (dev);
    DeleteCriticalSection (&dev->cs_mutex);
    *out_dev = NULL;
    free (dev);
  }
  return (r);
}

/*
 * Free the transfer buffers
 */
static void rtlsdr_free (rtlsdr_dev_t *dev)
{
  uint32_t i;

  RTL_TRACE (1, "rtlsdr_free (0x%p): overlapped: 0x%p, xfer_buf: 0x%p\n",
             dev, dev->overlapped, dev->xfer_buf);

  EnterCriticalSection (&dev->cs_mutex);

  if (dev->overlapped)
  {
    for (i = 0; i < dev->num_xfer_buf; i++)
        free (dev->overlapped[i]);
    free (dev->overlapped);
    dev->overlapped = NULL;
  }
  if (dev->xfer_buf)
  {
    for (i = 0; i < dev->num_xfer_buf; i++)
        free (dev->xfer_buf[i]);
    free (dev->xfer_buf);
    dev->xfer_buf = NULL;
  }

  dev->num_xfer_buf = 0;
  LeaveCriticalSection (&dev->cs_mutex);
}

int rtlsdr_close (rtlsdr_dev_t *dev)
{
  int r, entry_state, exit_state, loops = -1;

  if (!dev)
     return (-1);

  entry_state = dev->async_status;

  /* Automatic de-activation of bias-T
   */
  r = rtlsdr_set_bias_tee (dev, 0);

  if (!dev->opening)
  {
    /* block until all async operations have been completed (if any) */
    while (dev->async_status != RTLSDR_INACTIVE)
    {
      Sleep (1);
      if (dev->async_status == RTLSDR_CANCELING && ++loops >= 10)
         break;
    }
    rtlsdr_deinit_baseband (dev);
  }

  exit_state = dev->async_status;

  RTL_TRACE (1, "%s(): loops: %d, state: %s -> %s\n",
             __FUNCTION__, loops, async_status_name(entry_state), async_status_name(exit_state));

  Close_Device (dev);
  rtlsdr_free (dev);
  softagc_close (dev);

  DeleteCriticalSection (&dev->cs_mutex);
  free (dev);
  return (r);
}

int rtlsdr_reset_buffer (rtlsdr_dev_t *dev)
{
  if (!dev)
     return (-1);

  if (!WinUsb_ResetPipe(dev->usbHandle, EP_RX))
  {
    last_error = GetLastError();
    RTL_TRACE_WINUSB ("WinUsb_ResetPipe", last_error);
  }
  return (0);
}

int rtlsdr_read_sync (rtlsdr_dev_t *dev, void *buf, int len,  int *n_read)
{
  ULONG bytesRead = 0;

  RTL_TRACE (2, "%s(): dev: 0x%p\n", __FUNCTION__, dev);

  if (!dev)
     return (-1);

  rtlsdr_write_reg (dev, USBB, USB_EPA_CTL, 0x0000, 2);

  if (!WinUsb_ReadPipe(dev->usbHandle, EP_RX, buf, len, &bytesRead, NULL))
  {
    last_error = GetLastError();
    RTL_TRACE_WINUSB ("WinUsb_ReadPipe", last_error);
  }
  else
    RTL_TRACE (3, "WinUsb_ReadPipe(): got %lu bytes.\n", bytesRead);

  *n_read = bytesRead;
  rtlsdr_write_reg (dev, USBB, USB_EPA_CTL, 0x1002, 2);
  return (0);
}

static int rtlsdr_read_buffer (rtlsdr_dev_t *dev, uint8_t *xfer_buf, uint32_t buf_len, OVERLAPPED *overlapped)
{
  if (!WinUsb_ReadPipe(dev->usbHandle, EP_RX, xfer_buf, buf_len, NULL, overlapped))
  {
    DWORD error = GetLastError();

    if (error != ERROR_IO_PENDING)
    {
      last_error = error;
      dev->async_cancel = 1;
      RTL_TRACE_WINUSB ("WinUsb_ReadPipe", error);
      return (1);
    }
  }
  return (0);
}

int rtlsdr_read_async (rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t callback, void *ctx,
                       uint32_t buf_num, uint32_t buf_len)
{
  UCHAR policy = 1;
  uint32_t i;

  if (!dev)
  {
    RTL_TRACE (1, "%s(): dev: NULL!\n", __FUNCTION__);
    return (-1);
  }

  if (dev->async_status != RTLSDR_INACTIVE)
  {
    RTL_TRACE (1, "%s(): dev: 0x%p, state not RTLSDR_INACTIVE\n", __FUNCTION__, dev);
    return (-2);
  }

  RTL_TRACE (1, "%s(): dev: 0x%p, state: RTLSDR_INACTIVE -> RTLSDR_RUNNING\n", __FUNCTION__, dev);

  dev->async_status = RTLSDR_RUNNING;
  dev->async_cancel = 0;

  if (buf_num == 0)
     buf_num = DEFAULT_BUF_NUMBER;

  if (!buf_len || buf_len % 512 != 0) /* len must be multiple of 512 */
     buf_len = DEFAULT_BUF_LENGTH;

  if (!WinUsb_ResetPipe(dev->usbHandle, EP_RX))
  {
    last_error = GetLastError();
    RTL_TRACE_WINUSB ("WinUsb_ResetPipe", last_error);
  }

  if (!WinUsb_SetPipePolicy(dev->usbHandle, EP_RX, RAW_IO, 1, &policy))
  {
    last_error = GetLastError();
    RTL_TRACE_WINUSB ("WinUsb_GetPipePolicy", last_error);
  }

  /* Alloc async buffers
   */
  dev->num_xfer_buf = 0;
  dev->overlapped = calloc (buf_num, sizeof(OVERLAPPED*));
  dev->xfer_buf   = calloc (buf_num * sizeof(uint8_t*), 1);
  if (dev->overlapped)
  {
    for (i = 0; i < buf_num; i++)
        dev->overlapped [i] = calloc (sizeof(OVERLAPPED), 1);
  }
  if (dev->xfer_buf)
  {
    for (i = 0; i < buf_num; i++)
        dev->xfer_buf [i] = malloc (buf_len);
  }

  if (!dev->overlapped || !dev->xfer_buf)
       dev->async_cancel = 1;
  else dev->num_xfer_buf = buf_num;

  /* Start transfers
   */
  rtlsdr_write_reg (dev, USBB, USB_EPA_CTL, 0x1002, 2);
  rtlsdr_write_reg (dev, USBB, USB_EPA_CTL, 0x0000, 2);

  /* Submit transfers
   */
  for (i = 0; i < dev->num_xfer_buf; i++)
  {
    if (rtlsdr_read_buffer(dev, dev->xfer_buf[i], buf_len, dev->overlapped[i]))
       break;
  }

  /* Receiver loop
   */
  while (!dev->async_cancel)
  {
    for (i = 0; i < dev->num_xfer_buf; i++)
    {
      DWORD NumberOfBytesTransferred = 0;

      /* Wait for the operation to complete before continuing.
       * You could do some background work if you wanted to.
       */
      if (WinUsb_GetOverlappedResult(dev->usbHandle, dev->overlapped[i], &NumberOfBytesTransferred, TRUE))
      {
        if (NumberOfBytesTransferred && callback)
        {
          softagc (dev, dev->xfer_buf[i], NumberOfBytesTransferred);
          (*callback) (dev->xfer_buf[i], NumberOfBytesTransferred, ctx);
          RTL_TRACE (3, "WinUsb_GetOverlappedResult(): got %lu bytes overlapped.\n", NumberOfBytesTransferred);
        }
      }
      else
      {
        last_error = GetLastError();
        RTL_TRACE_WINUSB ("WinUsb_GetOverlappedResult", last_error);
        dev->async_cancel = 1;
        break;
      }

      if (rtlsdr_read_buffer(dev, dev->xfer_buf[i], buf_len, dev->overlapped[i]))
         break;
    }
  }

  /* Stop transfers
   */
  rtlsdr_write_reg (dev, USBB, USB_EPA_CTL, 0x1002, 2);

  if (!WinUsb_AbortPipe(dev->usbHandle, EP_RX))
  {
    last_error = GetLastError();
    RTL_TRACE_WINUSB ("WinUsb_AbortPipe", last_error);
  }

  rtlsdr_free (dev);
  dev->async_status = RTLSDR_INACTIVE;
  return (0);
}

int rtlsdr_cancel_async (rtlsdr_dev_t *dev)
{
  int entry_state = -1;
  int exit_state  = -1;
  int r = 0;  /* assume success */

  if (!dev)
  {
    r = -1;
    goto quit;
  }

  /* if streaming, try to cancel gracefully
   */
  entry_state = dev->async_status;
  if (dev->async_status == RTLSDR_RUNNING)
  {
    dev->async_status = RTLSDR_CANCELING;
    dev->async_cancel = 1;
    goto quit;
  }

  /* if called while in pending state, change the state forcefully
   */
#if 0
  if (dev->async_status != RTLSDR_INACTIVE)
  {
    dev->async_status = RTLSDR_INACTIVE;
    goto quit;
  }
#endif

  r = -2;

quit:
  if (!dev)
       exit_state = RTLSDR_INACTIVE;
  else exit_state = dev->async_status;
  RTL_TRACE (1, "%s(): r: %d, dev: 0x%p, state: %s -> %s\n",
             __FUNCTION__, r, dev, async_status_name(entry_state), async_status_name(exit_state));

  return (r);
}

int rtlsdr_wait_async (rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx)
{
  return rtlsdr_read_async (dev, cb, ctx, 0, 0);
}

double rtlsdr_get_tuner_clock (void *dev)
{
  double tuner_freq;

  if (!dev)
     return (-1);

  /* read corrected clock value */
  if (rtlsdr_get_xtal_freq ((rtlsdr_dev_t*)dev, NULL, &tuner_freq))
      return (-1);
  return (tuner_freq);
}

/*
 * Infrared (IR) sensor support
 * based on Linux dvb_usb_rtl28xxu drivers/media/usb/dvb-usb-v2/rtl28xxu.h
 * Copyright (C) 2009 Antti Palosaari <crope@iki.fi>
 * Copyright (C) 2011 Antti Palosaari <crope@iki.fi>
 * Copyright (C) 2012 Thomas Mair <thomas.mair86@googlemail.com>
 */
struct rtl28xxu_reg_val {
  uint16_t block;
  uint16_t reg;
  uint8_t val;
};

struct rtl28xxu_reg_val_mask {
  uint16_t block;
  uint16_t reg;
  uint8_t  val;
  uint8_t  mask;
};

static const struct rtl28xxu_reg_val refresh_tab[] = {
  { IRB, IR_RX_IF,       0x03 },
  { IRB, IR_RX_BUF_CTRL, 0x80 },
  { IRB, IR_RX_CTRL,     0x80 },
};

static const struct rtl28xxu_reg_val_mask init_tab[] = {
  { USBB, DEMOD_CTL1,      0x00, 0x04 },
  { USBB, DEMOD_CTL1,      0x00, 0x08 },
  { USBB, USB_CTRL,        0x20, 0x20 },
  { USBB, GPD,             0x00, 0x08 },
  { USBB, GPOE,            0x08, 0x08 },
  { USBB, GPO,             0x08, 0x08 },
  { IRB, IR_MAX_DURATION0, 0xd0, 0xff },
  { IRB, IR_MAX_DURATION1, 0x07, 0xff },
  { IRB, IR_IDLE_LEN0,     0xc0, 0xff },
  { IRB, IR_IDLE_LEN1,     0x00, 0xff },
  { IRB, IR_GLITCH_LEN,    0x03, 0xff },
  { IRB, IR_RX_CLK,        0x09, 0xff },
  { IRB, IR_RX_CFG,        0x1c, 0xff },
  { IRB, IR_MAX_H_TOL_LEN, 0x1e, 0xff },
  { IRB, IR_MAX_L_TOL_LEN, 0x1e, 0xff },
  { IRB, IR_RX_CTRL,       0x80, 0xff },
};

int rtlsdr_ir_query (rtlsdr_dev_t *d, uint8_t *buf, size_t buf_len)
{
  int      ret = -1;
  size_t   i;
  uint32_t len;

  /* init remote controller */
  if (!d->rc_active)
  {
    RTL_TRACE (1, "initializing remote controller\n");
    for (i = 0; i < ARRAY_SIZE (init_tab); i++)
    {
      ret = rtlsdr_write_reg_mask (d, init_tab[i].block, init_tab[i].reg,
                                   init_tab[i].val, init_tab[i].mask);

      if (ret < 0)
      {
        RTL_TRACE (1, "write %zu reg %d %.4x %.2x %.2x failed\n", i, init_tab[i].block,
                   init_tab[i].reg, init_tab[i].val, init_tab[i].mask);
        goto err;
      }
    }
    d->rc_active = 1;
    RTL_TRACE (1, "remote controller active\n");
  }

  /* TODO: option to IR disable
   */
  buf[0] = rtlsdr_read_reg (d, IRB, IR_RX_IF);

  if (buf[0] != 0x83)
  {
    if (buf[0] == 0 || /* no IR signal */
        /*
         * also observed: 0x82, 0x81 - with lengths 1, 5, 0.. unknown, sometimes occurs at edges
         * "IR not ready"? causes a -7 timeout if we read
         */
        buf[0] == 0x82 || buf[0] == 0x81)
    {
      /* graceful exit */
    }
    else
      RTL_TRACE (1, "read IR_RX_IF unexpected: %.2x\n", buf[0]);

    ret = 0;
    goto exit;
  }

  buf[0] = rtlsdr_read_reg (d, IRB, IR_RX_BC);
  len = buf[0];

  if (len > buf_len)
  {
    RTL_TRACE (1, "read IR_RX_BC too large for buffer, %lu > %lu\n", buf_len, buf_len);
    goto exit;
  }

  if ((len != 6) && len < 70) /* message is not complete */
  {
    uint32_t len2;

    Sleep (72 - len);
    len2 = rtlsdr_read_reg (d, IRB, IR_RX_BC);
#if 0
    if (len != len2)
       RTL_TRACE (1, "len=%d, len2=%d\n", len, len2);
#endif

    if (len2 > len)
       len = len2;
  }

  if (len > 0)
  {
    /* read raw code from HW */
    ret = rtlsdr_read_array (d, IRB, IR_RX_BUF, buf, len);
    if (ret < 0)
       goto err;

    /* let HW receive new code */
    for (i = 0; i < ARRAY_SIZE (refresh_tab); i++)
    {
      ret = rtlsdr_write_reg (d, refresh_tab[i].block, refresh_tab[i].reg, refresh_tab[i].val, 1);
      if (ret < 0)
         goto err;
    }
  }

  /* On success return length */
  ret = len;

exit:
  return (ret);

err:
  RTL_TRACE (1, "failed=%d\n", ret);
  return (ret);
}

int rtlsdr_set_bias_tee_gpio (rtlsdr_dev_t *dev, int gpio, int on)
{
  if (!dev)
     return (-1);

  rtlsdr_set_gpio_output (dev, gpio);
  rtlsdr_set_gpio_bit (dev, gpio, on);
  return (0);
}

int rtlsdr_set_bias_tee (rtlsdr_dev_t *dev, int on)
{
  if (dev->slave_demod)
     return (0);
  return rtlsdr_set_bias_tee_gpio (dev, 0, on);
}

const char *rtlsdr_get_opt_help (int longInfo)
{
  if (longInfo)
     return ("[ -O  set RTL driver options separated with ':', e.g. -O 'bw=1500:agc=0' ]\n"
             "  f=<freqHz>            set tuner frequency\n"
             "  bw=<bw_in_kHz>        set tuner bandwidth\n"
             "  sb=<sideband>         set tuner sideband/mirror: '0' for lower side band,\n"
             "                        '1' for upper side band. default for R820T/2: '0'\n"
             "  agc=<tuner_gain_mode> activates tuner agc with '1'. deactivates with '0'\n"
             "  gain=<tenth_dB>       set tuner gain. 400 for 40.0 dB\n"
             "  dagc=<rtl_agc>        set RTL2832's digital agc (after ADC). 1 to activate. 0 to deactivate\n"
             "  ds=<direct_sampling>  deactivate/bypass tuner with 1\n"
             "  T=<bias_tee>          1 activates power at antenna one some dongles, e.g. rtl-sdr.com's V3\n");

  return ("[ -O  set RTL options string separated with ':', e.g. -O 'bw=1500:agc=0' ]\n"
          "   verbose:f=<freqHz>:bw=<bw_in_kHz>:sb=<sideband>\n"
          "   agc=<tuner_gain_mode>:gain=<tenth_dB>:dagc=<rtl_agc>\n"
          "   ds=<direct_sampling_mode>:T=<bias_tee>\n");
}

int rtlsdr_set_opt_string (rtlsdr_dev_t *dev, const char *opts, int verbose)
{
  char *optStr, * optPart;
  int   retAll = 0;

  if (!dev)
     return (-1);

  optStr = _strdup (opts);
  if (!optStr)
  {
    last_error = ERROR_NOT_ENOUGH_MEMORY;
    return (-1);
  }

  optPart = strtok (optStr, ":,");

  while (optPart)
  {
    int ret = 0;

    if (!strcmp (optPart, "verbose"))
    {
      RTL_TRACE (1, "rtlsdr_set_opt_string(): parsed option verbose\n");
      dev->verbose = 1;
    }
    else if (!strncmp (optPart, "f=", 2))
    {
      uint32_t freq = (uint32_t) atol (optPart + 2);
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed frequency %u\n", (unsigned) freq);
      ret = rtlsdr_set_center_freq (dev, freq);
    }
    else if (!strncmp (optPart, "bw=", 3))
    {
      uint32_t bw = (uint32_t) (atol (optPart + 3) * 1000);
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed bandwidth %u\n", (unsigned) bw);
      ret = rtlsdr_set_tuner_bandwidth (dev, bw);
    }
    else if (!strncmp (optPart, "agc=", 4))
    {
      int manual = 1 - atoi (optPart + 4); /* invert logic */
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed tuner gain mode, manual=%d\n", manual);
      ret = rtlsdr_set_tuner_gain_mode (dev, manual);
    }
    else if (!strncmp (optPart, "gain=", 5))
    {
      int gain = atoi (optPart + 5);
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed tuner gain = %d /10 dB\n", gain);
      ret = rtlsdr_set_tuner_gain (dev, gain);
    }
    else if (!strncmp (optPart, "dagc=", 5))
    {
      int on = atoi (optPart + 5);
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed rtl/digital gain mode %d\n", on);
      ret = rtlsdr_set_agc_mode (dev, on);
    }
    else if (!strncmp (optPart, "ds=", 3))
    {
      int on = atoi (optPart + 3);
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed direct sampling mode %d\n", on);
      ret = rtlsdr_set_direct_sampling (dev, on);
    }
    else if (!strncmp (optPart, "t=", 2) || !strncmp (optPart, "T=", 2))
    {
      int on = atoi (optPart + 2);
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed bias tee %d\n", on);
      ret = rtlsdr_set_bias_tee (dev, on);
    }
    else
    {
      RTL_TRACE (verbose, "rtlsdr_set_opt_string(): parsed unknown option '%s'\n", optPart);
      last_error = ERROR_INVALID_PARAMETER;
      ret = -1;  /* unknown option */
    }

    RTL_TRACE (verbose, "  application of option returned %d\n", ret);

    if (ret < 0)
       retAll = ret;

    optPart = strtok (NULL, ":,");
  }

  free (optStr);
  return (retAll);
}

void rtlsdr_cal_imr (const int val)
{
  cal_imr = val;
}

const char * rtlsdr_get_ver_id (void)
{
  return RTL_VER_ID " (" __DATE__ ")";
}

uint32_t rtlsdr_get_version (void)
{
  return ((uint32_t)RTLSDR_MAJOR << 24) | ((uint32_t)RTLSDR_MINOR << 16) |
          ((uint32_t)RTLSDR_MICRO << 8) | (uint32_t)RTLSDR_NANO;
}

static void softagc_control_worker (void *arg)
{
  rtlsdr_dev_t         *dev = (rtlsdr_dev_t*) arg;
  struct softagc_state *agc = &dev->softagc;

  while (!agc->exit_command_thread)
  {
    if (agc->command_changeGain)
    {
      agc->command_changeGain = 0;
      rtlsdr_set_tuner_gain_index (dev, agc->command_newGain);
    }
    else
      Sleep (10);
  }
}

static void softagc_init (rtlsdr_dev_t *dev)
{
  struct softagc_state *agc = &dev->softagc;

  /* prepare thread */
  dev->gain_count = rtlsdr_get_tuner_gains (dev, dev->gains);
  dev->gain = -1;
  dev->gain_mode = 0; /* hardware AGC */
  dev->gain_index = 0;
  agc->exit_command_thread = 0;
  agc->command_newGain = 0;
  agc->command_changeGain = 0;
  agc->softAgcMode = SOFTAGC_OFF;

  /* Create thread */
  agc->command_thread = _beginthread (softagc_control_worker, 0, dev);
}

static void softagc_close (rtlsdr_dev_t *dev)
{
  struct softagc_state *agc = &dev->softagc;

  agc->softAgcMode = SOFTAGC_OFF;
  agc->exit_command_thread = 1;
  Sleep (50);

  RTL_TRACE (1, "%s(): killing thread: %zu\n", __FUNCTION__, agc->command_thread);
  TerminateThread (&agc->command_thread, 0);
  CloseHandle ((HANDLE)agc->command_thread);
}

static void softagc (rtlsdr_dev_t *dev, uint8_t *buf, int len)
{
  struct softagc_state *agc = &dev->softagc;
  int     overload = 0;
  int     high_level = 0;
  uint8_t u;

  if (agc->softAgcMode == SOFTAGC_OFF)
     return;

  /* detect oversteering
   */
  for (int i = 0; i < len; i++)
  {
    u = buf[i];

    if (u == 0 || u == 255) // 0 dBFS
       overload++;

    if (u < 64 || u > 191) // -6 dBFS
       high_level++;
  }

  if (8000 * overload >= len)
  {
    if (dev->gain_index > 0)
    {
      agc->command_newGain = dev->gain_index - 1;
      agc->command_changeGain = 1;
    }
  }
  else if (8000 * high_level <= len)
  {
    if (dev->gain_index < dev->gain_count - 1)
    {
      agc->command_newGain = dev->gain_index + 1;
      agc->command_changeGain = 1;
    }
  }
}
