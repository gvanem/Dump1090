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

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>
#include <libusb.h>

#define pthread_t                 HANDLE
#define pthread_mutex_t           CRITICAL_SECTION

#define pthread_mutex_init(m, a)  InitializeCriticalSection (m)
#define pthread_mutex_destroy(m)  DeleteCriticalSection (m)
#define pthread_mutex_lock(m)     EnterCriticalSection (m)
#define pthread_mutex_unlock(m)   LeaveCriticalSection (m)

/*
 * All libusb callback functions should be marked with the LIBUSB_CALL macro
 * to ensure that they are compiled with the same calling convention as libusb.
 *
 * If the macro isn't available in older libusb versions, we simply define it.
 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif


/* libusb < 1.0.9 doesn't have libusb_handle_events_timeout_completed */
#ifndef HAVE_LIBUSB_HANDLE_EVENTS_TIMEOUT_COMPLETED
#define libusb_handle_events_timeout_completed(ctx, tv, c) \
	libusb_handle_events_timeout(ctx, tv)
#endif

/* two raised to the power of n */
#define TWO_POW(n)	((double)(1ULL<<(n)))

#include "rtl-sdr.h"
#include "tuner_e4k.h"
#include "tuner_fc001x.h"
#include "tuner_fc2580.h"
#include "tuner_r82xx.h"
#include "trace.h"

typedef struct rtlsdr_tuner_iface {
	/* tuner interface */
	int (*init)(void *);
	int (*exit)(void *);
	int (*set_freq)(void *, uint32_t freq /* Hz */);
	int (*set_bw)(void *, int bw /* Hz */, uint32_t *applied_bw /* configured bw in Hz */, int apply /* 1 == configure it!, 0 == deliver applied_bw */);
	int (*set_gain)(void *, int gain /* tenth dB */);
	int (*set_if_gain)(void *, int stage, int gain /* tenth dB */);
	int (*set_gain_mode)(void *, int manual);
	int (*set_i2c_register)(void *, unsigned i2c_register, unsigned data /* byte */, unsigned mask /* byte */ );
	int (*get_i2c_register)(void *, unsigned char *data, int *len, int *strength);
	int (*set_sideband)(void *, int sideband);
	const int *(*get_gains)(int *len);
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
static const int fir_default[][FIR_LEN] = {
// 1.2 MHz
	{-54, -36, -41, -40, -32, -14, 14, 53,	/* 8 bit signed */
	 101, 156, 215, 273, 327, 372, 404, 421},/* 12 bit signed */
// 590 kHz
	{-14,36,37,48,63,81,101,122,144,165,185,203,219,231,239,244},
// 480 kHz
	{82,46,59,72,85,98,112,127,140,153,164,173,182,188,193,195},
};
static const int fir_bw[] = {2400, 1200, 1000, 300};

static int cal_imr = 0;

struct rtlsdr_dev {
	libusb_context *ctx;
	struct libusb_device_handle *devh;
	uint32_t xfer_buf_num;
	uint32_t xfer_buf_len;
	struct libusb_transfer **xfer;
	unsigned char **xfer_buf;
	rtlsdr_read_async_cb_t cb;
	void *cb_ctx;
	enum rtlsdr_async_status async_status;
	int async_cancel;
	int use_zerocopy;
	/* rtl demod context */
	uint32_t rate; /* Hz */
	uint32_t rtl_xtal; /* Hz */
	int fir;
	int direct_sampling;
	/* tuner context */
	enum rtlsdr_tuner tuner_type;
	rtlsdr_tuner_iface_t *tuner;
	uint32_t tun_xtal; /* Hz */
	uint32_t freq; /* Hz */
	uint32_t bw;
	uint32_t offs_freq; /* Hz */
	int corr; /* ppm */
	int gain; /* tenth dB */
	enum rtlsdr_ds_mode direct_sampling_mode;
	uint32_t direct_sampling_threshold; /* Hz */
	struct e4k_state e4k_s;
	struct r82xx_config r82xx_c;
	struct r82xx_priv r82xx_p;
	enum rtlsdr_demod slave_demod;

	/* -cs- Concurrent lock for the periodic reading of I2C registers */
	pthread_mutex_t cs_mutex;

	/* status */
	int dev_lost;
	int driver_active;
	unsigned int xfer_errors;
	int rc_active;
	int verbose;
};

static int rtlsdr_set_if_freq(rtlsdr_dev_t *dev, uint32_t freq);
static int rtlsdr_update_ds(rtlsdr_dev_t *dev, uint32_t freq);
static int rtlsdr_set_spectrum_inversion(rtlsdr_dev_t *dev, int sideband);
static int _rtlsdr_free_async_buffers(rtlsdr_dev_t *dev);

/* generic tuner interface functions, shall be moved to the tuner implementations */
int e4000_init(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	devt->e4k_s.i2c_addr = E4K_I2C_ADDR;
	rtlsdr_get_xtal_freq(devt, NULL, &devt->e4k_s.vco.fosc);
	devt->e4k_s.rtl_dev = dev;
	return e4k_init(&devt->e4k_s);
}
int e4000_exit(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_standby(&devt->e4k_s, 1);
}
int e4000_set_freq(void *dev, uint32_t freq) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_tune_freq(&devt->e4k_s, freq);
}
int e4000_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	if(!apply)
		return 0;
	return e4k_set_bandwidth(&devt->e4k_s, bw, applied_bw, apply);
}
int e4000_set_gain(void *dev, int gain) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_set_gain(&devt->e4k_s, gain);
}
int e4000_set_if_gain(void *dev, int stage, int gain) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_if_gain_set(&devt->e4k_s, (uint8_t)stage, (int8_t)(gain / 10));
}
int e4000_set_gain_mode(void *dev, int manual) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_enable_manual_gain(&devt->e4k_s, manual);
}
int e4000_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask ) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_set_i2c_register(&devt->e4k_s, i2c_register, data, mask);
}
int e4000_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_get_i2c_register(&devt->e4k_s, data, len, strength);
}

int r820t_init(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	devt->r82xx_p.rtl_dev = dev;

	if (devt->tuner_type == RTLSDR_TUNER_R828D) {
		devt->r82xx_c.i2c_addr = R828D_I2C_ADDR;
		devt->r82xx_c.rafael_chip = CHIP_R828D;
	} else {
		devt->r82xx_c.i2c_addr = R820T_I2C_ADDR;
		devt->r82xx_c.rafael_chip = CHIP_R820T;
	}

	rtlsdr_get_xtal_freq(devt, NULL, &devt->r82xx_c.xtal);

	devt->r82xx_c.use_predetect = 0;
	devt->r82xx_c.cal_imr = cal_imr;
	devt->r82xx_p.cfg = &devt->r82xx_c;

	return r82xx_init(&devt->r82xx_p);
}
int r820t_exit(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_standby(&devt->r82xx_p);
}

int r820t_set_freq(void *dev, uint32_t freq) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_freq(&devt->r82xx_p, freq);
}

static int rtlsdr_set_fir(rtlsdr_dev_t *dev, int table);

int r820t_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) {
	int r;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;

	r = r82xx_set_bandwidth(&devt->r82xx_p, bw, applied_bw, apply);
	if(!apply)
		return 0;
	if(r < 0)
		return r;

	r = rtlsdr_set_if_freq(devt, r);
	if (r)
		return r;

	return rtlsdr_set_center_freq(devt, devt->freq);
}

int r820t_set_gain(void *dev, int gain) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_gain(&devt->r82xx_p, gain);
}

int r820t_set_gain_mode(void *dev, int manual) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_gain_mode(&devt->r82xx_p, manual);
}

int r820t_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask ) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_i2c_register(&devt->r82xx_p, i2c_register, data, mask);
}

int r820t_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_get_i2c_register(&devt->r82xx_p, data, len, strength);
}

int r820t_set_sideband(void *dev, int sideband) {
	int r;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;

	r = r82xx_set_sideband(&devt->r82xx_p, sideband);
	if(r < 0)
		return r;
	r = rtlsdr_set_spectrum_inversion(devt, sideband);
	if (r)
		return r;
	return rtlsdr_set_center_freq(devt, devt->freq);
}

/* definition order must match enum rtlsdr_tuner */
static rtlsdr_tuner_iface_t tuners[] = {
	{
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL /* dummy for unknown tuners */
	},
	{
		e4000_init, e4000_exit,
		e4000_set_freq, e4000_set_bw, e4000_set_gain, e4000_set_if_gain,
		e4000_set_gain_mode, e4000_set_i2c_register,
		e4000_get_i2c_register, NULL, e4k_get_gains
	},
	{
		fc0012_init, fc0012_exit,
		fc0012_set_freq, fc001x_set_bw, fc0012_set_gain, NULL,
		fc001x_set_gain_mode, fc001x_set_i2c_register,
		fc0012_get_i2c_register, NULL, fc001x_get_gains
	},
	{
		fc0013_init, fc0013_exit,
		fc0013_set_freq, fc001x_set_bw, fc0013_set_gain, NULL,
		fc001x_set_gain_mode, fc001x_set_i2c_register,
		fc0013_get_i2c_register, NULL, fc001x_get_gains
	},
	{
		fc2580_init, fc2580_exit,
		fc2580_set_freq, fc2580_set_bw, NULL, NULL,
		NULL, fc2580_set_i2c_register,
		fc2580_get_i2c_register, NULL, NULL
	},
	{
		r820t_init, r820t_exit,
		r820t_set_freq, r820t_set_bw, r820t_set_gain, NULL,
		r820t_set_gain_mode, r820t_set_i2c_register,
		r820t_get_i2c_register, r820t_set_sideband, r82xx_get_gains
	},
	{
		r820t_init, r820t_exit,
		r820t_set_freq, r820t_set_bw, r820t_set_gain, NULL,
		r820t_set_gain_mode, r820t_set_i2c_register,
		r820t_get_i2c_register, r820t_set_sideband, r82xx_get_gains
	},
};

typedef struct rtlsdr_dongle {
	uint16_t vid;
	uint16_t pid;
	const char *name;
} rtlsdr_dongle_t;

/*
 * Please add your device here and send a patch to osmocom-sdr@lists.osmocom.org
 */
static rtlsdr_dongle_t known_devices[] = {
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

#define DEFAULT_BUF_NUMBER	15
#define DEFAULT_BUF_LENGTH	(16 * 32 * 512)
/* buf_len:
 * must be multiple of 512 - else it will be overwritten
 * in rtlsdr_read_async() in librtlsdr.c with DEFAULT_BUF_LENGTH (= 16*32 *512 = 512 *512)
 *
 * -> 512*512 -> 1048 ms @ 250 kS  or  81.92 ms @ 3.2 MS (internal default)
 * ->  32*512 ->   65 ms @ 250 kS  or   5.12 ms @ 3.2 MS (new default)
 */


#define DEF_RTL_XTAL_FREQ	28800000
#define MIN_RTL_XTAL_FREQ	(DEF_RTL_XTAL_FREQ - 1000)
#define MAX_RTL_XTAL_FREQ	(DEF_RTL_XTAL_FREQ + 1000)

#define CTRL_IN			(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
#define CTRL_OUT		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)
#define CTRL_TIMEOUT	300
#define BULK_TIMEOUT	0

#define EEPROM_ADDR			0xa0
#define RTL2832_DEMOD_ADDR	0x20
#define	DUMMY_PAGE			0x0a
#define	DUMMY_ADDR			0x01


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
	USB_SYSCTL			= 0x2000, /* USB system control */
	USB_IRQSTAT			= 0x2008, /* SIE interrupt status */
	USB_IRQEN			= 0x200C, /* SIE interrupt enable */
	USB_CTRL			= 0x2010, /* USB control */
	USB_STAT			= 0x2014, /* USB status */
	USB_DEVADDR			= 0x2018, /* USB device address */
	USB_TEST			= 0x201C, /* USB test mode */
	USB_FRAME_NUMBER	= 0x2020, /* frame number */
	USB_FIFO_ADDR		= 0x2028, /* address of SIE FIFO RAM */
	USB_FIFO_CMD		= 0x202A, /* SIE FIFO RAM access command */
	USB_FIFO_DATA		= 0x2030, /* SIE FIFO RAM data */
	/* Endpoint Registers */
	EP0_SETUPA     		= 0x20F8, /* EP 0 setup packet lower byte */
	EP0_SETUPB     		= 0x20FC, /* EP 0 setup packet higher byte */
	USB_EP0_CFG    		= 0x2104, /* EP 0 configure */
	USB_EP0_CTL    		= 0x2108, /* EP 0 control */
	USB_EP0_STAT   		= 0x210C, /* EP 0 status */
	USB_EP0_IRQSTAT		= 0x2110, /* EP 0 interrupt status */
	USB_EP0_IRQEN  		= 0x2114, /* EP 0 interrupt enable */
	USB_EP0_MAXPKT 		= 0x2118, /* EP 0 max packet size */
	USB_EP0_BC     		= 0x2120, /* EP 0 FIFO byte counter */
	USB_EPA_CFG			= 0x2144, /* EP A configure */
	USB_EPA_CTL			= 0x2148, /* EP A control */
	USB_EPA_STAT   	    = 0x214C, /* EP A status */
	USB_EPA_IRQSTAT	    = 0x2150, /* EP A interrupt status */
	USB_EPA_IRQEN  	    = 0x2154, /* EP A interrupt enable */
	USB_EPA_MAXPKT		= 0x2158, /* EP A max packet size */
	USB_EPA_FIFO_CFG	= 0x2160, /* EP A FIFO configure */
	/* Debug Registers */
	USB_PHYTSTDIS      	= 0x2F04, /* PHY test disable */
	USB_TOUT_VAL       	= 0x2F08, /* USB time-out time */
	USB_VDRCTRL        	= 0x2F10, /* UTMI vendor signal control */
	USB_VSTAIN         	= 0x2F14, /* UTMI vendor signal status in */
	USB_VLOADM         	= 0x2F18, /* UTMI load vendor signal status in */
	USB_VSTAOUT        	= 0x2F1C, /* UTMI vendor signal status out */
	USB_UTMI_TST       	= 0x2F80, /* UTMI test */
	USB_UTMI_STATUS    	= 0x2F84, /* UTMI status */
	USB_TSTCTL         	= 0x2F88, /* test control */
	USB_TSTCTL2        	= 0x2F8C, /* test control 2 */
	USB_PID_FORCE      	= 0x2F90, /* force PID */
	USB_PKTERR_CNT     	= 0x2F94, /* packet error counter */
	USB_RXERR_CNT      	= 0x2F98, /* RX error counter */
	USB_MEM_BIST       	= 0x2F9C, /* MEM BIST test */
	USB_SLBBIST        	= 0x2FA0, /* self-loop-back BIST */
	USB_CNTTEST        	= 0x2FA4, /* counter test */
	USB_PHYTST         	= 0x2FC0, /* USB PHY test */
	USB_DBGIDX         	= 0x2FF0, /* select individual block debug signal */
	USB_DBGMUX        	= 0x2FF4  /* debug signal module mux */
};

enum sys_reg {
	/* demod control registers */
	DEMOD_CTL			= 0x3000, /* control register for DVB-T demodulator */
	GPO					= 0x3001, /* output value of GPIO */
	GPI					= 0x3002, /* input value of GPIO */
	GPOE				= 0x3003, /* output enable of GPIO */
	GPD					= 0x3004, /* direction control for GPIO */
	SYSINTE				= 0x3005, /* system interrupt enable */
	SYSINTS				= 0x3006, /* system interrupt status */
	GP_CFG0				= 0x3007, /* PAD configuration for GPIO0-GPIO3 */
	GP_CFG1				= 0x3008, /* PAD configuration for GPIO4 */
	SYSINTE_1			= 0x3009,
	SYSINTS_1			= 0x300A,
	DEMOD_CTL1			= 0x300B,
	IR_SUSPEND			= 0x300C,
	/* I2C master registers */
	I2CCR				= 0x3040, /* I2C clock */
	I2CMCR				= 0x3044, /* I2C master control */
	I2CMSTR				= 0x3048, /* I2C master SCL timing */
	I2CMSR				= 0x304C, /* I2C master status */
	I2CMFR				= 0x3050  /* I2C master FIFO */
};

enum ir_reg {
	/* IR registers */
	IR_RX_BUF			= 0xFC00,
	IR_RX_IE			= 0xFD00,
	IR_RX_IF			= 0xFD01,
	IR_RX_CTRL			= 0xFD02,
	IR_RX_CFG			= 0xFD03,
	IR_MAX_DURATION0	= 0xFD04,
	IR_MAX_DURATION1	= 0xFD05,
	IR_IDLE_LEN0		= 0xFD06,
	IR_IDLE_LEN1		= 0xFD07,
	IR_GLITCH_LEN		= 0xFD08,
	IR_RX_BUF_CTRL		= 0xFD09,
	IR_RX_BUF_DATA		= 0xFD0A,
	IR_RX_BC			= 0xFD0B,
	IR_RX_CLK			= 0xFD0C,
	IR_RX_C_COUNT_L		= 0xFD0D,
	IR_RX_C_COUNT_H		= 0xFD0E,
	IR_SUSPEND_CTRL		= 0xFD10,
	IR_ERR_TOL_CTRL		= 0xFD11,
	IR_UNIT_LEN			= 0xFD12,
	IR_ERR_TOL_LEN		= 0xFD13,
	IR_MAX_H_TOL_LEN	= 0xFD14,
	IR_MAX_L_TOL_LEN	= 0xFD15,
	IR_MASK_CTRL		= 0xFD16,
	IR_MASK_DATA		= 0xFD17,
	IR_RES_MASK_ADDR	= 0xFD18,
	IR_RES_MASK_T_LEN	= 0xFD19
};

enum blocks {
	DEMODB	= 0x0000,
	USBB  	= 0x0100,
	SYSB  	= 0x0200,
	IRB   	= 0x0201,
	TUNB  	= 0x0300,
	ROMB  	= 0x0400,
	IICB 	= 0x0600
};

static inline int rtlsdr_read_array(rtlsdr_dev_t *dev, uint16_t index, uint16_t addr, uint8_t *array, uint16_t len)
{
	return libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, array, len, CTRL_TIMEOUT);
}

static inline int rtlsdr_write_array(rtlsdr_dev_t *dev, uint16_t index, uint16_t addr, uint8_t *array, uint8_t len)
{
	return libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index | 0x10, array, len, CTRL_TIMEOUT);
}

static uint8_t rtlsdr_read_reg(rtlsdr_dev_t *dev, uint16_t index, uint16_t addr)
{
	uint8_t data;

	int r = rtlsdr_read_array(dev, index, addr, &data, 1);
	if (r != 1)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	return data;
}

static int rtlsdr_write_reg(rtlsdr_dev_t *dev, uint16_t index, uint16_t addr, uint16_t val, uint8_t len)
{
	int r;
	unsigned char data[2];

	if (len == 1)
		data[0] = val & 0xff;
	else {
		data[0] = val >> 8;
		data[1] = val & 0xff;
	}
	r = rtlsdr_write_array(dev, index, addr, data, len);
	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	return r;
}

static int rtlsdr_write_reg_mask(rtlsdr_dev_t *dev, uint16_t index, uint16_t addr, uint8_t val,
		uint8_t mask)
{
	uint8_t tmp = rtlsdr_read_reg(dev, index, addr);

	val = (tmp & ~mask) | (val & mask);
	if(tmp == val)
		return 0;
	else
		return rtlsdr_write_reg(dev, index, addr, (uint16_t)val, 1);
}


static uint8_t check_tuner(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg)
{
	uint8_t data = 0;

	rtlsdr_read_array(dev, TUNB, reg << 8 | i2c_addr, &data, 1);
	return data;
}

int rtlsdr_i2c_write_fn(void *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len)
{
	int wr_len;

	if (!dev)
		return -1;

	wr_len = rtlsdr_write_array((rtlsdr_dev_t *)dev, TUNB, reg << 8 | addr, buf, len);

	TRACE_LIBUSB (wr_len);
	TRACE (1, "I2C-bus addr: 0x%02X, reg: 0x%02X, wr_len: %d,\n", addr, reg, wr_len);
	return wr_len;
}

int rtlsdr_i2c_read_fn(void *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len)
{
	int rd_len;

	if (!dev)
		return -1;

	rd_len = rtlsdr_read_array((rtlsdr_dev_t *)dev, TUNB, reg << 8 | addr, buf, len);

	TRACE (1, "I2C-bus addr: 0x%02X, reg: 0x%02X, rd_len: %d\n", addr, reg, rd_len);
	TRACE_LIBUSB (rd_len);
	return rd_len;
}

uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint16_t page, uint16_t addr, uint8_t len)
{
	unsigned char data[2];

	int r = rtlsdr_read_array(dev, page, (addr << 8) | RTL2832_DEMOD_ADDR, data, len);
	if (r != len)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	if (len == 1)
		return data[0];
	else
		return (data[0] << 8) | data[1];
}

int rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
	int r;
	unsigned char data[2];

	addr = (addr << 8) | RTL2832_DEMOD_ADDR;

	if (len == 1)
		data[0] = val & 0xff;
	else {
		data[0] = val >> 8;
		data[1] = val & 0xff;
	}

	r = rtlsdr_write_array(dev, page, addr, data, len);
	if (r != len)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	rtlsdr_demod_read_reg(dev, DUMMY_PAGE, DUMMY_ADDR, 1);

	return (r == len) ? 0 : -1;
}

static int rtlsdr_demod_write_reg_mask(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint8_t val, uint8_t mask)
{
	uint8_t tmp = rtlsdr_demod_read_reg(dev, page, addr, 1);

	val = (tmp & ~mask) | (val & mask);
	if(tmp == val)
		return 0;
	else
		return rtlsdr_demod_write_reg(dev, page, addr, (uint16_t)val, 1);
}

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val)
{
	rtlsdr_write_reg_mask(dev, SYSB, GPO, val << gpio, 1 << gpio);
}

static void rtlsdr_set_gpio_output(rtlsdr_dev_t *dev, uint8_t gpio)
{
	gpio = 1 << gpio;
	rtlsdr_write_reg_mask(dev, SYSB, GPD, ~gpio, gpio);
	rtlsdr_write_reg_mask(dev, SYSB, GPOE, gpio, gpio);
}

static int rtlsdr_set_i2c_repeater(rtlsdr_dev_t *dev, int on)
{
	int r;

	if (on)
		pthread_mutex_lock(&dev->cs_mutex);

	r = rtlsdr_demod_write_reg_mask(dev, 1, 0x01, on ? 0x08 : 0x00, 0x08);

	if (!on)
		pthread_mutex_unlock(&dev->cs_mutex);

    return (r);
}

static int Set2(void *dev)
{
	const uint8_t FM_coe2[6] = {-1, 1, 6, 13, 22, 27};
    unsigned short addr = 0x1F;

	int i;
	int rst = 0;
	for(i=0; i<6; i++)
	{
		rst |= rtlsdr_demod_write_reg(dev, 0, addr, FM_coe2[i], 1);
		addr--;
	}

	return rst;
}

static const char *dump_fir_values (const uint8_t *values, size_t size)
{
   static char ret [4*FIR_LEN+10];
   char *p, *q = ret;
   size_t i;

   for (i = 0; i < size && q < ret + sizeof(ret)-2; i++)
   {
     _itoa (values[i], q, 10);
     p = strchr (q, '\0');
     *p++ = ',';
     q = p;
   }
   q[-1] = '\0';
   return (ret);
}

static int rtlsdr_set_fir(rtlsdr_dev_t *dev, int table)
{
	uint8_t fir[20];
	const int *fir_table;
	int i, r = 0;

	if((dev->fir == table) || (table > 3))
		return 0;

	if(rtlsdr_demod_write_reg_mask(dev, 0, 0x19, (table == 3) ? 0x00 : 0x04, 0x04))
		return -1;

    printf("FIR Filter %d kHz\n", fir_bw[table]);

	dev->fir = table;
	if (dev->offs_freq)
		rtlsdr_set_offset_tuning(dev, 1);
	if(table == 3)
	{
		Set2(dev);
		table = 2;
	}
	fir_table = fir_default[table];

	/* format: int8_t[8] */
	for (i = 0; i < 8; ++i) {
		const int val = fir_table[i];
		if (val < -128 || val > 127) {
			goto fail;
		}
		fir[i] = val;
	}
	/* format: int12_t[8] */
	for (i = 0; i < 8; i += 2) {
		const int val0 = fir_table[8+i];
		const int val1 = fir_table[8+i+1];
		if (val0 < -2048 || val0 > 2047 || val1 < -2048 || val1 > 2047) {
			goto fail;
		}
		fir[8+i*3/2] = val0 >> 4;
		fir[8+i*3/2+1] = (val0 << 4) | ((val1 >> 8) & 0x0f);
		fir[8+i*3/2+2] = val1;
	}

	for (i = 0; i < (int)sizeof(fir); i++) {
		r = rtlsdr_demod_write_reg(dev, 1, 0x1c + i, fir[i], 1);
		if (r)
			goto fail;
	}

	TRACE (1, "FIR Filter %d kHz: FIR-coeff from 'fir_default[%d]':\n"
	       "                  %s\n",
	       fir_bw[table], table, dump_fir_values(fir, sizeof(fir)));
	return 0;

fail:
	TRACE (1, "FIR Filter %d kHz, r: %d, wrong FIR-coeff at 'fir_default[%d][%d]':\n",
	       "                  %s\n",
	       fir_bw[table], r, table, i, dump_fir_values(fir, i));
	return -1;
}


int rtlsdr_get_agc_val(void *dev, int *slave_demod)
{
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;

	*slave_demod = devt->slave_demod;
	return rtlsdr_demod_read_reg(dev, 3, 0x59, 2);
}

int16_t interpolate(int16_t freq, int size, const int16_t *freqs, const int16_t *gains)
{
	int16_t gain = 0;
	int i;

	if(freq < freqs[0])	freq = freqs[0];
	if(freq >= freqs[size - 1])
		gain = gains[size - 1];
	else
		for(i=0; i < (size - 1); ++i)
			if (freq < freqs[i+1])
			{
				gain = gains[i] + ((gains[i+1] - gains[i]) * (freq - freqs[i])) / (freqs[i+1] - freqs[i]);
				break;
			}
	return gain;
}

int rtlsdr_reset_demod(rtlsdr_dev_t *dev)
{
	/* reset demod (bit 3, soft_rst) */
	int r = rtlsdr_demod_write_reg_mask(dev, 1, 0x01, 0x04, 0x04);
	r |= rtlsdr_demod_write_reg_mask(dev, 1, 0x01, 0x00, 0x04);
	return r;
}

static void rtlsdr_init_baseband(rtlsdr_dev_t *dev)
{
	unsigned int i;

	/* initialize USB */
	rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1);
	rtlsdr_write_reg(dev, USBB, USB_EPA_MAXPKT, 0x0002, 2);
	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);

	/* disable IR interrupts in order to avoid SDR sample loss */
	rtlsdr_write_reg(dev, IRB, IR_RX_IE, 0x00, 1);

	/* poweron demod */
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL1, 0x22, 1);
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0xe8, 1);

	rtlsdr_reset_demod(dev);

	/* disable spectrum inversion and adjacent channel rejection */
	rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);

	/* clear both DDC shift and IF frequency registers	*/
	for (i = 0; i < 6; i++)
		rtlsdr_demod_write_reg(dev, 1, 0x16 + i, 0x00, 1);

	dev->fir = -1;
	rtlsdr_set_fir(dev, 0);

	/* enable SDR mode, disable DAGC (bit 5) */
	rtlsdr_demod_write_reg(dev, 0, 0x19, 0x05, 1);

	/* init FSM state-holding register */
	rtlsdr_demod_write_reg(dev, 1, 0x92, 0x00, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x93, 0xf0, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x94, 0x0f, 1);

	/* disable PID filter (enable_PID = 0) */
	rtlsdr_demod_write_reg(dev, 0, 0x61, 0x60, 1);

	/* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
	rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

	//dab dagc_target;     (S,8,7f) when dagc on
	rtlsdr_demod_write_reg(dev, 0, 0x17, 0x11, 1);

	//dagc_gain_set;  (S,8,1f) when dagc off
	rtlsdr_demod_write_reg(dev, 0, 0x18, 0x10, 1);

	/* Enable Zero-IF mode (en_bbin bit), DC cancellation (en_dc_est),
	 * IQ estimation/compensation (en_iq_comp, en_iq_est) */
	rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);

	/* enable In-phase + Quadrature ADC input */
	rtlsdr_demod_write_reg(dev, 0, 0x08, 0xcd, 1);

	/* disable 4.096 MHz clock output on pin TP_CK0 */
	rtlsdr_demod_write_reg(dev, 0, 0x0d, 0x83, 1);

}

static int rtlsdr_deinit_baseband(rtlsdr_dev_t *dev)
{
	int r = 0;

	if (!dev)
		return -1;

	if (dev->tuner && dev->tuner->exit) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->exit(dev); /* deinitialize tuner */
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	/* poweroff demodulator and ADCs */
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0x20, 1);

	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

#ifdef DEBUG
static int rtlsdr_demod_read_regs(rtlsdr_dev_t *dev, uint16_t page, uint16_t addr, unsigned char *data, uint8_t len)
{
	int r = rtlsdr_read_array(dev, page, (addr << 8) | RTL2832_DEMOD_ADDR, data, len);
	if (r != len)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	return r;
}

void print_demod_register(rtlsdr_dev_t *dev, uint8_t page)
{
	unsigned char data[16];
	int i, j, k;
	int reg = 0;

	printf("Page %d\n", page);
	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	for(i=0; i<16; i++)
	{
		printf("%02x: ", reg);
		for(j=0; j<4; j++)
		{
			rtlsdr_demod_read_regs(dev, page, reg+j, data, 4);
			for(k=0; k<4; k++)
				printf("%02x ", data[k]);
			reg += 4;
		}
		printf("\n");
	}
}

void print_rom(rtlsdr_dev_t *dev)
{
    uint8_t data[64];
    int i;
    FILE * pFile;
    int addr = 0;
    int len = sizeof(data);

    printf("write file\n");
    pFile = fopen("rtl2832.bin","wb");
    if (pFile!=NULL)
    {
        for(i=0; i<1024; i++)
        {
            rtlsdr_read_array(dev, ROMB, addr, data, len);
            fwrite (data, 1, sizeof(data), pFile);
            addr += sizeof(data);
        }
        fclose(pFile);
    }
}

void print_usb_register(rtlsdr_dev_t *dev, uint16_t addr)
{
	uint8_t data[16];
	int i, j, index;
    int len = sizeof(data);

	printf("       0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	if(addr < 0x2000) index = ROMB;
	else if(addr < 0x3000) index = USBB;
	else if(addr < 0xfc00) index = SYSB;
	else index = IRB;
	for(i=0; i<16; i++)
	{
		printf("%04x: ", addr);
        rtlsdr_read_array(dev, index, addr, data, len);
		for(j=0; j<16; j++)
			printf("%02x ", data[j]);
        addr += sizeof(data);
		printf("\n");
	}
}
#endif

static int rtlsdr_set_if_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
	uint32_t rtl_xtal;
	int32_t if_freq;
	uint8_t tmp;
	int r;

	if (!dev)
		return -1;

	/* read corrected clock value */
	if (rtlsdr_get_xtal_freq(dev, &rtl_xtal, NULL))
	{
		TRACE (1, "%s(): freq: %, r: %d\n", __FUNCTION__, freq, -2);
		return -2;
	}

	if_freq = ((freq * TWO_POW(22)) / rtl_xtal) * (-1);

	tmp = (if_freq >> 16) & 0x3f;
	r = rtlsdr_demod_write_reg(dev, 1, 0x19, tmp, 1);
	tmp = (if_freq >> 8) & 0xff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0x1a, tmp, 1);
	tmp = if_freq & 0xff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0x1b, tmp, 1);

	TRACE (1, "%s(): freq: %.3f MHz, IF-freq: %.3f MHz, XTAL: %.3f MHz, r: %d\n",
	       __FUNCTION__, (double)freq / 1E6, (double)if_freq / 1E6, (double)rtl_xtal / 1E6, r);
	return r;
}

static inline int rtlsdr_set_spectrum_inversion(rtlsdr_dev_t *dev, int sideband)
{
	return rtlsdr_demod_write_reg_mask(dev, 1, 0x15, sideband ? 0x00 : 0x01, 0x01);
}

static int rtlsdr_set_sample_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
	int r = 0;
	int16_t offs = ppm * (-1) * TWO_POW(24) / 1000000;

	r |= rtlsdr_demod_write_reg(dev, 1, 0x3e, (offs >> 8) & 0x3f, 1);
	r |= rtlsdr_demod_write_reg(dev, 1, 0x3f, offs & 0xff, 1);

	TRACE (1, "%s(): ppm: %d, r: %d\n", __FUNCTION__, ppm, r);
	return r;
}

int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
	int r = 0;

	if (!dev)
		return -1;

	if (rtl_freq > 0 &&
		(rtl_freq < MIN_RTL_XTAL_FREQ || rtl_freq > MAX_RTL_XTAL_FREQ))
		return -2;

	if (rtl_freq > 0 && dev->rtl_xtal != rtl_freq) {
		dev->rtl_xtal = rtl_freq;

		/* update xtal-dependent settings */
		if (dev->rate)
			r = rtlsdr_set_sample_rate(dev, dev->rate);
	}

	if (dev->tun_xtal != tuner_freq) {
		if (0 == tuner_freq)
			dev->tun_xtal = dev->rtl_xtal;
		else
			dev->tun_xtal = tuner_freq;

		/* read corrected clock value into e4k and r82xx structure */
		if (rtlsdr_get_xtal_freq(dev, NULL, &dev->e4k_s.vco.fosc) ||
			rtlsdr_get_xtal_freq(dev, NULL, &dev->r82xx_c.xtal))
			return -3;

		/* update xtal-dependent settings */
		if (dev->freq)
			r = rtlsdr_set_center_freq(dev, dev->freq);
	}
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq, uint32_t *tuner_freq)
{
	if (!dev)
		return -1;

	#define APPLY_PPM_CORR(val,ppm) (((val) * (1.0 + (ppm) / 1e6)))

	if (rtl_freq)
		*rtl_freq = (uint32_t) APPLY_PPM_CORR(dev->rtl_xtal, dev->corr);

	if (tuner_freq)
		*tuner_freq = (uint32_t) APPLY_PPM_CORR(dev->tun_xtal, dev->corr);

	return 0;
}

int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char *manufact, char *product,
							char *serial)
{
	struct libusb_device_descriptor dd;
	libusb_device *device = NULL;
	const int buf_max = 256;
	int r = 0;

	if (!dev || !dev->devh)
		return -1;

	device = libusb_get_device(dev->devh);

	r = libusb_get_device_descriptor(device, &dd);
	if (r < 0)
		return -1;

	if (manufact) {
		memset(manufact, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iManufacturer,
							 (unsigned char *)manufact,
							 buf_max);
	}

	if (product) {
		memset(product, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iProduct,
							 (unsigned char *)product,
							 buf_max);
	}

	if (serial) {
		memset(serial, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iSerialNumber,
							 (unsigned char *)serial,
							 buf_max);
	}

	return 0;
}

int rtlsdr_write_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
	int r = 0;
	int i;
	uint8_t cmd[2];

	if (!dev)
		return -1;

	if ((len + offset) > 256)
	{
	  r = -2;
	  goto quit;
	}

	for (i = 0; i < len; i++) {
		cmd[0] = i + offset;
		r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, cmd, 1);
		r = rtlsdr_read_array(dev, IICB, EEPROM_ADDR, &cmd[1], 1);

		/* only write the byte if it differs */
		if (cmd[1] == data[i])
			continue;

		cmd[1] = data[i];
		r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, cmd, 2);
		if (r != sizeof(cmd))
		{
			r = -3;
			goto quit;
		}

		/* for some EEPROMs (e.g. ATC 240LC02) we need a delay
		 * between write operations, otherwise they will fail */
		usleep(5000);
	}
	r = 0;

quit:
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_read_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
	int r;

	if (!dev)
		r = -1;

	else if ((len + offset) > 256)
		r = -2;

	else r = rtlsdr_read_array(dev, TUNB, offset << 8 | EEPROM_ADDR, data, len);
	if (r < 0)
		r = -3;

	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
	int r = -1;
	if (!dev || !dev->tuner)
		return -1;

	if (dev->direct_sampling_mode > RTLSDR_DS_Q)
		rtlsdr_update_ds(dev, freq);

	if (dev->direct_sampling) {
		r = rtlsdr_set_if_freq(dev, freq);
	} else if (dev->tuner && dev->tuner->set_freq) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_freq(dev, freq - dev->offs_freq);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	if (!r)
		dev->freq = freq;
	else
		dev->freq = 0;

	TRACE (1, "%s(): freq: %.3f MHz, direct_sampling: %d, direct_sampling_mode: %d, r: %d\n",
	       __FUNCTION__, (double)freq / 1E6, dev->direct_sampling, dev->direct_sampling_mode, r);
	return r;
}

uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
	if (!dev)
		return 0;

	return dev->freq;
}

int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
	int r = 0;

	if (!dev)
		return -1;

	if (dev->corr == ppm)
		return -2;

	dev->corr = ppm;

	r |= rtlsdr_set_sample_freq_correction(dev, ppm);

	/* read corrected clock value into e4k and r82xx structure */
	if (rtlsdr_get_xtal_freq(dev, NULL, &dev->e4k_s.vco.fosc) ||
			rtlsdr_get_xtal_freq(dev, NULL, &dev->r82xx_c.xtal))
		return -3;

	if (dev->freq) /* retune to apply new correction value */
		r |= rtlsdr_set_center_freq(dev, dev->freq);

	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
	if (!dev)
		return 0;

	return dev->corr;
}

enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev)
{
	if (!dev)
		return RTLSDR_TUNER_UNKNOWN;

	return dev->tuner_type;
}

int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains)
{
	const int unknown_gains[] = { 0 /* no gain values */ };
	const int *ptr = unknown_gains;
	int len = sizeof(unknown_gains);

	if (!dev)
		return -1;

	if (dev->tuner->get_gains)
		ptr = dev->tuner->get_gains(&len);
	if (!gains) /* no buffer provided, just return the count */
		;
	else
		memcpy(gains, ptr, len);
	return len / sizeof(int);
}

int rtlsdr_set_and_get_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw, uint32_t *applied_bw, int apply_bw )
{
	int  r2, r = 0;

		*applied_bw = 0;		/* unknown */

	if (!dev || !dev->tuner)
		return -1;

	if(!apply_bw)
	{
		if (dev->tuner->set_bw)
			r = dev->tuner->set_bw(dev, bw > 0 ? bw : dev->rate, applied_bw, apply_bw);
		TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
		return r;
	}
	if (dev->tuner->set_bw) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_bw(dev, bw > 0 ? bw : dev->rate, applied_bw, apply_bw);
		rtlsdr_set_i2c_repeater(dev, 0);
		if (r)
			return r;
		dev->bw = bw;
	}

	if(bw == 0)
	{
		if(dev->rate <= 1000000)
			r2 = rtlsdr_set_fir(dev, 2); //1.0 MHz
		else if(dev->rate <= 1200000)
			r2 = rtlsdr_set_fir(dev, 1); //1.2 MHz
		else
			r2 = rtlsdr_set_fir(dev, 0); //2.4 MHz
		TRACE (1, "%s(): r2: %d\n", __FUNCTION__, r2);
	}
	else
	{
		if(bw <= 300000)
			r2 = rtlsdr_set_fir(dev, 3); //0.3 MHz
		else if(bw <= 1000000)
			r2 = rtlsdr_set_fir(dev, 2); //1.0 MHz
		else if((bw <= 1500000) && (*applied_bw >= 2000000))
			r2 = rtlsdr_set_fir(dev, 1); //1.2 MHz
		else
			r2 = rtlsdr_set_fir(dev, 0); //2.4 MHz
		TRACE (1, "%s(): r2: %d\n", __FUNCTION__, r2);
	}

	return r;
}

int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw )
{
	uint32_t applied_bw = 0;
	int r = rtlsdr_set_and_get_tuner_bandwidth(dev, bw, &applied_bw, 1 /* =apply_bw */ );

	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}


int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain)
{
	int r = 0;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_gain) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_gain((void *)dev, gain);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	if (!r)
		dev->gain = gain;
	else
		dev->gain = 0;

	TRACE (1, "%s(): gain: %d.%d dB, r: %d\n", __FUNCTION__, dev->gain/10, dev->gain % 10, r);
	return r;
}

int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
	if (!dev)
		return 0;

	return dev->gain;
}

int rtlsdr_set_tuner_if_gain(rtlsdr_dev_t *dev, int stage, int gain)
{
	int r = 0;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_if_gain) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_if_gain(dev, stage, gain);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int mode)
{
	int r = 0;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_gain_mode) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_gain_mode((void *)dev, mode);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	TRACE (1, "%s(): mode: %d (%s), r: %d\n", __FUNCTION__, mode, mode == 0 ? "auto" : "manual", r);
	return r;
}

int rtlsdr_set_tuner_sideband(rtlsdr_dev_t *dev, int sideband)
{
	int r = 0;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_sideband) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_sideband((void *)dev, sideband);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_tuner_i2c_register(rtlsdr_dev_t *dev, unsigned i2c_register, unsigned mask /* byte */, unsigned data /* byte */ )
{
	int r = 0;

	if (!dev || !dev->tuner)
		return -1;
	if (dev->tuner->set_i2c_register) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_i2c_register((void *)dev, i2c_register, data, mask);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_get_tuner_i2c_register(rtlsdr_dev_t *dev, unsigned char *data, int *len, int *strength)
{
	int r = 0;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->get_i2c_register) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->get_i2c_register((void *)dev, data, len, strength);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_dithering(rtlsdr_dev_t *dev, int dither)
{
	int r = 0;
	if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
		(dev->tuner_type == RTLSDR_TUNER_R828D)) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = r82xx_set_dither(&dev->r82xx_p, dither);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t samp_rate)
{
	int r = 0;
	uint16_t tmp;
	uint32_t rsamp_ratio, real_rsamp_ratio;
	double real_rate;

	if (!dev)
		return -1;

	/* check if the rate is supported by the resampler */
	if ((samp_rate <= 225000) || (samp_rate > 4096000) ||
		 ((samp_rate > 300000) && (samp_rate <= 900000))) {
		fprintf(stderr, "Invalid sample rate: %u Hz\n", samp_rate);
		return -EINVAL;
	}

	rsamp_ratio = (dev->rtl_xtal * TWO_POW(22)) / samp_rate;
	rsamp_ratio &= 0x0ffffffc;

	real_rsamp_ratio = rsamp_ratio | ((rsamp_ratio & 0x08000000) << 1);
	real_rate = (dev->rtl_xtal * TWO_POW(22)) / real_rsamp_ratio;

	if ( ((double)samp_rate) != real_rate )
		fprintf(stderr, "Exact sample rate is: %f Hz\n", real_rate);

	dev->rate = (uint32_t)real_rate;

	tmp = (rsamp_ratio >> 16);
	r |= rtlsdr_demod_write_reg(dev, 1, 0x9f, tmp, 2);
	tmp = rsamp_ratio & 0xffff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0xa1, tmp, 2);

	r |= rtlsdr_set_sample_freq_correction(dev, dev->corr);

	r |= rtlsdr_reset_demod(dev);

	/* recalculate offset frequency if offset tuning is enabled */
	if (dev->offs_freq)
		rtlsdr_set_offset_tuning(dev, 1);

	TRACE (1, "%s(): real_rate: %.3f MS/s,r: %d\n", __FUNCTION__, real_rate / 1E6, r);
	return r;
}

uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
	if (!dev)
		return 0;

	return dev->rate;
}

int rtlsdr_set_testmode(rtlsdr_dev_t *dev, int on)
{
	if (!dev)
		return -1;

	int r = rtlsdr_demod_write_reg_mask(dev, 0, 0x19, on ? 0x02 : 0x00, 0x02);
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on)
{
	if (!dev)
		return -1;
	int r = rtlsdr_demod_write_reg_mask(dev, 0, 0x19, on ? 0x20 : 0x00, 0x20);
	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	return r;
}

int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on)
{
	int r = 0;

	if (!dev)
		return -1;

	if (on) {
		if (dev->tuner && dev->tuner->exit) {
			rtlsdr_set_i2c_repeater(dev, 1);
			r = dev->tuner->exit(dev);
			rtlsdr_set_i2c_repeater(dev, 0);
		}

		/* disable Zero-IF mode */
		r |= rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1a, 1);

		/* disable spectrum inversion */
		r |= rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);

		/* only enable In-phase ADC input */
		r |= rtlsdr_demod_write_reg(dev, 0, 0x08, 0x4d, 1);

		/* swap I and Q ADC, this allows to select between two inputs */
		r |= rtlsdr_demod_write_reg(dev, 0, 0x06, (on > 1) ? 0x90 : 0x80, 1);

		fprintf(stderr, "Enabled direct sampling mode, input %i\n", on);
		dev->direct_sampling = on;
	} else {
		if (dev->tuner && dev->tuner->init) {
			rtlsdr_set_i2c_repeater(dev, 1);
			r |= dev->tuner->init(dev);
			rtlsdr_set_i2c_repeater(dev, 0);
		}

		if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
				(dev->tuner_type == RTLSDR_TUNER_R828D)) {
			r |= rtlsdr_set_if_freq(dev, R82XX_IF_FREQ);

			/* enable spectrum inversion */
			r |= rtlsdr_demod_write_reg(dev, 1, 0x15, 0x01, 1);
		} else {
			r |= rtlsdr_set_if_freq(dev, 0);

			/* enable In-phase + Quadrature ADC input */
			r |= rtlsdr_demod_write_reg(dev, 0, 0x08, 0xcd, 1);

			/* Enable Zero-IF mode */
			r |= rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);
		}

		/* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
		//r |= rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

		fprintf(stderr, "Disabled direct sampling mode\n");
		dev->direct_sampling = 0;
	}

	r |= rtlsdr_set_center_freq(dev, dev->freq);

	return r;
}

int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	return dev->direct_sampling;
}

int rtlsdr_set_ds_mode(rtlsdr_dev_t *dev, enum rtlsdr_ds_mode mode, uint32_t freq_threshold)
{
	uint32_t center_freq;
	if (!dev)
		return -1;

	center_freq = rtlsdr_get_center_freq(dev);
	if ( !center_freq )
		return -2;

	if (!freq_threshold) {
		switch(dev->tuner_type) {
		default:
		case RTLSDR_TUNER_UNKNOWN:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_E4000:	freq_threshold = 50*1000000; break; /* E4K_FLO_MIN_MHZ */
		case RTLSDR_TUNER_FC0012:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_FC0013:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_FC2580:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_R820T:	freq_threshold = 24000000; break; /* ~ */
		case RTLSDR_TUNER_R828D:	freq_threshold = 28800000; break; /* no idea!!! */
		}
	}

	dev->direct_sampling_mode = mode;
	dev->direct_sampling_threshold = freq_threshold;

	if (mode <= RTLSDR_DS_Q)
		rtlsdr_set_direct_sampling(dev, mode);

	return rtlsdr_set_center_freq(dev, center_freq);
}

static int rtlsdr_update_ds(rtlsdr_dev_t *dev, uint32_t freq)
{
	int new_ds = 0;
	int curr_ds = rtlsdr_get_direct_sampling(dev);
	if ( curr_ds < 0 )
		return -1;

	switch (dev->direct_sampling_mode) {
	default:
	case RTLSDR_DS_IQ:		break;
	case RTLSDR_DS_I:		new_ds = 1; break;
	case RTLSDR_DS_Q:		new_ds = 2; break;
	case RTLSDR_DS_I_BELOW:	new_ds = (freq < dev->direct_sampling_threshold) ? 1 : 0; break;
	case RTLSDR_DS_Q_BELOW:	new_ds = (freq < dev->direct_sampling_threshold) ? 2 : 0; break;
	}

	if ( curr_ds != new_ds )
		return rtlsdr_set_direct_sampling(dev, new_ds);

	return 0;
}

int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on)
{
	int r = 0;
	int bw;

	if (!dev)
		return -1;

	if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
			(dev->tuner_type == RTLSDR_TUNER_R828D))
		return -2;

	if (dev->direct_sampling)
		return -3;

	if(on)
	{
		dev->offs_freq = dev->rate / 2;
		if((dev->offs_freq < 400000) || ((rtlsdr_demod_read_reg(dev, 0, 0x19, 1) & 0x04) == 0))
			dev->offs_freq = 400000;
	}
	else
		dev->offs_freq = 0;
	r |= rtlsdr_set_if_freq(dev, dev->offs_freq);

	if ((dev->tuner && dev->tuner->set_bw) && (dev->bw < (2 * dev->offs_freq))) {
		uint32_t applied_bw = 0;
		rtlsdr_set_i2c_repeater(dev, 1);
		if (on)
			bw = 2 * dev->offs_freq;
		else if (dev->bw > 0)
			bw = dev->bw;
		else
			bw = dev->rate;
		dev->tuner->set_bw(dev, bw, &applied_bw, 1);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	if (dev->freq > dev->offs_freq)
		r |= rtlsdr_set_center_freq(dev, dev->freq);

	return r;
}

int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	return (dev->offs_freq) ? 1 : 0;
}


static rtlsdr_dongle_t *find_known_device(uint16_t vid, uint16_t pid)
{
	unsigned int i;
	rtlsdr_dongle_t *device = NULL;

	for (i = 0; i < ARRAY_SIZE(known_devices); i++ ) {
		if (known_devices[i].vid == vid && known_devices[i].pid == pid) {
			TRACE (1, "Found VID: 0x%04X PID: 0x%04X -> \"%s\"\n",
					vid, pid, known_devices[i].name);
			device = &known_devices[i];
			break;
		}
	}

	return device;
}

uint32_t rtlsdr_get_device_count(void)
{
	int i;
	libusb_context *ctx;
	libusb_device **list;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	ssize_t cnt;

	const struct libusb_version* version = libusb_get_version();
	TRACE (1, "Using libusb v%d.%d.%d.%d\n", version->major, version->minor, version->micro, version->nano);

	if(libusb_init(&ctx) < 0)
		return 0;

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	return device_count;
}

const char *rtlsdr_get_device_name(uint32_t index)
{
	int i;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	rtlsdr_dongle_t *device = NULL;
	uint32_t device_count = 0;
	ssize_t cnt;

	if(libusb_init(&ctx) < 0)
		return "";

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		device = find_known_device(dd.idVendor, dd.idProduct);

		if (device) {
			if (index == device_count)
				break;
			device_count++;
		}
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	if (device)
		return device->name;
	else
		return "";
}

int rtlsdr_get_device_usb_strings(uint32_t index, char *manufact,
					 char *product, char *serial)
{
	int r = -2;
	int i;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	rtlsdr_dev_t devt;
	uint32_t device_count = 0;
	ssize_t cnt;

	r = libusb_init(&ctx);
	if(r < 0)
		return r;

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++)
	{
		libusb_get_device_descriptor(list[i], &dd);
		if (find_known_device(dd.idVendor, dd.idProduct))
		{
			if (index == device_count)
			{
				r = libusb_open(list[i], &devt.devh);
				if (!r)
				{
					r = rtlsdr_get_usb_strings(&devt, manufact, product, serial);
					libusb_close(devt.devh);
				}
				break;
			}
			device_count++;
		}
	}

	TRACE (1, "rtlsdr_get_device_usb_strings(): cnt: %u, device_count: %u,\n"
	          "       manufact: %s, product: %s, serial: %s\n", cnt, device_count,
	       (manufact && *manufact) ? manufact : "<None>",
	       (product  && *product)  ? product  : "<None>",
	       (serial   && *serial)   ? serial   : "<None>");

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);
	return r;
}

int rtlsdr_get_index_by_serial(const char *serial)
{
	int i, cnt, r;
	char str[256];

	if (!serial)
		return -1;

	cnt = rtlsdr_get_device_count();

	if (!cnt)
		return -2;

	for (i = 0; i < cnt; i++) {
		r = rtlsdr_get_device_usb_strings(i, NULL, NULL, str);
		if (!r && !strcmp(serial, str))
			return i;
	}

	return -3;
}

int rtlsdr_open(rtlsdr_dev_t **out_dev, uint32_t index)
{
	int r;
	int i;
	libusb_device **list;
	rtlsdr_dev_t *dev;
	libusb_device *device = NULL;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	uint8_t reg;
	ssize_t cnt;

	dev = calloc(1, sizeof(rtlsdr_dev_t));
	if (!dev)
		return -ENOMEM;

	r = libusb_init(&dev->ctx);
	if(r < 0) {
		free(dev);
		return -1;
	}

	libusb_set_option(dev->ctx, LIBUSB_OPTION_LOG_LEVEL, 1);

	pthread_mutex_init(&dev->cs_mutex, NULL);

	dev->dev_lost = 1;

	cnt = libusb_get_device_list(dev->ctx, &list);

	for (i = 0; i < cnt; i++) {
		device = list[i];

		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct)) {

			if (index == device_count)
				break;
			device_count++;
		}
		device = NULL;
	}

	if (!device) {
		r = -1;
		goto err;
	}

	r = libusb_open(device, &dev->devh);
	if (r < 0) {
		libusb_free_device_list(list, 1);
		fprintf(stderr, "usb_open error %d\n", r);
		if(r == LIBUSB_ERROR_ACCESS)
			fprintf(stderr, "Please fix the device permissions, e.g. "
			"by installing the udev rules file rtl-sdr.rules\n");
		goto err;
	}
	libusb_free_device_list(list, 1);

	if (libusb_kernel_driver_active(dev->devh, 0) == 1) {
		dev->driver_active = 1;

#ifdef DETACH_KERNEL_DRIVER
		if (!libusb_detach_kernel_driver(dev->devh, 0)) {
			fprintf(stderr, "Detached kernel driver\n");
		} else {
			fprintf(stderr, "Detaching kernel driver failed!");
			goto err;
		}
#else
		fprintf(stderr, "\nKernel driver is active, or device is "
				"claimed by second instance of librtlsdr."
				"\nIn the first case, please either detach"
				" or blacklist the kernel module\n"
				"(dvb_usb_rtl28xxu), or enable automatic"
				" detaching at compile time.\n\n");
#endif
	}

	r = libusb_claim_interface(dev->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		goto err;
	}

	dev->rtl_xtal = DEF_RTL_XTAL_FREQ;

	/* perform a dummy write, if it fails, reset the device */

	TRACE (1, "Calling rtlsdr_write_reg() for a dummy write.\n");

	if (rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1) < 0) {
		fprintf(stderr, "Resetting device...\n");
		libusb_reset_device(dev->devh);
	}

	TRACE (1, "Calling rtlsdr_init_baseband().\n");
	rtlsdr_init_baseband(dev);

	dev->dev_lost = 0;

	/* Probe tuners */
	rtlsdr_set_i2c_repeater(dev, 1);

	reg = check_tuner(dev, E4K_I2C_ADDR, E4K_CHECK_ADDR);
	if (reg == E4K_CHECK_VAL) {
		fprintf(stderr, "Found Elonics E4000 tuner\n");
		dev->tuner_type = RTLSDR_TUNER_E4000;
		goto found;
	}

	reg = check_tuner(dev, FC001X_I2C_ADDR, FC001X_CHECK_ADDR);
	if (reg == FC0013_CHECK_VAL) {
		fprintf(stderr, "Found Fitipower FC0013 tuner\n");
		dev->tuner_type = RTLSDR_TUNER_FC0013;
		goto found;
	}

	reg = check_tuner(dev, R820T_I2C_ADDR, R82XX_CHECK_ADDR);
	if (reg == R82XX_CHECK_VAL) {
		fprintf(stderr, "Found Rafael Micro R820T tuner\n");
		dev->tuner_type = RTLSDR_TUNER_R820T;
		goto found;
	}

	reg = check_tuner(dev, R828D_I2C_ADDR, R82XX_CHECK_ADDR);
	if (reg == R82XX_CHECK_VAL) {
		fprintf(stderr, "Found Rafael Micro R828D tuner\n");
		dev->tuner_type = RTLSDR_TUNER_R828D;
		goto found;
	}

	/* initialise GPIOs */
	rtlsdr_set_gpio_output(dev, 4);

	/* reset tuner before probing */
	rtlsdr_set_gpio_bit(dev, 4, 1);
	rtlsdr_set_gpio_bit(dev, 4, 0);

	reg = check_tuner(dev, FC2580_I2C_ADDR, FC2580_CHECK_ADDR);
	if ((reg & 0x7f) == FC2580_CHECK_VAL) {
		fprintf(stderr, "Found FCI 2580 tuner\n");
		dev->tuner_type = RTLSDR_TUNER_FC2580;
		goto found;
	}

	reg = check_tuner(dev, FC001X_I2C_ADDR, FC001X_CHECK_ADDR);
	if (reg == FC0012_CHECK_VAL) {
		fprintf(stderr, "Found Fitipower FC0012 tuner\n");
		rtlsdr_set_gpio_output(dev, 6);
		dev->tuner_type = RTLSDR_TUNER_FC0012;
	}

found:
	/* use the rtl clock value by default */
	dev->tun_xtal = dev->rtl_xtal;
	dev->tuner = &tuners[dev->tuner_type];

	switch (dev->tuner_type) {
	case RTLSDR_TUNER_FC2580:
		dev->tun_xtal = FC2580_XTAL_FREQ;
		break;
	case RTLSDR_TUNER_E4000:
		rtlsdr_demod_write_reg(dev, 1, 0x12, 0x5a, 1);//DVBT_DAGC_TRG_VAL
		rtlsdr_demod_write_reg(dev, 1, 0x02, 0x40, 1);//DVBT_AGC_TARG_VAL_0
		rtlsdr_demod_write_reg(dev, 1, 0x03, 0x5a, 1);//DVBT_AGC_TARG_VAL_8_1
		rtlsdr_demod_write_reg(dev, 1, 0xc7, 0x30, 1);//DVBT_AAGC_LOOP_GAIN
		rtlsdr_demod_write_reg(dev, 1, 0x04, 0xd0, 1);//DVBT_LOOP_GAIN2_3_0
		rtlsdr_demod_write_reg(dev, 1, 0x05, 0xbe, 1);//DVBT_LOOP_GAIN2_4
		rtlsdr_demod_write_reg(dev, 1, 0xc8, 0x18, 1);//DVBT_LOOP_GAIN3
		rtlsdr_demod_write_reg(dev, 1, 0x06, 0x35, 1);//DVBT_VTOP1
		rtlsdr_demod_write_reg(dev, 1, 0xc9, 0x21, 1);//DVBT_VTOP2
		rtlsdr_demod_write_reg(dev, 1, 0xca, 0x21, 1);//DVBT_VTOP3
		rtlsdr_demod_write_reg(dev, 1, 0xcb, 0x00, 1);//DVBT_KRF1
		rtlsdr_demod_write_reg(dev, 1, 0x07, 0x40, 1);//DVBT_KRF2
		rtlsdr_demod_write_reg(dev, 1, 0xcd, 0x10, 1);//DVBT_KRF3
		rtlsdr_demod_write_reg(dev, 1, 0xce, 0x10, 1);//DVBT_KRF4
		rtlsdr_demod_write_reg(dev, 0, 0x11, 0xe9d4, 2);//DVBT_AD7_SETTING
		rtlsdr_demod_write_reg(dev, 1, 0xe5, 0xf0, 1);//DVBT_EN_GI_PGA
		rtlsdr_demod_write_reg(dev, 1, 0xd9, 0x00, 1);//DVBT_THD_LOCK_UP
		rtlsdr_demod_write_reg(dev, 1, 0xdb, 0x00, 1);//DVBT_THD_LOCK_DW
		rtlsdr_demod_write_reg(dev, 1, 0xdd, 0x14, 1);//DVBT_THD_UP1
		rtlsdr_demod_write_reg(dev, 1, 0xde, 0xec, 1);//DVBT_THD_DW1
		rtlsdr_demod_write_reg(dev, 1, 0xd8, 0x0c, 1);//DVBT_INTER_CNT_LEN
		rtlsdr_demod_write_reg(dev, 1, 0xe6, 0x02, 1);//DVBT_GI_PGA_STATE
		rtlsdr_demod_write_reg(dev, 1, 0xd7, 0x09, 1);//DVBT_EN_AGC_PGA
		rtlsdr_demod_write_reg(dev, 0, 0x10, 0x49, 1);//DVBT_REG_GPO
		rtlsdr_demod_write_reg(dev, 0, 0x0d, 0x85, 1);//DVBT_REG_MON,DVBT_REG_MONSEL
 		rtlsdr_demod_write_reg(dev, 0, 0x13, 0x02, 1);
		break;
	case RTLSDR_TUNER_FC0012:
	case RTLSDR_TUNER_FC0013:
		rtlsdr_demod_write_reg(dev, 1, 0x12, 0x5a, 1);//DVBT_DAGC_TRG_VAL
		rtlsdr_demod_write_reg(dev, 1, 0x02, 0x40, 1);//DVBT_AGC_TARG_VAL_0
		rtlsdr_demod_write_reg(dev, 1, 0x03, 0x5a, 1);//DVBT_AGC_TARG_VAL_8_1
		rtlsdr_demod_write_reg(dev, 1, 0xc7, 0x2c, 1);//DVBT_AAGC_LOOP_GAIN
		rtlsdr_demod_write_reg(dev, 1, 0x04, 0xcc, 1);//DVBT_LOOP_GAIN2_3_0
		rtlsdr_demod_write_reg(dev, 1, 0x05, 0xbe, 1);//DVBT_LOOP_GAIN2_4
		rtlsdr_demod_write_reg(dev, 1, 0xc8, 0x16, 1);//DVBT_LOOP_GAIN3
		rtlsdr_demod_write_reg(dev, 1, 0x06, 0x35, 1);//DVBT_VTOP1
		rtlsdr_demod_write_reg(dev, 1, 0xc9, 0x21, 1);//DVBT_VTOP2
		rtlsdr_demod_write_reg(dev, 1, 0xca, 0x21, 1);//DVBT_VTOP3
		rtlsdr_demod_write_reg(dev, 1, 0xcb, 0x00, 1);//DVBT_KRF1
		rtlsdr_demod_write_reg(dev, 1, 0x07, 0x40, 1);//DVBT_KRF2
		rtlsdr_demod_write_reg(dev, 1, 0xcd, 0x10, 1);//DVBT_KRF3
		rtlsdr_demod_write_reg(dev, 1, 0xce, 0x10, 1);//DVBT_KRF4
		rtlsdr_demod_write_reg(dev, 0, 0x11, 0xe9bf, 2);//DVBT_AD7_SETTING
		rtlsdr_demod_write_reg(dev, 1, 0xe5, 0xf0, 1);//DVBT_EN_GI_PGA
		rtlsdr_demod_write_reg(dev, 1, 0xd9, 0x00, 1);//DVBT_THD_LOCK_UP
		rtlsdr_demod_write_reg(dev, 1, 0xdb, 0x00, 1);//DVBT_THD_LOCK_DW
		rtlsdr_demod_write_reg(dev, 1, 0xdd, 0x11, 1);//DVBT_THD_UP1
		rtlsdr_demod_write_reg(dev, 1, 0xde, 0xef, 1);//DVBT_THD_DW1
		rtlsdr_demod_write_reg(dev, 1, 0xd8, 0x0c, 1);//DVBT_INTER_CNT_LEN
		rtlsdr_demod_write_reg(dev, 1, 0xe6, 0x02, 1);//DVBT_GI_PGA_STATE
		rtlsdr_demod_write_reg(dev, 1, 0xd7, 0x09, 1);//DVBT_EN_AGC_PGA
		break;
	case RTLSDR_TUNER_R828D:
		dev->tun_xtal = R828D_XTAL_FREQ;

		/* power off slave demod on GPIO0 to reset CXD2837ER */
		rtlsdr_set_gpio_bit(dev, 0, 0);
		rtlsdr_write_reg_mask(dev, SYSB, GPOE, 0x00, 0x01);
		usleep(50000);

		/* power on slave demod on GPIO0 */
		rtlsdr_set_gpio_bit(dev, 0, 1);
		rtlsdr_set_gpio_output(dev, 0);

		/* check slave answers */
		reg = check_tuner(dev, MN8847X_I2C_ADDR, MN8847X_CHECK_ADDR);
		if (reg == MN88472_CHIP_ID)
		{
			fprintf(stderr, "Found Panasonic MN88472 demod\n");
			dev->slave_demod = SLAVE_DEMOD_MN88472;
			goto demod_found;
		}
		if (reg == MN88473_CHIP_ID)
		{
			fprintf(stderr, "Found Panasonic MN88473 demod\n");
			dev->slave_demod = SLAVE_DEMOD_MN88473;
			goto demod_found;
		}
		reg = check_tuner(dev, CXD2837_I2C_ADDR, CXD2837_CHECK_ADDR);
		if (reg == CXD2837ER_CHIP_ID)
		{
			fprintf(stderr, "Found Sony CXD2837ER demod\n");
			dev->slave_demod = SLAVE_DEMOD_CXD2837ER;
			goto demod_found;
		}
		reg = check_tuner(dev, SI2168_I2C_ADDR, SI2168_CHECK_ADDR);
		if (reg == SI2168_CHIP_ID)
		{
			fprintf(stderr, "Found Silicon Labs SI2168 demod\n");
			dev->slave_demod = SLAVE_DEMOD_SI2168;
		}

demod_found:
		if (dev->slave_demod) //switch off dvbt2 demod
		{
			rtlsdr_write_reg(dev, SYSB, GPO, 0x88, 1);
			rtlsdr_write_reg(dev, SYSB, GPOE, 0x9d, 1);
			rtlsdr_write_reg(dev, SYSB, GPD, 0x02, 1);
		}

		/* fall-through */
	case RTLSDR_TUNER_R820T:
		TRACE (1, "Writing DVBT_DAGC_TRG_VAL.\n");

		rtlsdr_demod_write_reg(dev, 1, 0x12, 0x5a, 1);//DVBT_DAGC_TRG_VAL
		rtlsdr_demod_write_reg(dev, 1, 0x02, 0x40, 1);//DVBT_AGC_TARG_VAL_0
		rtlsdr_demod_write_reg(dev, 1, 0x03, 0x80, 1);//DVBT_AGC_TARG_VAL_8_1
		rtlsdr_demod_write_reg(dev, 1, 0xc7, 0x24, 1);//DVBT_AAGC_LOOP_GAIN
		rtlsdr_demod_write_reg(dev, 1, 0x04, 0xcc, 1);//DVBT_LOOP_GAIN2_3_0
		rtlsdr_demod_write_reg(dev, 1, 0x05, 0xbe, 1);//DVBT_LOOP_GAIN2_4
		rtlsdr_demod_write_reg(dev, 1, 0xc8, 0x14, 1);//DVBT_LOOP_GAIN3
		rtlsdr_demod_write_reg(dev, 1, 0x06, 0x35, 1);//DVBT_VTOP1
		rtlsdr_demod_write_reg(dev, 1, 0xc9, 0x21, 1);//DVBT_VTOP2
		rtlsdr_demod_write_reg(dev, 1, 0xca, 0x21, 1);//DVBT_VTOP3
		rtlsdr_demod_write_reg(dev, 1, 0xcb, 0x00, 1);//DVBT_KRF1
		rtlsdr_demod_write_reg(dev, 1, 0x07, 0x40, 1);//DVBT_KRF2
		rtlsdr_demod_write_reg(dev, 1, 0xcd, 0x10, 1);//DVBT_KRF3
		rtlsdr_demod_write_reg(dev, 1, 0xce, 0x10, 1);//DVBT_KRF4
		rtlsdr_demod_write_reg(dev, 0, 0x11, 0xf4, 1);//DVBT_AD7_SETTING
		/* disable Zero-IF mode */
		rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1a, 1);
		/* only enable In-phase ADC input */
		rtlsdr_demod_write_reg(dev, 0, 0x08, 0x4d, 1);
		// the R82XX use 3.57 MHz IF for the DVB-T 6 MHz mode
		rtlsdr_set_if_freq(dev, R82XX_IF_FREQ);
		/* enable spectrum inversion */
		rtlsdr_demod_write_reg(dev, 1, 0x15, 0x01, 1);
		break;
	case RTLSDR_TUNER_UNKNOWN:
		fprintf(stderr, "No supported tuner found\n");
		rtlsdr_set_direct_sampling(dev, 1);
		break;
	default:
		break;
	}

	if (dev->tuner->init)
		r = dev->tuner->init(dev);

	int r2 = rtlsdr_set_i2c_repeater(dev, 0);
	TRACE (1, "rtlsdr_set_i2c_repeater(0): r: %d.\n", r2);

	*out_dev = dev;

	return 0;
err:
	if (dev) {
		if (dev->devh)
			libusb_close(dev->devh);

		if (dev->ctx)
			libusb_exit(dev->ctx);

		free(dev);
	}
	return r;
}

int rtlsdr_close(rtlsdr_dev_t *dev)
{
	int r;

	if (!dev)
		return -1;

	/* automatic de-activation of bias-T */
	rtlsdr_set_bias_tee(dev, 0);

	if(!dev->dev_lost) {
		/* block until all async operations have been completed (if any) */
		while (RTLSDR_INACTIVE != dev->async_status) {
			usleep(1000);
		}

		rtlsdr_deinit_baseband(dev);
	}
	pthread_mutex_destroy(&dev->cs_mutex);

	r = libusb_release_interface(dev->devh, 0);

#ifdef DETACH_KERNEL_DRIVER
	if (dev->driver_active) {
		if (!libusb_attach_kernel_driver(dev->devh, 0))
			fprintf(stderr, "Reattached kernel driver\n");
		else
			fprintf(stderr, "Reattaching kernel driver failed!\n");
	}
#endif

#if 1 // !! moved from rtlsdr_read_async()
	_rtlsdr_free_async_buffers(dev);
#endif

	libusb_close(dev->devh);

	libusb_exit(dev->ctx);

	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);
	free(dev);

	return 0;
}

int rtlsdr_reset_buffer(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	int r = 0; // libusb_reset_device(dev->devh);

	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);
	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x0000, 2);

	TRACE (1, "%s(): r: %d\n", __FUNCTION__, r);

	return r;
}

int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
	if (!dev)
		return -1;

	int r = libusb_bulk_transfer(dev->devh, 0x81, buf, len, n_read, BULK_TIMEOUT);
	TRACE (3, "libusb_bulk_transfer (0x%02X, %p, %d) -> %d\n", 0x81, buf, len, r);
	return r;
}

static const char *get_transfer_name (int st)
{
  static const char *libusb_transfer_names[] = {
    "LIBUSB_TRANSFER_COMPLETED",
    "LIBUSB_TRANSFER_ERROR",
    "LIBUSB_TRANSFER_TIMED_OUT",
    "LIBUSB_TRANSFER_CANCELLED",
    "LIBUSB_TRANSFER_STALL",
    "LIBUSB_TRANSFER_NO_DEVICE",
    "LIBUSB_TRANSFER_OVERFLOW",
  };
  static char buf[20];

  if (st >= 0 && st < ARRAY_SIZE(libusb_transfer_names))
     return libusb_transfer_names[st];
  return _itoa (st, buf, 10);
}

static void LIBUSB_CALL _libusb_callback(struct libusb_transfer *xfer)
{
	rtlsdr_dev_t *dev = (rtlsdr_dev_t *)xfer->user_data;

	TRACE (2, "_libusb_callback(): xfer->status = %s\n", get_transfer_name(xfer->status));

	if (LIBUSB_TRANSFER_COMPLETED == xfer->status) {
		if (dev->cb)
			dev->cb(xfer->buffer, xfer->actual_length, dev->cb_ctx);
		libusb_submit_transfer(xfer); /* resubmit transfer */
		dev->xfer_errors = 0;
	} else if (LIBUSB_TRANSFER_CANCELLED != xfer->status) {
#if !defined(_WIN32) || 1
		if (LIBUSB_TRANSFER_ERROR == xfer->status)
			dev->xfer_errors++;

		if (dev->xfer_errors >= dev->xfer_buf_num ||
			LIBUSB_TRANSFER_NO_DEVICE == xfer->status) {
#endif
			dev->dev_lost = 1;
			rtlsdr_cancel_async(dev);
			fprintf(stderr, "cb transfer status: %d, "
				"canceling...\n", xfer->status);
#if !defined(_WIN32) || 1
		}
#endif
	}
}

static int _rtlsdr_alloc_async_buffers(rtlsdr_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return -1;

	if (!dev->xfer) {
		dev->xfer = malloc(dev->xfer_buf_num *
					 sizeof(struct libusb_transfer *));

		for(i = 0; i < dev->xfer_buf_num; ++i)
			dev->xfer[i] = libusb_alloc_transfer(0);
	}

	if (dev->xfer_buf)
		return -2;

	dev->xfer_buf = calloc(1, dev->xfer_buf_num * sizeof(unsigned char *));

#if defined (__linux__) && LIBUSB_API_VERSION >= 0x01000105
	fprintf(stderr, "Allocating %d zero-copy buffers\n", dev->xfer_buf_num);

	dev->use_zerocopy = 1;
	for (i = 0; i < dev->xfer_buf_num; ++i) {
		dev->xfer_buf[i] = libusb_dev_mem_alloc(dev->devh, dev->xfer_buf_len);

		if (!dev->xfer_buf[i]) {
			fprintf(stderr, "Failed to allocate zero-copy "
					"buffer for transfer %d\nFalling "
					"back to buffers in userspace\n", i);

			dev->use_zerocopy = 0;
			break;
		}
	}

	/* zero-copy buffer allocation failed (partially or completely)
	 * we need to free the buffers again if already allocated */
	if (!dev->use_zerocopy) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i])
				libusb_dev_mem_free(dev->devh,
						    dev->xfer_buf[i],
						    dev->xfer_buf_len);
		}
	}
#endif

	/* no zero-copy available, allocate buffers in userspace */
	if (!dev->use_zerocopy) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			dev->xfer_buf[i] = malloc(dev->xfer_buf_len);

			if (!dev->xfer_buf[i])
				return -ENOMEM;
		}
	}

	return 0;
}

static int _rtlsdr_free_async_buffers(rtlsdr_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return -1;

	if (dev->xfer) {
		for(i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer[i]) {
				libusb_free_transfer(dev->xfer[i]);
			}
		}

		free(dev->xfer);
		dev->xfer = NULL;
	}

	if (dev->xfer_buf) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i]) {
				if (dev->use_zerocopy) {
#if defined (__linux__) && LIBUSB_API_VERSION >= 0x01000105
					libusb_dev_mem_free(dev->devh,
							    dev->xfer_buf[i],
							    dev->xfer_buf_len);
#endif
				} else {
					free(dev->xfer_buf[i]);
				}
			}
		}

		free(dev->xfer_buf);
		dev->xfer_buf = NULL;
	}

	return 0;
}

int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx,
				uint32_t buf_num, uint32_t buf_len)
{
	unsigned int i;
	int r = 0;
	int r2 = -100;
	struct timeval tv = { 1, 0 };
	struct timeval zerotv = { 0, 0 };
	enum rtlsdr_async_status next_status = RTLSDR_INACTIVE;

	if (!dev) {
		TRACE (1, "%s(): dev == NULL!\n", __FUNCTION__);
		return -1;
	}

	if (RTLSDR_INACTIVE != dev->async_status) {
		TRACE (1, "%s(): dev->async_status != RTLSDR_INACTIVE\n", __FUNCTION__);
		return -2;
	}

	dev->async_status = RTLSDR_RUNNING;
	dev->async_cancel = 0;

	dev->cb = cb;
	dev->cb_ctx = ctx;

	if (buf_num > 0)
		dev->xfer_buf_num = buf_num;
	else
		dev->xfer_buf_num = DEFAULT_BUF_NUMBER;

	if (buf_len > 0 && buf_len % 512 == 0) /* len must be multiple of 512 */
		dev->xfer_buf_len = buf_len;
	else
		dev->xfer_buf_len = DEFAULT_BUF_LENGTH;

	_rtlsdr_alloc_async_buffers(dev);

	for(i = 0; i < dev->xfer_buf_num; ++i) {
		libusb_fill_bulk_transfer(dev->xfer[i],
						dev->devh,
						0x81,
						dev->xfer_buf[i],
						dev->xfer_buf_len,
						_libusb_callback,
						(void *)dev,
						BULK_TIMEOUT);

		r = libusb_submit_transfer(dev->xfer[i]);
		if (r < 0) {
			fprintf(stderr, "Failed to submit transfer %i\n"
					"Please increase your allowed "
					"usbfs buffer size with the "
					"following command:\n"
					"echo 0 > /sys/module/usbcore"
					"/parameters/usbfs_memory_mb\n", i);
			dev->async_status = RTLSDR_CANCELING;
			break;
		}
	}

	while (RTLSDR_INACTIVE != dev->async_status) {
		r = libusb_handle_events_timeout_completed(dev->ctx, &tv,
								 &dev->async_cancel);
		if (r < 0) {
			TRACE (1, "handle_events returned: %d/%s\n", r, libusb_strerror(r));
			if (r == LIBUSB_ERROR_INTERRUPTED) /* stray signal */
				continue;
			break;
		}

		if (RTLSDR_CANCELING == dev->async_status) {
			next_status = RTLSDR_INACTIVE;

			if (!dev->xfer) {
				TRACE (1, "%s(): !dev->xfer\n", __FUNCTION__);
				break;
			}

			for(i = 0; i < dev->xfer_buf_num; ++i) {
				if (!dev->xfer[i])
					continue;

				if (LIBUSB_TRANSFER_CANCELLED !=
						dev->xfer[i]->status) {
					r = libusb_cancel_transfer(dev->xfer[i]);
					/* handle events after canceling
					 * to allow transfer status to
					 * propagate */
					r2 = libusb_handle_events_timeout_completed(dev->ctx,
												 &zerotv, NULL);
					if (r < 0)
						continue;

					next_status = RTLSDR_CANCELING;
				}
			}

			if (dev->dev_lost || RTLSDR_INACTIVE == next_status) {
				/* handle any events that still need to
				 * be handled before exiting after we
				 * just cancelled all transfers */
				libusb_handle_events_timeout_completed(dev->ctx,
											 &zerotv, NULL);
				break;
			}
		}
	}

#if 0 // !!
	_rtlsdr_free_async_buffers(dev);
#endif

	dev->async_status = next_status;
	TRACE (1, "%s(): dev->async_status: %d, r: %d, r2: %d\n", __FUNCTION__, dev->async_status, r, r2);

	return r;
}

int rtlsdr_cancel_async(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	/* if streaming, try to cancel gracefully */
	if (RTLSDR_RUNNING == dev->async_status) {
		dev->async_status = RTLSDR_CANCELING;
		dev->async_cancel = 1;
		return 0;
	}

	/* if called while in pending state, change the state forcefully */
#if 0
	if (RTLSDR_INACTIVE != dev->async_status) {
		dev->async_status = RTLSDR_INACTIVE;
		return 0;
	}
#endif
	return -2;
}

uint32_t rtlsdr_get_tuner_clock(void *dev)
{
	uint32_t tuner_freq;

	if (!dev)
		return 0;

	/* read corrected clock value */
	if (rtlsdr_get_xtal_freq((rtlsdr_dev_t *)dev, NULL, &tuner_freq))
		return 0;

	return tuner_freq;
}

/* Infrared (IR) sensor support
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
	uint8_t val;
	uint8_t mask;
};

int rtlsdr_ir_query(rtlsdr_dev_t *d, uint8_t *buf, size_t buf_len)
{
	int ret = -1;
	size_t i;
	uint32_t len;
	static const struct rtl28xxu_reg_val refresh_tab[] = {
		{IRB, IR_RX_IF,			0x03},
		{IRB, IR_RX_BUF_CTRL,	0x80},
		{IRB, IR_RX_CTRL,		0x80},
	};

	/* init remote controller */
	if (!d->rc_active) {
		//fprintf(stderr, "initializing remote controller\n");
		static const struct rtl28xxu_reg_val_mask init_tab[] = {
			{USBB, DEMOD_CTL1,		0x00, 0x04},
			{USBB, DEMOD_CTL1,		0x00, 0x08},
			{USBB, USB_CTRL,		0x20, 0x20},
			{USBB, GPD,				0x00, 0x08},
			{USBB, GPOE,			0x08, 0x08},
			{USBB, GPO,				0x08, 0x08},
			{IRB, IR_MAX_DURATION0,	0xd0, 0xff},
			{IRB, IR_MAX_DURATION1,	0x07, 0xff},
			{IRB, IR_IDLE_LEN0,		0xc0, 0xff},
			{IRB, IR_IDLE_LEN1,		0x00, 0xff},
			{IRB, IR_GLITCH_LEN,	0x03, 0xff},
			{IRB, IR_RX_CLK,		0x09, 0xff},
			{IRB, IR_RX_CFG,		0x1c, 0xff},
			{IRB, IR_MAX_H_TOL_LEN,	0x1e, 0xff},
			{IRB, IR_MAX_L_TOL_LEN,	0x1e, 0xff},
			{IRB, IR_RX_CTRL,		0x80, 0xff},
		};

		for (i = 0; i < ARRAY_SIZE(init_tab); i++) {
			ret = rtlsdr_write_reg_mask(d, init_tab[i].block, init_tab[i].reg,
					init_tab[i].val, init_tab[i].mask);
			if (ret < 0) {
				fprintf(stderr, "write %zu reg %d %.4x %.2x %.2x failed\n", i, init_tab[i].block,
						init_tab[i].reg, init_tab[i].val, init_tab[i].mask);
				goto err;
			}
		}
		d->rc_active = 1;
		//fprintf(stderr, "rc active\n");
	}
	// TODO: option to ir disable

	buf[0] = rtlsdr_read_reg(d, IRB, IR_RX_IF);
	if (buf[0] != 0x83) {
		if (buf[0] == 0 || // no IR signal
			// also observed: 0x82, 0x81 - with lengths 1, 5, 0.. unknown, sometimes occurs at edges
			// "IR not ready"? causes a -7 timeout if we read
			buf[0] == 0x82 || buf[0] == 0x81) {
			// graceful exit
		}
		else
			fprintf(stderr, "read IR_RX_IF unexpected: %.2x\n", buf[0]);

		ret = 0;
		goto exit;
	}

	buf[0] = rtlsdr_read_reg(d, IRB, IR_RX_BC);
	len = buf[0];
	//fprintf(stderr, "read IR_RX_BC len=%d\n", len);

	if (len > buf_len) {
		//fprintf(stderr, "read IR_RX_BC too large for buffer, %lu > %lu\n", buf_len, buf_len);
		goto exit;
	}
	if ((len != 6) && (len < 70)) //message is not complete
	{
		uint32_t len2;
		usleep((72-len)*1000);
		len2 = rtlsdr_read_reg(d, IRB, IR_RX_BC);
#if 0
		if(len != len2)
			printf("len=%d, len2=%d\n", len, len2);
#endif
		if(len2 > len)
			len = len2;
	}

	if(len > 0)
	{
		/* read raw code from hw */
		ret = rtlsdr_read_array(d, IRB, IR_RX_BUF, buf, len);
		if (ret < 0)
			goto err;
		/* let hw receive new code */
		for (i = 0; i < ARRAY_SIZE(refresh_tab); i++) {
			ret = rtlsdr_write_reg(d, refresh_tab[i].block, refresh_tab[i].reg,
					refresh_tab[i].val, 1);
			if (ret < 0)
				goto err;
		}
	}

	// On success return length
	ret = len;

exit:
	return ret;
err:
	printf("failed=%d\n", ret);
	return ret;
}

int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t *dev, int gpio, int on)
{
	if (!dev)
		return -1;

	rtlsdr_set_gpio_output(dev, gpio);
	rtlsdr_set_gpio_bit(dev, gpio, on);

	return 0;
}

int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on)
{
	if (dev->slave_demod)
		return 0;
	else
		return rtlsdr_set_bias_tee_gpio(dev, 0, on);
}

int rtlsdr_set_opt_string(rtlsdr_dev_t *dev, const char *opts, int verbose)
{
	char * optStr, * optPart;
	int retAll = 0;

	if (!dev)
		return -1;

	optStr = strdup(opts);
	if (!optStr)
		return -1;

	optPart = strtok(optStr, ":,");
	while (optPart)
	{
		int ret = 0;
		if (!strcmp(optPart, "verbose")) {
			fprintf(stderr, "\nrtlsdr_set_opt_string(): parsed option verbose\n");
			dev->verbose = 1;
		}
		else if (!strncmp(optPart, "f=", 2)) {
			uint32_t freq = (uint32_t)atol(optPart + 2);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed frequency %u\n", (unsigned)freq);
			ret = rtlsdr_set_center_freq(dev, freq);
		}
		else if (!strncmp(optPart, "bw=", 3)) {
			uint32_t bw = (uint32_t)( atol(optPart +3) * 1000 );
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed bandwidth %u\n", (unsigned)bw);
			ret = rtlsdr_set_tuner_bandwidth(dev, bw);
		}
		else if (!strncmp(optPart, "agc=", 4)) {
			int manual = 1 - atoi(optPart +4);	/* invert logic */
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed tuner gain mode, manual=%d\n", manual);
			ret = rtlsdr_set_tuner_gain_mode(dev, manual);
		}
		else if (!strncmp(optPart, "gain=", 5)) {
			int gain = atoi(optPart +5);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed tuner gain = %d /10 dB\n", gain);
			ret = rtlsdr_set_tuner_gain(dev, gain);
		}
		else if (!strncmp(optPart, "dagc=", 5)) {
			int on = atoi(optPart +5);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed rtl/digital gain mode %d\n", on);
			ret = rtlsdr_set_agc_mode(dev, on);
		}
		else if (!strncmp(optPart, "ds=", 3)) {
			int on = atoi(optPart +3);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed direct sampling mode %d\n", on);
			ret = rtlsdr_set_direct_sampling(dev, on);
		}
		else if (!strncmp(optPart, "t=", 2) || !strncmp(optPart, "T=", 2)) {
			int on = atoi(optPart +2);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed bias tee %d\n", on);
			ret = rtlsdr_set_bias_tee(dev, on);
		}
		else {
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed unknown option '%s'\n", optPart);
			ret = -1;  /* unknown option */
		}
		if (verbose)
			fprintf(stderr, "  application of option returned %d\n", ret);
		if (ret < 0)
			retAll = ret;
		optPart = strtok(NULL, ":,");
	}

	free(optStr);
	return retAll;
}

void rtlsdr_cal_imr(const int val)
{
	cal_imr = val;
}
