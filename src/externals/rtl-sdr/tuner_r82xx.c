/*
 * Rafael Micro R820T/R828D driver
 *
 * Copyright (C) 2013 Mauro Carvalho Chehab <mchehab@redhat.com>
 * Copyright (C) 2013 Steve Markgraf <steve@steve-m.de>
 *
 * This driver is a heavily modified version of the driver found in the
 * Linux kernel:
 * http://git.linuxtv.org/linux-2.6.git/history/HEAD:/drivers/media/tuners/r820t.c
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

#include "rtl-sdr.h"
#include "rtlsdr_i2c.h"
#include "tuner_r82xx.h"

#define MHZ(x)		((x)*1000*1000)

#define HF 1
#define VHF 2
#define UHF 3

extern int16_t interpolate(int16_t freq, int size, const int16_t *freqs, const int16_t *gains);
extern int rtlsdr_get_agc_val(void *dev, int *slave_demod);
extern uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint16_t page, uint16_t addr, uint8_t len);

/*
Read registers

Reg		Bitmap	Symbol			Description
------------------------------------------------------------------------------------
R0		[7:0]	CHIP_ID			reference check point for read mode: 0x96
0x00
------------------------------------------------------------------------------------
R1		[7:6]					10
0x01	[5:0]	ADC				Analog-Digital Converter for detector 3
------------------------------------------------------------------------------------
R2		[7]						1
0x02	[6]		VCO_LOCK		0: PLL has not locked, 1: PLL has locked
		[5:0]	VCO_INDICATOR	VCO band
	 							000000: min (1.75 GHz), 111111: max (3.6 GHz)
------------------------------------------------------------------------------------
R3		[7:4]	RF_INDICATOR	Mixer gain
0x03							0: Lowest, 15: Highest
		[3:0]					LNA gain
								0: Lowest, 15: Highest
------------------------------------------------------------------------------------
R4		[5:4]					vco_fine_tune
0x04	[3:0]					fil_cal_code
------------------------------------------------------------------------------------
*/
/*
Write registers

Reg		Bitmap	Symbol			Description
------------------------------------------------------------------------------------
R0		[7]		sw_tfq			tracking filter Q enhance
0x00							0: off, 1:on
		[6]		sw_ltsum		ltsum pin out switch
								0: off, 1:on
		[5:4]	pw_ltsum		ltsum current
								00: highest, 01: high, 10: low, 11: lowest
		[3]		ltsum			lt sum LPF function
								0: finger 12, 1: finger 6
		[2:0]	ltsum			lt sum HPF function
								000: finger 3
								001: finger 7
								010: finger 11
								011: finger 15
								100: finger 11
								101: finger 15
								110: finger 19
								111: finger 23
------------------------------------------------------------------------------------
R1		[7:6]	low_gain0		Air LNA, maximum gain feedback low gain
0x01							00: 1.8k, 01: 1.25k, 10: 1.25k, 11: 1k
		[5]		low_gain		LNA low gain
								0: off, 1:on
		[4]		pwd_150			lna impedence 150
								0: low gain only, 1: on
		[3]		more_cap		lna low pass (air2) more cap
								0: off, 1:on
		[2]		sel_rfa			air in diplexer
								0: lna1, 1: lna2
		[1]		lna_gain		lna manual gain lsb(new)
		[0]		lna_15db_enb	lna 1.5db enable
								0: off, 1:on
------------------------------------------------------------------------------------
R2		[7]		g30_31			g30_31
0x02							0: off, 1: on
		[6]		vcomp			channel filter Q control
								1: low Q, 0: high Q
		[5]		vgacomp_15db	vga comp range
								0: 3dB, 1: 1.5dB
		[4]		comp_enb		echo compensation
								0: off, 1: on
		[3]		b0_en			echo compensation mode
								0: 3dB, 1: 1.5dB
		[2:0]	atten			Loop Through attenuation
	 							0~7
------------------------------------------------------------------------------------
R3		[2]		sw_res1			pll reference spur reduce
0x03							0: off, 1: on
		[1]		pw_vcoauto		pll vco power auto
								0: off, 1: on
		[0]		cp_ix2			pll cp x 2
								0: off, 1: on
------------------------------------------------------------------------------------
R4		[7:6]	lt_hp			new loop through high pass strength
0x04							00: off, 01: low, 10: low, 11: high
		[5:4]	lt_att			new loop through attenuation
								00: off, 01: low, 10: higw, 11: highest
		[3:2]	pwd150_rf2		airin2 pwd150
								00: off, 01: low, 10: low, 11: high
		[1:0]	pwd150_rf1		airin1 pwd150
								00: off, 01: low, 10: low, 11: high
------------------------------------------------------------------------------------
R5		[7:6] 	LOOP_THROUGH	Loop through ON/OFF
0x05							0: on, 1: off
		[6]		pwd_cable1		Cable1 LNA (R828D pin 2)
								0:off, 1:on
		[5] 	pwd_air			Air in LNA
								0:on, 1:off
		[4] 	LNA_GAIN_MODE	LNA gain mode switch
								0: auto, 1: manual
		[3:0] 	LNA_GAIN		LNA manual gain control
								15: max gain, 0: min gain
------------------------------------------------------------------------------------
R6		[7] 	pwd_pdect_lna	LNA power detector (wide band) on/off
0x06							0: on, 1: off
		[6] 	pwd_pdect_mix	LNA power detector(narrow band) on/off
								0: off, 1: on
		[5] 	FILT_GAIN		Filter gain 3db
								0:0db, 1:+4db (>4MHz bw) or +8db (<4MHz bw)
		[4]		v6Mhz			Mixer Filter 6MHz function
								0: off, 1: on
		[3]		pwd_cable2		Cable2 LNA (R828D pin 3)
								0: off, 1: on
		[2:0]	PW_LNA			LNA power control
								000: max, 111: min
------------------------------------------------------------------------------------
R7		[7]		IMG_R			Mixer Sideband
0x07							0: lower, 1: upper
		[6] 	PWD_MIX			Mixer power
								0:off, 1:on
		[5] 	PW0_MIX			Mixer current control
								0:max current, 1:normal current
		[4] 	MIXGAIN_MODE	Mixer gain mode
								0:manual mode, 1:auto mode
		[3:0] 	MIX_GAIN		Mixer manual gain control
								0000->min, 1111->max
------------------------------------------------------------------------------------
R8		[7] 	PWD_AMP			Mixer buffer power on/off
0x08							0: off, 1:on
		[6] 	PW0_AMP			Mixer buffer current setting
								0: high current, 1: low current
		[5]						0: Q, 1: I
		[4:0] 	IMR_G			Image Gain Adjustment
								0: min, 31: max
------------------------------------------------------------------------------------
R9		[7] 	PWD_IFFILT		IF Filter power on/off
0x09							0: filter on, 1: off
		[6] 	PW1_IFFILT		IF Filter current
								0: high current, 1: low current
		[5]						0: Q, 1: I
		[4:0] 	IMR_P			Image Phase Adjustment
								0: min, 31: max
------------------------------------------------------------------------------------
R10		[7] 	PWD_FILT		Filter power on/off
0x0A							0: channel filter off, 1: on
		[6:5] 	PW_FILT			Filter power control
								00: highest power, 11: lowest power
		[4]		FILT_Q			channel filter Q control
								0: low Q, 1: high Q
		[3:0] 	FILT_CODE		Filter bandwidth manual fine tune
								0000 Widest, 1111 narrowest
------------------------------------------------------------------------------------
R11		[7:5] 	FILT_BW			Filter bandwidth manual course tune
0x0B							000: widest
								010 or 001: middle
								111: narrowest
		[4]		CAL_TRIGGER		channel filter auto calibration start triggering
								1: start
		[3:0] 	HP_COR			High pass filter corner control
								0000: highest
								1111: lowest
------------------------------------------------------------------------------------
R12		[7]		pwd_adc			adc power control
								0: on, 1: off
0x0C	[6] 	PWD_VGA			VGA power control
								0: vga power off, 1: vga power on
		[5]		pw0_vga			0: vga max power, 1: vga min power
		[4] 	VGA_MODE		VGA GAIN manual / pin selector
								1: IF vga gain controlled by vagc pin
								0: IF vga gain controlled by vga_code[3:0]
		[3:0] 	VGA_CODE		IF vga manual gain control
								0000: -12.0 dB
								1111: +40.5 dB; -3.5dB/step
------------------------------------------------------------------------------------
R13		[7:4]	LNA_VTHH		LNA agc power detector voltage threshold high setting
0x0D							1111: 1.94 V
								0000: 0.34 V, ~0.1 V/step
		[3:0] 	LNA_VTHL		LNA agc power detector voltage threshold low setting
								1111: 1.94 V
								0000: 0.34 V, ~0.1 V/step
------------------------------------------------------------------------------------
R14 	[7:4] 	MIX_VTH_H		MIXER agc power detector voltage threshold high setting
0x0E							1111: 1.94 V
								0000: 0.34 V, ~0.1 V/step
		[3:0] 	MIX_VTH_L		MIXER agc power detector voltage threshold low setting
								1111: 1.94 V
								0000: 0.34 V, ~0.1 V/step
------------------------------------------------------------------------------------
R15		[7]		FLT_EXT_WIDEST	filter extension widest
0x0F							0: off, 1: on
		[6:5]	ldo5vh			0: LDO 2.9V, 1:LDO 3.0V
		[4] 	CLK_OUT_ENB		Clock out pin control
								0: clk output on, 1: off
		[3]		clk_ring_enb	0: ring pll reference clock on, 1: off
		[2]		clk_filt_enb	0: channel filter calibration clock off, 1: on
		[1] 	CLK_AGC_ENB		AGC clk control
								0: internal agc clock on, 1: off
		[0]		GPIO			gpio (R828D pin 1)
								0: 0, 1: 1
------------------------------------------------------------------------------------
R16		[7:5] 	SEL_DIV			PLL to Mixer divider number control
0x10							000: mixer in = vco out / 2
								001: mixer in = vco out / 4
								010: mixer in = vco out / 8
								011: mixer in = vco out / 16
								100: mixer in = vco out / 32
								101: mixer in = vco out / 64
		[4] 	REFDIV			PLL Reference frequency Divider
								0 -> fref=xtal_freq
								1 -> fref=xta_freql / 2 (for Xtal >24MHz)
		[3]		sw_xtal			xtal swing control
								0: High, 1: Low
		[2]		agc_clk_s2		1
		[1:0] 	CAPX			Internal xtal cap setting
								00->no cap
								01->10pF
								10->20pF
								11->30pF
------------------------------------------------------------------------------------
R17		[7:6] 	PW_LDO_A		PLL analog low drop out regulator switch
0x11							00: off
								01: 2.1V
								10: 2.0V
								11: 1.9V
		[5:3]	pw_cp			charge pump current control
								000: 0.7 mA
								001: 0.6 mA
								010: 0.5 mA
								011: 0.4 mA
								100: 0.3 mA
								101: 0.2 mA
								110: 0.1 mA
								111: Auto
		[2]		pwd_bias		PLL Divider Power
								0:  on, 1: off
		[1:0]	pw_hfd			PLL BiasHF
								00: 100uA
								01:  50uA
								10: 200uA
								11: 150uA
------------------------------------------------------------------------------------
R18		[7:5] 	pw_vco			VCO Core Power
0x12							000: 0-Max
								110: low ... 110: 6-Min
								111: 7-OFF
		[4]		pw_dither		sigma delta modulator dither function switch
								0: on, 1: off
		[3]		PW_SDM			sigma delta modulator switch
								0: Enable frac pll, 1: Disable frac pll
		[2:1]	offset			charge pump offset current
								00: No offset
								01: 30uA
								10: 60uA
								11: 90uA
		[0]		p_0406			CP Reference Voltage
								0: 0.4-1.4V
								1: 0.4-1.2V
------------------------------------------------------------------------------------
R19		[7]		pw_atune		PLL Auto Tune Clock
0x13							0: on, 1: off
		[6]		vco_control		VCO state auto/manual control switch
								0: auto (VCO autotune)
								1:  manual (select VCO & VCO bank by sel_vco[5:0])
		[5:0]	sel_vco			VCO bank
	 							000000: min (1.75 GHz), 111111: max (3.6 GHz)
------------------------------------------------------------------------------------
R20		[7:6] 	SI2C			PLL integer divider number input Si2c
0x14							Nint=4*Ni2c+Si2c+13
								PLL divider number Ndiv = (Nint + Nfra)*2
		[5:0] 	NI2C			PLL integer divider number input Ni2c
------------------------------------------------------------------------------------
R21		[7:0] 	SDM_IN[8:1]		PLL fractional divider number input SDM[16:1]
0x15							Nfra=SDM_IN[16]*2^-1+SDM_IN[15]*2^-2+...
R22		[7:0] 	SDM_IN[16:9]	+SDM_IN[2]*2^-15+SDM_IN[1]*2^-16
0x16
------------------------------------------------------------------------------------
R23		[7:6] 	PW_LDO_D		PLL digital low drop out regulator supply current switch
0x17							00: 1.8V,8mA
								01: 1.8V,4mA
								10: 2.0V,8mA
								11: OFF
		[5:4]	pw45			prescale 45 current
								00: 100uA, 01:   50uA
								10: 200u, 11: 150u
		[3] 	OPEN_D			Open drain (R828D pin 4)
								0: High-Z, 1: Low-Z
		[2:1]	pw_IQ			IQ generator current control
								00: Div_min, Buf_min
								01: Div_mid, Buf_max
								10: Div_mid, Buf_min
								11: Div_max, Buf_max
		[0] 	pwd_IQ			IQ generator power
								0: on, 1: off
------------------------------------------------------------------------------------
R24		[7]		pw_ringout		RingPLL Test VCO Output Enable
								0: off, 1: on
		[6]		ring_cp_current	RingPLL charge pump curren
								0: 15u, 1: 150u
		[5]		ring_div[0]		ring_div bit 0
0x18	[4] 	ring_pwd		RingPLL power
								0: off, 1:on
		[3:0]	n_ring			RingPLL integer divider number control
								ring_vco = (16+n_ring)*8*pll_ref, n_ring = 9...14
------------------------------------------------------------------------------------
R25		[7] 	PWD_RFFILT		RF Filter power
0x19							0: off, 1:on
		[6:5]	POLYFIL_CUR		RF poly filter current
								11: min
		[4] 	SW_AGC			Switch agc_pin
								0:agc=agc_in
								1:agc=agc_in2 (R828D Pin 18)
		[3:2]	ring_pw			RingPLL VCO power
								00: off, 01: off, 10: min, 11: max
		[1:0]	ring_div[2:1]	cal_freq = ring_vco / divisor
								000: ring_freq = ring_vco / 4
								001: ring_freq = ring_vco / 6
								010: ring_freq = ring_vco / 8
								011: ring_freq = ring_vco / 12
								100: ring_freq = ring_vco / 16
								101: ring_freq = ring_vco / 24
								110: ring_freq = ring_vco / 32
								111: ring_freq = ring_vco / 48
------------------------------------------------------------------------------------
R26		[7:6] 	RF_MUX_POLY		Tracking Filter switch
0x1A							00: TF on
								01: Bypass
		[5:4]					AGC clk
								00: 300ms, 01: 300ms, 10: 80ms, 11: 20ms
		[3:2]	PLL_AUTO_CLK	PLL auto tune clock rate
								00: 128 kHz
								01: 32 kHz
								10: 8 kHz
		[1:0]	RFFILT			RF FILTER to reject 3rd harmonic
								00: highest band
								01: med band
								10: low band
------------------------------------------------------------------------------------
R27		[7:4]	TF_NCH			0000 highest corner for LPNF
0x1B							1111 lowest corner for LPNF
		[3:0]	TF_LP			0000 highest corner for LPF
								1111 lowest corner for LPF
------------------------------------------------------------------------------------
R28		[7:4]	PDET3_GAIN		Power detector 3 (Mixer) TOP(take off point) control
0x1C							0: Highest, 15: Lowest
		[3]						discharge mode
								0: on
		[2]		pdect_mode		LNA power detector mode switch
								0: normal, 1: low discharge mode
		[1]		from_ring		Mixer input source select
								0: rf in, 1: ring pll in
		[0]		pwd_vco_out		PLL VCO Output Enable
								0: OFF, 1: ON
------------------------------------------------------------------------------------
R29		[7:6]	dectbw			LNA narrow band power detector bw switch
0x1D							0: highest bw, ..., 3: lowest bw
		[5:3]	PDET1_GAIN		Power detector 1 (LNA wide band) TOP(take off point) control
								0: Highest, 7: Lowest
		[2:0] 	PDET2_GAIN		Power detector 2 (LNA narrow band) TOP(take off point) control
								0: Highest, 7: Lowest
------------------------------------------------------------------------------------
R30		[7]		sw_pdect		det_cap2 input switch
0x1E							0: for mixer AGC operation
								1: for ADC readout operation
	 	[6]		FILTER_EXT		Filter extension under weak signal
								0: Disable, 1: Enable
		[5]		att13_ext		channel filter extension start point
								0: extension @ LNA max, 1: extension @ LNA max-1
		[4:0]	PDET_CLK		Power detector timing control (LNA discharge current)
	 							11111: max, 00000: min
------------------------------------------------------------------------------------
R31		[7]		LT_ATT			Loop through attenuation
0x1F							0: Enable, 1: Disable
		[6:2]					10000
		[1:0]					pw_ring
								0: -5dB, 1: 0dB, 2: -8dB, 3: -3dB
------------------------------------------------------------------------------------
*/

/*
 * Static constants
 */

/* Those initial values start from REG_SHADOW_START */
static uint8_t r82xx_init_array[] = {
	0x80,	//Reg 0x05
	0x12, 	//Reg 0x06
	0x70,	//Reg 0x07
	0xc0, 	//Reg 0x08
	0x40, 	//Reg 0x09
	0xdb, 	//Reg 0x0a
	0x6b,	//Reg 0x0b
	0xf0, 	//Reg 0x0c
	0x53, 	//Reg 0x0d
	0x53, 	//Reg 0x0e
	0x68,	//Reg 0x0f
	0x64, 	//Reg 0x10
	0xbb, 	//Reg 0x11
	0x80, 	//Reg 0x12
	0x00,	//Reg 0x13
	0x0f, 	//Reg 0x14
	0x00, 	//Reg 0x15
	0xc0, 	//Reg 0x16
	0x30,	//Reg 0x17
	0x48, 	//Reg 0x18
	0xec, 	//Reg 0x19
	0x60, 	//Reg 0x1a
	0x00,	//Reg 0x1b
	0x24,	//Reg 0x1c
	0xdd, 	//Reg 0x1d
	0x0e, 	//Reg 0x1e
	0x40	//Reg 0x1f
};

/* Tuner frequency ranges */
static const struct r82xx_freq_range freq_ranges[] = {
	{
	/* .freq = */			0,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0xdf,	/* R27[7:0]  band2,band0 */
	}, {
	/* .freq = */			50,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0xbe,	/* R27[7:0]  band4,band1  */
	}, {
	/* .freq = */			55,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0x8b,	/* R27[7:0]  band7,band4 */
	}, {
	/* .freq = */			60,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0x7b,	/* R27[7:0]  band8,band4 */
	}, {
	/* .freq = */			65,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0x69,	/* R27[7:0]  band9,band6 */
	}, {
	/* .freq = */			70,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0x58,	/* R27[7:0]  band10,band7 */
	}, {
	/* .freq = */			75,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0x44,	/* R27[7:0]  band11,band11 */
	}, {
	/* .freq = */			90,		/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x02,	/* R26[7:6]=0 (LPF)  R26[1:0]=2 (low) */
	/* .tf_c = */			0x34,	/* R27[7:0]  band12,band11 */
	}, {
	/* .freq = */			100,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x01,	/* R26[7:6]=0 (LPF)  R26[1:0]=1 (middle) */
	/* .tf_c = */			0x34,	/* R27[7:0]  band12,band11 */
	}, {
	/* .freq = */			110,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x01,	/* R26[7:6]=0 (LPF)  R26[1:0]=1 (middle) */
	/* .tf_c = */			0x24,	/* R27[7:0]  band13,band11 */
	}, {
	/* .freq = */			140,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x01,	/* R26[7:6]=0 (LPF)  R26[1:0]=1 (middle) */
	/* .tf_c = */			0x14,	/* R27[7:0]  band14,band11 */
	}, {
	/* .freq = */			174,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x00,	/* R26[7:6]=0 (LPF)  R26[1:0]=0 (high) */
	/* .tf_c = */			0x12,	/* R27[7:0]  band14,band13 */
	}, {
	/* .freq = */			200,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x00,	/* R26[7:6]=0 (LPF)  R26[1:0]=0 (high) */
	/* .tf_c = */			0x11,	/* R27[7:0]  band14,band14 */
	}, {
	/* .freq = */			240,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x00,	/* R26[7:6]=0 (LPF)  R26[1:0]=0 (high) */
	/* .tf_c = */			0x00,	/* R27[7:0]  highest,highest */
	}, {
	/* .freq = */			280,	/* Start freq, in MHz */
	/* .rf_mux_ploy = */	0x40,	/* R26[7:6]=1 (bypass)  R26[1:0]=0 (high) */
	/* .tf_c = */			0x00,	/* R27[7:0]  highest,highest */
	}
};

/*
 * I2C read/write code and shadow registers logic
 */
static void shadow_store(struct r82xx_priv *priv, uint8_t reg, const uint8_t *val, int len)
{
	if(reg >= NUM_REGS)
		return;
	if (len <= 0)
		return;
	if (len > NUM_REGS - reg)
		len = NUM_REGS - reg;

	memcpy(&priv->regs[reg], val, len);
}

static int r82xx_write(struct r82xx_priv *priv, uint8_t reg, uint8_t *buf, int len)
{
	int rc;

	/* Store the shadow registers */
	shadow_store(priv, reg, buf, len);

	rc = rtlsdr_i2c_write_fn(priv->rtl_dev, priv->cfg->i2c_addr, reg, buf, len);
	if (rc != len) {
		printf( "%s: i2c wr failed=%d reg=%02x len=%d\n",
			   __FUNCTION__, rc, reg, len);
		if (rc < 0)
			return rc;
		return -1;
	}
	return 0;
}

static inline int r82xx_write_reg(struct r82xx_priv *priv, uint8_t reg, uint8_t val)
{
	return r82xx_write(priv, reg, &val, 1);
}

static int r82xx_read_cache_reg(struct r82xx_priv *priv, int reg)
{
	if (reg >= 0 && reg < NUM_REGS)
		return priv->regs[reg];
	else
		return -1;
}

static int r82xx_write_reg_mask(struct r82xx_priv *priv, uint8_t reg, uint8_t val,
				uint8_t bit_mask)
{
	int rc = r82xx_read_cache_reg(priv, reg);

	if (rc < 0)
		return rc;

	val = (rc & ~bit_mask) | (val & bit_mask);

	if(rc == val)
		return 0;
	else
		return r82xx_write(priv, reg, &val, 1);
}


static uint8_t r82xx_bitrev(uint8_t byte)
{
	const uint8_t lut[16] = { 0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
				  0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf };

	return (lut[byte & 0xf] << 4) | lut[byte >> 4];
}

static int r82xx_read(struct r82xx_priv *priv, uint8_t *buf, int len)
{
	int rc, i;

	//up to 16 registers can be read
	if(len > 16)
		return -1;

	rc = rtlsdr_i2c_read_fn(priv->rtl_dev, priv->cfg->i2c_addr, 0, buf, len);
	if (rc != len) {
		printf( "%s: i2c rd failed=%d len=%d\n",
			   __FUNCTION__, rc, len);
		if (rc < 0)
			return rc;
		return -1;
	}

	/* Copy data to the output buffer */
	for (i = 0; i < len; i++)
		buf[i] = r82xx_bitrev(buf[i]);

	return 0;
}

/*static void print_registers(struct r82xx_priv *priv)
{
	uint8_t data[5];
	int rc;
	unsigned int i;

	rc = r82xx_read(priv, data, sizeof(data));
	if (rc < 0)
		return;
	for(i=0; i<sizeof(data); i++)
		printf("%02x ", data[i]);
	for(i=sizeof(data); i<32; i++)
		printf("%02x ", r82xx_read_cache_reg(priv, i));
	printf("\n");
}*/

//RTL-SDR.COM
static const int16_t abs_freqs_r820t[] = {
  25, 26, 27, 28, 30, 32, 35, 40, 50, 50, 55, 55, 60, 60, 65, 65, 70, 70, 75, 75,100,100,140,140,174,174,200,200,240,240,280,280,320,345,345,600,850,1000,1500,1700,1750};
static const int16_t abs_gains_r820t[] = {
 194,178,169,161,149,141,134,137,143,139,139,127,127,127,127,118,118,113,114, 90, 92,104,107,107,102, 86, 83, 77, 72, 69, 68, 59, 66, 71, 71, 92,109, 118, 138, 145, 153};

//Astrometa
static const int16_t abs_freqs_r828d[] = {
 25, 26, 27, 28, 30, 32, 35, 40, 50, 50, 55, 55, 60, 60, 65, 65, 70, 70, 75, 75,100,100,140,140,174,174,200,200,240,240,260,280,280,320,345,345,365,400,500,600,850,1000,1500,1700,1750};
static const int16_t abs_gains_r828d[] = {
251,245,239,234,224,215,209,202,192,189,184,174,170,170,167,160,157,153,151,130,124,137,125,127,105,100, 90, 89, 92, 84, 87, 98, 93,114,146,129,110, 98, 97, 102,110,121, 187, 230, 241};

//RTL-SDR Blog V4
static const int16_t abs_freqs_rtlsdr_v4[] = {
  1,  3,  5, 10, 15, 20, 23, 25, 27, 27, 30, 35, 40, 50, 50, 55, 55, 60, 60, 65, 65, 70, 70, 75, 75, 90, 90,100,100,110,110,120,120,140,140,160,160,174,174,200,220,230,230,240,240,250,250,265,280,280,300,330,360,400,450,500,600,800,1000,1250,1500,1760};
static const int16_t abs_gains_rtlsdr_v4[] = {
210,136,112,101,114,138,169,215,255,167,165,168,168,182,171,170,160,159,160,159,152,152,148,148,126,127,127,128,141,140,141,140,149,129,130,140,126,120,115,114,119,127,114,115,114,119,146,119,109,106, 97, 94, 94, 97,106,115,137,169, 189, 206, 211, 208};

static void calculate_abs_gain(struct r82xx_priv *priv)
{
	if (priv->cfg->rafael_chip == CHIP_R828D)
	{
		if (priv->cfg->xtal > 24000000.0) //RTL-SDR Blog V4
			priv->abs_gain = interpolate(priv->freq, ARRAY_SIZE(abs_gains_rtlsdr_v4), abs_freqs_rtlsdr_v4, abs_gains_rtlsdr_v4);
		else
			priv->abs_gain = interpolate(priv->freq, ARRAY_SIZE(abs_gains_r828d), abs_freqs_r828d, abs_gains_r828d);
	}
	else
		priv->abs_gain = interpolate(priv->freq, ARRAY_SIZE(abs_gains_r820t), abs_freqs_r820t, abs_gains_r820t);
	//printf("abs_gain = %d\n", priv->abs_gain);
}

/*
 * r82xx tuning logic
 */
static int r82xx_set_mux(struct r82xx_priv *priv, uint32_t freq)
{
	const struct r82xx_freq_range *range;
	int rc;
	unsigned int i;

	/* Get the proper frequency range */
	freq = freq / 1000000;
	for (i = 0; i < ARRAY_SIZE(freq_ranges) - 1; i++)
	{
		if (freq < freq_ranges[i + 1].freq)
			break;
	}
	range = &freq_ranges[i];

	/* RF_MUX,Polymux */
	rc = r82xx_write_reg_mask(priv, 0x1a, range->rf_mux_ploy, 0xc3);
	if (rc < 0)
		return rc;

	/* TF BAND */
	rc = r82xx_write_reg(priv, 0x1b, range->tf_c);

	return rc;
}

static int r82xx_set_pll(struct r82xx_priv *priv, uint32_t freq)
{
	int rc, i;
	int64_t vco_freq, vco_div;
	uint32_t vco_min = 1770000000;
	double pll_ref;
	uint32_t sdm;
	uint8_t div_num;
	uint8_t refdiv2 = 0;
	uint8_t ni, si, nint, val;
	uint8_t data[3];

	if ((freq < 25000000) || (freq > vco_min)){
		printf( "[R82XX] No valid PLL values for %u Hz!\n", freq);
		return -1;
	}

	pll_ref = priv->cfg->xtal;

	if (priv->cfg->xtal > 24000000.0) {
		pll_ref /= 2;
		refdiv2 = 0x10;
	}
	rc = r82xx_write_reg_mask(priv, 0x10, refdiv2, 0x10);
	if (rc < 0)
		return rc;

	/* set pll autotune = 128kHz */
	rc = r82xx_write_reg_mask(priv, 0x1a, 0x00, 0x0c);
	if (rc < 0)
		return rc;

	/* set VCO current = 100 */
	rc = r82xx_write_reg_mask(priv, 0x12, 0x80, 0xe0);
	if (rc < 0)
		return rc;

	/* Calculate divider */
	for (div_num = 0; div_num < 5; div_num++)
		if ((freq << (div_num + 1)) >= vco_min)
			break;

	rc = r82xx_write_reg_mask(priv, 0x10, div_num << 5, 0xe0);
	if (rc < 0)
		return rc;

	vco_freq = (int64_t)freq << (div_num + 1);

	/*
	 * We want to approximate:
	 *
	 *  vco_freq / (2 * pll_ref)
	 *
	 * in the form:
	 *
	 *  nint + sdm/65536
	 *
	 * where nint,sdm are integers and 0 < nint, 0 <= sdm < 65536
	 *
	 * Scaling to fixed point and rounding:
	 *
	 *  vco_div = 65536*(nint + sdm/65536) = int( 0.5 + 65536 * vco_freq / (2 * pll_ref) )
	 *  vco_div = 65536*nint + sdm         = int( (pll_ref + 65536 * vco_freq) / (2 * pll_ref) )
	 */

	vco_div = (pll_ref + 65536 * vco_freq) / (2 * pll_ref);
    nint = vco_div / 65536;
	sdm = vco_div % 65536;
    //printf("nint = %d, sdm = %d\n", nint, sdm);

	ni = (nint - 13) / 4;
	si = nint - 4 * ni - 13;
	rc = r82xx_write_reg(priv, 0x14, ni + (si << 6));
	if (rc < 0)
		return rc;

	/* pw_sdm */
	if (sdm == 0)
		val = 0x08;
	else
		val = 0x00;
	rc = r82xx_write_reg_mask(priv, 0x12, val, 0x08);
	if (rc < 0)
		return rc;

	rc = r82xx_write_reg(priv, 0x16, sdm >> 8);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x15, sdm & 0xff);
	if (rc < 0)
		return rc;

	for (i = 0; i < 2; i++) {

		/* Check if PLL has locked */
		rc = r82xx_read(priv, data, 3);
		if (rc < 0)
			return rc;
		if (data[2] & 0x40)
			break;

		if (!i) {
			/* Didn't lock. Increase VCO current */
			rc = r82xx_write_reg_mask(priv, 0x12, 0x60, 0xe0);
			if (rc < 0)
				return rc;
		}
	}

	if (!(data[2] & 0x40)) {
		printf( "[R82XX] PLL not locked for %u Hz!\n", freq);
		priv->has_lock = 0;
		return -1;
	}

	priv->has_lock = 1;

	/* set pll autotune = 8kHz */
	rc = r82xx_write_reg_mask(priv, 0x1a, 0x08, 0x08);
	if (rc < 0)
		return rc;
	{
		int zf, tuning_error;
		int64_t actual_vco;
		double dither_offset = 0.0;
		uint8_t mix_div = 1 << (div_num + 1);
		if(sdm) //frac pll enabled
		{
			if(r82xx_read_cache_reg(priv, 0x12) & 0x10)
				dither_offset = 0.5;
			else
				dither_offset = 0.25;
		}
		actual_vco = 2 * pll_ref * nint + 2 * pll_ref * (dither_offset + sdm) / 65536;
		tuning_error = (int)(actual_vco - vco_freq) / mix_div;
		//printf( "[R82XX] requested %uHz; selected mix_div=%u vco_freq=%lld nint=%u sdm=%u; actual_vco=%lld; xtal=%.1f, tuning error=%dHz\n",
		//		freq, mix_div, vco_freq, nint, sdm, actual_vco, priv->cfg->xtal, tuning_error);
		if(priv->sideband)
			zf = priv->int_freq - tuning_error;
		else
			zf = priv->int_freq + tuning_error;
		return rtlsdr_set_if_freq(priv->rtl_dev, zf+3);
	}
}

static int r82xx_sysfreq_sel(struct r82xx_priv *priv,
				 enum r82xx_tuner_type type)
{
	int rc;

	if (priv->cfg->use_predetect) {
		rc = r82xx_write_reg_mask(priv, 0x06, 0x40, 0x40);
		if (rc < 0)
			return rc;
	}

	priv->input = 0;

	/*
	 * Set LNA
	 */

	if (type != TUNER_ANALOG_TV) {
		/* LNA TOP: lowest */
		rc = r82xx_write_reg_mask(priv, 0x1d, 0, 0x38);
		if (rc < 0)
			return rc;

		/* 0: PRE_DECT off */
		rc = r82xx_write_reg_mask(priv, 0x06, 0, 0x40);
		if (rc < 0)
			return rc;

		/* agc clk 250hz */
		rc = r82xx_write_reg_mask(priv, 0x1a, 0x30, 0x30);
		if (rc < 0)
			return rc;

		/* write LNA TOP = 3 */
		rc = r82xx_write_reg_mask(priv, 0x1d, 0x18, 0x38);
		if (rc < 0)
			return rc;

		/* agc clk 60hz */
		rc = r82xx_write_reg_mask(priv, 0x1a, 0x20, 0x30);
		if (rc < 0)
			return rc;
	} else {
		/* PRE_DECT off */
		rc = r82xx_write_reg_mask(priv, 0x06, 0, 0x40);
		if (rc < 0)
			return rc;

		/* write LNA TOP */ /* detect bw 3, lna top:4, predet top:2 */
		rc = r82xx_write_reg_mask(priv, 0x1d, 0xe5, 0x38);
		if (rc < 0)
			return rc;

		/* agc clk 1Khz, external det1 cap 1u */
		rc = r82xx_write_reg_mask(priv, 0x1a, 0x00, 0x30);
		if (rc < 0)
			return rc;

		rc = r82xx_write_reg_mask(priv, 0x10, 0x00, 0x04);
		if (rc < 0)
			return rc;
	 }
	 return 0;
}

int r82xx_set_gain_mode(struct r82xx_priv *priv, int set_manual_gain)
{
	//printf("manual_gain=%d\n", set_manual_gain);
	return r82xx_write_reg_mask(priv, 0x0c, set_manual_gain ? 0x00 : 0x10, 0x10);
}

static const int r82xx_gains[] = {
	0,34,68,102,137,171,207,240,278,312,346,382,416,453,488,527};

int r82xx_set_gain_index(struct r82xx_priv *priv, unsigned int i)
{
	return r82xx_write_reg_mask(priv, 0x0c, i, 0x0f);
}

#ifdef DEBUG
static unsigned char cmd = 0;
#endif

/* expose/permit tuner specific i2c register hacking! */
int r82xx_set_i2c_register(struct r82xx_priv *priv, unsigned i2c_register, unsigned data, unsigned mask)
{
#ifdef DEBUG
	if(i2c_register == NUM_REGS+REG_SHADOW_START) //Debug register
	{
		//AGC-Test
		if(mask & 1)
		{
			rtlsdr_set_agc_mode(priv->rtl_dev, data & 1);
			printf("set agc mode %u\n", data & 1);
		}
		//Reset Demod
		if((mask & 2) && (data & 2))
		{
			rtlsdr_reset_demod(priv->rtl_dev);
			printf("reset demod\n");
		}
		if((mask & 8) && (data & 8))
		{
			r82xx_write_reg(priv, 0x05, 0xa0); //LNA off
			r82xx_write_reg(priv, 0x07, 0x60); //Mixer minimal gain
			r82xx_write_reg(priv, 0x0f, 0x60); //ring clk on
			r82xx_write_reg(priv, 0x18, 0x5b); //ring power on
			r82xx_write_reg(priv, 0x19, 0xef); //ring_freq = ring_vco / 48
			r82xx_write_reg(priv, 0x1c, 0x26); //from ring = ring pll in
		}
		cmd = (cmd & ~mask) | (data & mask);
		return 0;
	}
	else
	{
		if(i2c_register >= NUM_REGS)
			i2c_register -= NUM_REGS;
		return r82xx_write_reg_mask(priv, i2c_register & 0xFF, data & 0xff, mask & 0xff);
	}
#endif
	if(i2c_register < NUM_REGS)
		return r82xx_write_reg_mask(priv, i2c_register & 0xFF, data & 0xff, mask & 0xff);
	else
		return -1;
}

static const int16_t lna_freqs_r820t[] = {
	  25,  30,  50,  75, 100, 200, 500, 750, 980,1250,1500,1700};
static const int16_t lna_gains_r820t[][16] = {
	{  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},
	{ 36,  36,  35,  35,  35,  35,  33,  30,  29,  28,  30,  30},
	{ 77,  76,  74,  74,  74,  74,  70,  66,  65,  64,  69,  68},
	{115, 113, 109, 108, 108, 107, 105, 103, 104, 104, 104, 104},
	{146, 141, 136, 131, 131, 130, 131, 134, 137, 139, 130, 124},
	{160, 155, 150, 147, 146, 145, 146, 149, 152, 154, 147, 142},
	{184, 180, 176, 174, 172, 172, 173, 176, 179, 181, 175, 170},
	{208, 205, 201, 200, 199, 198, 200, 202, 205, 206, 202, 194},
	{234, 231, 228, 227, 226, 226, 229, 231, 233, 230, 227, 215},
	{260, 258, 254, 254, 254, 253, 255, 253, 249, 240, 232, 211},
	{281, 279, 275, 274, 273, 271, 274, 267, 256, 242, 233, 213},
	{293, 291, 287, 286, 284, 282, 288, 278, 263, 246, 236, 215},
	{306, 305, 301, 299, 296, 294, 302, 290, 271, 251, 241, 220},
	{328, 327, 322, 321, 319, 316, 317, 297, 276, 256, 242, 218},
	{345, 343, 339, 338, 337, 334, 334, 317, 296, 272, 252, 225},
	{289, 299, 322, 339, 342, 343, 346, 325, 303, 279, 255, 227}};

static const int16_t lna_freqs_r828d[] = {
	  25,  30,  50, 100, 200, 345, 345, 500, 750, 980,1250,1500,1700};
static const int16_t lna_gains_r828d[][16] = {
	{  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},
	{ 43,  41,  37,  35,  36,  41,  42,  39,  33,  32,  33,  29,  29},
	{ 99,  94,  84,  78,  79,  87,  87,  82,  71,  69,  72,  67,  64},
	{133, 130, 122, 114, 111, 111, 111, 108, 103, 103, 103, 101, 100},
	{146, 147, 148, 139, 130, 117, 120, 121, 123, 128, 119, 117, 124},
	{177, 177, 177, 169, 163, 153, 136, 137, 139, 144, 138, 134, 143},
	{205, 204, 202, 195, 191, 185, 166, 166, 167, 172, 169, 168, 168},
	{232, 231, 227, 221, 219, 216, 196, 195, 195, 200, 201, 196, 189},
	{262, 260, 254, 249, 248, 251, 227, 225 ,225, 231, 234, 221, 201},
	{293, 290, 281, 277, 278, 286, 258, 255, 252, 253, 248, 217, 191},
	{300, 299, 297, 295, 293, 297, 267, 270, 268, 262, 252, 218, 192},
	{302, 303, 306, 305, 301, 303, 273, 279, 281, 271, 258, 222, 195},
	{304, 306, 315, 316, 312, 309, 278, 289, 297, 283, 265, 228, 201},
	{341, 341, 342, 341, 338, 345, 307, 315, 308, 289, 264, 222, 196},
	{374, 372, 364, 362, 362, 375, 335, 337, 326, 308, 276, 233, 205},
	{403, 397, 379, 376, 381, 399, 354, 350, 327, 307, 280, 239, 214}};

static const int16_t lna_freqs_rtlsdr_v4[] = {
	   1,  10,  27,  27,  50, 100, 120, 120, 150, 200, 230, 230, 250, 250, 350, 500, 750,1000,1250,1500,1760};
static const int16_t lna_gains_rtlsdr_v4[][21] = {
	{  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},
	{ 19,  20,  22,  19,  22,  21,  21,  20,  21,  23,  24,  24,  25,  32,  21,  21,  19,  18,  17,  18,  18},
	{ 45,  48,  51,  44,  49,  45,  45,  43,  44,  48,  52,  52,  54,  56,  45,  46,  40,  38,  36,  36,  37},
	{ 98, 100, 101,  87,  85,  66,  64,  64,  63,  75,  90,  88,  89,  89,  76,  75,  58,  53,  51,  54,  57},
	{129, 130, 130, 122, 119,  98,  96,  95,  95, 104, 118, 117, 118, 119, 106, 104,  89,  85,  84,  87,  90},
	{132, 134, 136, 137, 140, 128, 127, 123, 124, 125, 131, 135, 135, 142, 130, 131, 124, 122, 120, 122, 120},
	{135, 137, 140, 152, 159, 162, 163, 155, 157, 146, 144, 149, 150, 164, 155, 156, 164, 167, 168, 166, 157},
	{164, 166, 169, 177, 184, 185, 186, 179, 181, 173, 172, 176, 178, 191, 181, 182, 186, 187, 185, 182, 173},
	{197, 199, 201, 207, 212, 211, 212, 206, 208, 203, 204, 207, 209, 220, 209, 210, 211, 209, 206, 202, 195},
	{230, 232, 234, 236, 240, 236, 237, 233, 234, 232, 236, 238, 239, 248, 236, 237, 234, 230, 223, 218, 213},
	{264, 265, 267, 266, 268, 261, 261, 258, 259, 260, 267, 268, 269, 274, 264, 264, 256, 248, 238, 231, 229},
	{299, 300, 301, 296, 296, 285, 285, 283, 283, 288, 298, 297, 299, 300, 289, 286, 270, 256, 242, 235, 232},
	{333, 333, 333, 325, 323, 307, 306, 305, 304, 313, 326, 324, 325, 313, 305, 310, 298, 276, 257, 245, 240},
	{371, 370, 368, 356, 349, 328, 326, 326, 324, 338, 356, 352, 352, 343, 332, 328, 301, 278, 259, 248, 244},
	{419, 417, 413, 392, 379, 351, 348, 351, 348, 371, 396, 386, 384, 377, 363, 353, 325, 300, 279, 266, 260},
	{446, 443, 438, 415, 400, 372, 369, 372, 369, 394, 420, 408, 404, 379, 370, 377, 355, 326, 303, 287, 274}};

static const int r82xx_mixer_gains[]  = {
 	0, 13, 32, 49, 63, 76, 91, 105, 119, 133, 148, 161, 174, 174, 174, 174
};

//RTL-SDR.COM
static const int16_t if_agc_tab_r820t[] = {
	0xe000,0xffa0,0x02a0,0x0480,0x06b0,0x08f0,0x0ae0,0x0c70,0x0e20,0x0fb0,0x10e0,0x11d0,0x12d0,0x13c0,0x1520,0x16a0,0x1870,0x1a50,0x1fff
};
static const int16_t if_gain_tab_r820t[] = {
	   -60,   -41,   -01,    29,    59,    89,   119,   149,   179,   209,   239,   268,  297,   327,   356,   384,   412,   440,  459
};

//Astrometa
static const int16_t if_agc_tab_r828d[]  = {
	0xe000,0x0bc0,0xf30,0x11a0,0x13d0,0x15e0,0x1770,0x1920,0x1ac0,0x1c40,0x1d50,0x1e20,0x1ee0,0x1fc0,0x1fff};
static const int16_t if_gain_tab_r828d[] = {
	   -73,   -43,  -13,   17,     47,    77,   107,   137,   167,   197,   227,   257,   287,   315,   324};

/*NESDDR Nano2+
static const int16_t if_agc_tab_r820t[]  = {
	0xe000,0xf260,0xf8e0,0xfc80,0x00d0,0x0330,0x0550,0x07b0,0x09a0,0x0b60,0x0ca0,0x0dd0,0x0ee5,0x0fe0,0x10d0,0x11a0,0x1250,0x1350,0x13e0,0x1fff
};
static const int16_t if_gain_tab_r820t[] = {
	   -54,   -24,    06,    36,    66,    95,   124,   153,   179,   208,   235,   259,   283,   305,   332,   344,   366,   378,   395,  400
};*/

static int r82xx_get_signal_strength(struct r82xx_priv *priv, unsigned char* data)
{
	unsigned int lna_index, lna_gain;
	int	slave_demod;
	int if_gain = 0;
	uint8_t mixer_gain = (data[3] >> 4) & 0x0f;

	/* set IMR_G */
	if(mixer_gain != priv->old_gain)
	{
		int rc = r82xx_write_reg_mask(priv, 0x08, priv->reg8[mixer_gain], 0x3f);
		if(rc < 0)
			return rc;
		priv->old_gain = mixer_gain;
	}

	/* IF gain */
	if((data[0x0c] & 0x10) == 0x10) //IF vga gain controlled by vagc pin
	{
		int16_t if_agc_val = rtlsdr_get_agc_val(priv->rtl_dev, &slave_demod);
		if(slave_demod)
			if_gain = interpolate(if_agc_val, ARRAY_SIZE(if_agc_tab_r828d), if_agc_tab_r828d, if_gain_tab_r828d);
		else
			if_gain = interpolate(if_agc_val, ARRAY_SIZE(if_agc_tab_r820t), if_agc_tab_r820t, if_gain_tab_r820t);
#ifdef DEBUG
		data[NUM_REGS+REG_SHADOW_START+1] = (if_agc_val >> 8) & 0xff;
		data[NUM_REGS+REG_SHADOW_START+2] = if_agc_val & 0xff;
#endif
	}
	else
	{
		if_gain = r82xx_gains[data[0x0c] & 0x0f];
#ifdef DEBUG
		data[NUM_REGS+REG_SHADOW_START+1] = 0;
		data[NUM_REGS+REG_SHADOW_START+2] = 0;
#endif
	}
	/* LNA gain */
	lna_index = data[3] & 0xf;
	if(lna_index)
	{
		if (priv->cfg->rafael_chip == CHIP_R828D)
		{
			if (priv->cfg->xtal > 24000000.0) //RTL-SDR Blog V4
				lna_gain = interpolate(priv->freq, ARRAY_SIZE(lna_freqs_rtlsdr_v4), lna_freqs_rtlsdr_v4, lna_gains_rtlsdr_v4[lna_index]);
			else
				lna_gain = interpolate(priv->freq, ARRAY_SIZE(lna_freqs_r828d), lna_freqs_r828d, lna_gains_r828d[lna_index]);
		}
		else
			lna_gain = interpolate(priv->freq, ARRAY_SIZE(lna_freqs_r820t), lna_freqs_r820t, lna_gains_r820t[lna_index]);
	}
	else
		lna_gain = 0;

	/* Sum_of_all_gains = if_gain + lna_gain + mixer_gain + absolute gain*/
	//printf("if_gain=%d, lna_gain=%d, mixer_gain=%d, abs_gain=%d\n", if_gain, lna_gain, r82xx_mixer_gains[mixer_gain], priv->abs_gain);
	return if_gain + lna_gain + r82xx_mixer_gains[mixer_gain] - priv->abs_gain;

}

int r82xx_get_i2c_register(struct r82xx_priv *priv, unsigned char* data, int *len, int *strength)
{
	int rc;

	*len = NUM_REGS;
	// The lower 16 I2C registers can be read with the normal read fct, the upper ones are read from the cache
	rc = r82xx_read(priv, data, REG_SHADOW_START);
	if (rc < 0)
		return rc;
	memcpy(data+REG_SHADOW_START, priv->regs+REG_SHADOW_START, NUM_REGS-REG_SHADOW_START);
#ifdef DEBUG
	*len += REG_SHADOW_START+4;
	memcpy(data+NUM_REGS, priv->regs, REG_SHADOW_START);
	data[NUM_REGS+REG_SHADOW_START] = cmd;
	data[NUM_REGS+REG_SHADOW_START+3] = rtlsdr_demod_read_reg(priv->rtl_dev, 3, 0x05, 1);
#endif
	*strength = r82xx_get_signal_strength(priv, data);
	return 0;
}

struct r82xx_val {
	int bw; 		// bandwidth in kHz
	uint8_t lp; 	// low pass fine filter
	uint8_t hp; 	// high pass filter and low pass course filter
	int int_freq;	// intermediate frequency
};
static const struct r82xx_val r82xx[]= {
	{ 400, 0x0f, 0xe8, 1780},
	{ 500, 0x0f, 0xe9, 1710},
	{ 620, 0x0f, 0xea, 1640},
	{ 900, 0x0f, 0xeb, 1500},
	{1150, 0x0f, 0xec, 1380},
	{1250, 0x0f, 0xed, 1340},
	{1370, 0x0f, 0xee, 1305},
	{1410, 0x0e, 0xee, 1320},
	{1440, 0x0d, 0xee, 1340},
	{1480, 0x0c, 0xee, 1360},
	{1510, 0x0b, 0xee, 1375},
	{1550, 0x0a, 0xee, 1395},
	{1590, 0x09, 0xee, 1415},
	{1630, 0x08, 0xee, 1435},
	{1660, 0x07, 0xee, 1450},
	{1700, 0x06, 0xee, 1470},
	{1740, 0x05, 0xee, 1490},
	{1790, 0x04, 0xee, 1515},
	{1830, 0x03, 0xee, 1535},
	{1870, 0x02, 0xee, 1555},
	{2000, 0x09, 0xae, 1580},
	{2100, 0x0f, 0x8e, 1620},
	{2900, 0x04, 0x8e, 2060},
	{5000, 0x0b, 0x6b, 3570},
};

int r82xx_set_bandwidth(struct r82xx_priv *priv, int bw, uint32_t * applied_bw, int apply)
{
	int rc;
	unsigned int i;

	for(i=0; i < ARRAY_SIZE(r82xx) - 1; ++i)
	{
		/* bandwidth is compared to median of the current and next available bandwidth in the table */
		if (bw < (r82xx[i+1].bw + r82xx[i].bw) * 500)
			break;
	}
	*applied_bw = r82xx[i].bw * 1000;
	if (apply)
		priv->int_freq = r82xx[i].int_freq * 1000;
	else
		return 0;

	rc = r82xx_write_reg_mask(priv, 0x0a, r82xx[i].lp, 0x0f);
	if (rc < 0)
		return rc;

	/* Register 0xB = R11 with undocumented Bit 7 for filter bandwidth for Hi-part FILT_BW */
	rc = r82xx_write_reg_mask(priv, 0x0b, r82xx[i].hp, 0xef);
	if (rc < 0)
		return rc;

	return priv->int_freq;
}

int r82xx_set_sideband(struct r82xx_priv *priv, int sideband)
{
	int rc;
	priv->sideband = sideband;
	rc = r82xx_write_reg_mask(priv, 0x07, sideband ? 0x80 : 0x00, 0x80);
	if (rc < 0)
		return rc;
	return 0;
}

int r82xx_set_freq(struct r82xx_priv *priv, uint32_t freq)
{
	int rc = -1;
	uint32_t lo_freq;
	uint8_t low_gain;
	int is_rtlsdr_blog_v4;
	uint32_t upconvert_freq;
	uint8_t air_cable1_in;
	uint8_t cable_2_in;
	uint8_t cable_1_in;
	uint8_t air_in;
	uint8_t open_d;
	uint8_t band;


	is_rtlsdr_blog_v4 = (priv->cfg->xtal > 24000000.0) && (priv->cfg->rafael_chip == CHIP_R828D);

	/* if it's an RTL-SDR Blog V4, automatically upconvert by 28.8 MHz if we tune to HF
	 * so that we don't need to manually set any upconvert offset in the SDR software */
	upconvert_freq = is_rtlsdr_blog_v4 ? ((freq <= MHZ(27)) ? (freq + priv->cfg->xtal) : freq) : freq;

	priv->freq = freq / 1000000;
	calculate_abs_gain(priv);
	rc = r82xx_set_mux(priv, upconvert_freq);
	if (rc < 0)
		goto err;

	if (is_rtlsdr_blog_v4)
	{
		/* determine if notch filters should be on or off notches are turned OFF
		 * when tuned within the notch band and ON when tuned outside the notch band.
		 */
		open_d = (freq < MHZ(8) || (freq >= MHZ(50) && freq < MHZ(120)) || (freq >= MHZ(160) && freq < MHZ(230))) ? 0x00 : 0x08;
		rc = r82xx_write_reg_mask(priv, 0x17, open_d, 0x08);
		if (rc < 0)
			goto err;

		/* select tuner band based on frequency and only switch if there is a band change
		 *(to avoid excessive register writes when tuning rapidly)
		 */
		band = (freq < MHZ(27)) ? HF : ((freq >= MHZ(27) && freq < MHZ(250)) ? VHF : UHF);

		/* switch between tuner inputs on the RTL-SDR Blog V4 */
		if (band != priv->input) {
			priv->input = band;

			/* activate cable 2 (HF input) */
			cable_2_in = (band == HF) ? 0x08 : 0x00;
			rc = r82xx_write_reg_mask(priv, 0x06, cable_2_in, 0x08);
			if (rc < 0)
				goto err;

			/* activate cable 1 (VHF input) */
			cable_1_in = (band == VHF) ? 0x40 : 0x00;
			rc = r82xx_write_reg_mask(priv, 0x05, cable_1_in, 0x40);
			if (rc < 0)
				goto err;

			/* activate air_in (UHF input) */
			air_in = (band == UHF) ? 0x00 : 0x20;
			rc = r82xx_write_reg_mask(priv, 0x05, air_in, 0x20);
			if (rc < 0)
				goto err;

		}
	}
	else if (priv->cfg->rafael_chip == CHIP_R828D) /* Standard R828D dongle*/
	{
		/* switch between 'Cable1' and 'Air-In' inputs on sticks with
		 * R828D tuner. We switch at 345 MHz, because that's where the
		 * noise-floor has about the same level with identical LNA
		 * settings. The original driver used 320 MHz. */
		air_cable1_in = (freq >= MHZ(345)) ? 0x00 : 0x60;
		if(air_cable1_in != priv->input)
		{
			priv->input = air_cable1_in;
			rc = r82xx_write_reg_mask(priv, 0x05, air_cable1_in, 0x60);
			if (rc < 0)
				goto err;
		}

		/* Open Drain */
		rc = r82xx_write_reg_mask(priv, 0x17, (freq < MHZ(75)) ? 8 : 0, 0x08);
		if (rc < 0)
			return rc;

		low_gain = (freq >= MHZ(345)) ? 0x20 : 0x00;
		rc = r82xx_write_reg(priv, 0x01, low_gain);
		if (rc < 0)
			goto err;
	}

	if(priv->sideband)
		lo_freq = upconvert_freq - priv->int_freq;
	else
		lo_freq = upconvert_freq + priv->int_freq;
	rc = r82xx_set_pll(priv, lo_freq);

err:
	return rc;
}

/*
 * r82xx standby logic
 */

int r82xx_standby(struct r82xx_priv *priv)
{
	int rc;

	/* If device was not initialized yet, don't need to standby */
	if (!priv->init_done)
		return 0;

	rc = r82xx_write_reg(priv, 0x06, 0xb1);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x05, 0xa0);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x07, 0x3a);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x08, 0x40);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x09, 0xc0);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x0a, 0x36);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x0c, 0x35);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x0f, 0x68);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x11, 0x03);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x17, 0xf4);
	if (rc < 0)
		return rc;
	rc = r82xx_write_reg(priv, 0x19, 0x0c);

	return rc;
}

static int r82xx_multi_read(struct r82xx_priv *priv)
{
	int rc, i;
	uint8_t data[2];
	//uint8_t buf[4];
	int sum = 0;

#ifdef _WIN32
	LARGE_INTEGER StartingTime, EndingTime;
	LARGE_INTEGER Frequency;
	int64_t Microseconds = 0;

	QueryPerformanceCounter(&StartingTime);
	QueryPerformanceFrequency(&Frequency);
	while(Microseconds < 6000)
	{
		Sleep(0);
		QueryPerformanceCounter(&EndingTime);
		Microseconds = EndingTime.QuadPart - StartingTime.QuadPart;
		Microseconds *= 1000000;
		Microseconds /= Frequency.QuadPart;
	};
	//printf("Microseconds %d\n",(int)Microseconds);
#else
	usleep(6000);
#endif
	for (i = 0; i < 2; i++) {
		rc = r82xx_read(priv, data, sizeof(data));
		if (rc < 0)
			return rc;
		data[1] &= 0x3f;
		sum += data[1];
		//buf[i] = data[1];
	}
	//printf("data[1] = %02x %02x\n", buf[0],buf[1]);
	return sum;
}

static int test_imrg(struct r82xx_priv *priv, int start, int end, int *min)
{
	int i, rc;
	int reg8 = 0;

	for(i=start; i<end; i++)
	{
		rc = r82xx_write_reg_mask(priv, 0x08, i, 0x3f);
		if (rc < 0)
			return rc;
		rc = r82xx_multi_read(priv);
		if (rc < 0)
			return rc;
		if (rc < *min)
		{
			*min = rc;
			reg8 = i;
			//printf("Reg8=%02x, sum=%d\n", reg8, rc);
		}
		else
			break;
	}
	return reg8;
}

static int r82xx_imr(struct r82xx_priv *priv, uint8_t range)
{
	uint8_t ring_div[] =	{  48,  32,  24,  16,  12,   8,   6,   4};
	uint8_t ring_se23[] =	{0x20,0x00,0x20,0x00,0x20,0x00,0x20,0x00};
	uint8_t ring_seldiv[] =	{   3,   3,   2,   2,   1,   1,   0,   0};

							//0  0 -3 -5 -5 -8 -8 -3 -5 -5 -8 -8 -8 dB
	uint8_t ring_att[] =	{ 1, 1, 3, 0, 0, 2, 2, 3, 0, 0, 2, 2, 2 };
	uint8_t n_ring = 15;
	int rc, i, min;
	uint32_t ring_freq, ring_ref;
	uint8_t vga;
	uint8_t gain;

	printf("IMR_G =");
	if (priv->cfg->xtal > 24000000)
		ring_ref = priv->cfg->xtal / 2000;
	else
		ring_ref = priv->cfg->xtal / 1000;

	//ring_vco = (16 + n_ring) * 8 * ring_ref;
	for (i = 0; i < 16; i++) {
		if ((16 + i) * 8 * ring_ref >= 3100000) {
			n_ring = i;
			break;
		}
	}

	range &= 7;
	ring_freq = ((16 + n_ring) * 8 * ring_ref) / ring_div[range];

	/* n_ring, ring_se23 */
	rc = r82xx_write_reg_mask(priv, 0x18, ring_se23[range] | n_ring, 0x2f);
	if (rc < 0)
		return rc;

	/* ring_sediv */
	rc = r82xx_write_reg_mask(priv, 0x19, ring_seldiv[range], 0x03);
	if (rc < 0)
		return rc;

	rc = r82xx_set_freq(priv, ring_freq * 1000 - 2 * priv->int_freq); //Image frequency
	if (rc < 0)
		return rc;
	//printf("Freq=%dkHz\n", ring_freq - priv->int_freq / 500);

	for(gain=0; gain < 13; gain++)
	{
		rc = r82xx_write_reg_mask(priv, 0x07, gain, 0x0f);
		if (rc < 0)
			return rc;
		if(gain < 7)
			//Filter gain +7 db
			rc = r82xx_write_reg_mask(priv, 0x06, 0x20, 0x20);
		else
			//Filter gain +0 db
			rc = r82xx_write_reg_mask(priv, 0x06, 0x00, 0x20);
		if (rc < 0)
			return rc;

		//set optimal level
		rc = r82xx_write_reg_mask(priv, 0x1f, ring_att[gain], 0x03);
		if (rc < 0)
			return rc;

		rc = r82xx_write_reg_mask(priv, 0x08, 0, 0x3f);
		if (rc < 0)
			goto err;

		/* decrease vga power to let image significant */
		for(vga=15; vga>0; vga--)
		{
			rc = r82xx_write_reg_mask(priv, 0x0c, vga, 0x0f);
			if (rc < 0)
				goto err;
			rc = r82xx_multi_read(priv);
			if (rc < 0)
				goto err;
			if (rc < 80)
				break;
		}
		min = rc;
		//printf("Mixer=%d, VGA=%d\n", gain, vga);

		rc = test_imrg(priv, 0x02, 0x0a, &min);
		if(rc==0)
			rc = test_imrg(priv, 0x22, 0x2a, &min);
		if(rc==0)
			rc = test_imrg(priv, 0x01, 0x02, &min);
		if(rc==0)
			rc = test_imrg(priv, 0x21, 0x22, &min);
		if (rc < 0)
			goto err;
		priv->reg8[gain] = rc;
		printf(" %02X", rc);
	}
	printf("\n");

	for(gain = 13; gain < 16; gain++)
		priv->reg8[gain] = priv->reg8[12];
	return 0;

err:
	if (rc < 0)
		printf( "%s: failed=%d\n", __FUNCTION__, rc);
	return rc;
}

static	uint8_t r82xx_calib_array[] = {
	0xa0,	//Reg 0x05  lna off (air-in off)
	0x32, 	//Reg 0x06	Set filt_3dB
	0x60,	//Reg 0x07	mixer gain mode = manual, gain = 0 dB
	0xc0, 	//Reg 0x08
	0x40, 	//Reg 0x09
	0xdf, 	//Reg 0x0a	narrowest filter
	0xe8,	//Reg 0x0b	fiter 400kHz
	0x6f, 	//Reg 0x0c	adc=on, vga code mode, gain = 52.5 dB
	0x53, 	//Reg 0x0d
	0x53, 	//Reg 0x0e
	0x60,	//Reg 0x0f	ring clk = on
	0x94, 	//Reg 0x10	mixer in = vco out / 32
	0xbb, 	//Reg 0x11
	0x80, 	//Reg 0x12
	0x00,	//Reg 0x13
	0x57, 	//Reg 0x14
	0xb0, 	//Reg 0x15
	0x05, 	//Reg 0x16
	0x30,	//Reg 0x17
	0x5b, 	//Reg 0x18	ring power = on
	0xef, 	//Reg 0x19
	0x2a, 	//Reg 0x1a
	0x34,	//Reg 0x1b
	0x26,	//Reg 0x1c	from ring = ring pll in
	0xdd, 	//Reg 0x1d
	0x8e, 	//Reg 0x1e	sw_pdect = det3
	0x41	//Reg 0x1f	pw_ring = 0 dB
};

static int r82xx_imr_callibrate(struct r82xx_priv *priv)
{
	int rc;
	uint32_t applied_bw;
	/*struct timeval tv;
	uint64_t StartTime, EndTime;

	gettimeofday(&tv, NULL);
	StartTime = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;*/

	/* Initialize registers */
	rc = r82xx_write(priv, 0x05, r82xx_calib_array, sizeof(r82xx_calib_array));
	if (rc < 0)
		goto err;

	if ((rc = r82xx_set_bandwidth(priv, 400000, &applied_bw, 1)) < 0) goto err;

	if ((rc = r82xx_imr(priv, 1)) < 0) goto err;

	/*gettimeofday(&tv, NULL);
	EndTime = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
	printf("%d msec\n", (int)(EndTime-StartTime));*/

	return 0;

err:
	if (rc < 0)
		printf( "%s: failed=%d\n", __FUNCTION__, rc);
	return rc;
}

/*
 * r82xx device init logic
 */
int r82xx_init(struct r82xx_priv *priv)
{
	int rc, i, checksum = 0;
	uint8_t buf[16];
	int offset = 0x80;

	memset(priv->reg8, 0, 16);
	priv->old_gain = 255;

	if (priv->cfg->cal_imr) // (re)calibration wanted
	{
 		if ((rc = r82xx_imr_callibrate(priv)) < 0) goto err;
 		checksum = 0;
 		buf[0] = 14;
		memcpy(buf+1, priv->reg8, 13);
		for(i = 0; i < 13; i++)
			checksum += priv->reg8[i];
		buf[14] = checksum & 0xff;
		// write calibration results to offset 0x80 in eeprom
		if ((rc = rtlsdr_write_eeprom(priv->rtl_dev, buf, offset, 15)) < 0)
			goto err;
	}

	// read calibration results from offset 0x80 in eeprom
	if(rtlsdr_read_eeprom(priv->rtl_dev, buf, offset, 15) == 15)
	{
		for(i=1; i<14; i++)
			checksum += buf[i];
		if((buf[0] == 14) && ((checksum & 0xff) == buf[14])) // checksum ok
		{
			memcpy(priv->reg8, buf+1, 13);
			for(i = 13; i < 16; i++)
				priv->reg8[i] = priv->reg8[12];
		}
		else
			printf("Image rejection not calibrated yet\n");
	}
	else
		printf("Reading from eeprom failed\n");

	/* Initialize registers */
	if ((rc = r82xx_write(priv, 0x05, r82xx_init_array, sizeof(r82xx_init_array))) < 0) goto err;
	priv->int_freq = 3570 * 1000;

	if ((rc = r82xx_sysfreq_sel(priv, TUNER_DIGITAL_TV)) < 0) goto err;
	rc = r82xx_write_reg_mask(priv, 0x08, priv->reg8[12], 0x3f);
	if(rc < 0)
		goto err;
	priv->init_done = 1;

err:
	if (rc < 0)
		printf( "%s: failed=%d\n", __FUNCTION__, rc);
	return rc;
}

const int *r82xx_get_gains(int *len)
{
	*len = sizeof(r82xx_gains);
	return r82xx_gains;
}

int r82xx_set_dither(struct r82xx_priv *priv, int dither)
{
	return r82xx_write_reg_mask(priv, 0x12, dither ? 0x00 : 0x10, 0x10);
}
