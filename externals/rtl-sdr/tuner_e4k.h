#ifndef _E4K_TUNER_H
#define _E4K_TUNER_H

/*
 * Elonics E4000 tuner driver
 *
 * (C) 2011-2012 by Harald Welte <laforge@gnumonks.org>
 * (C) 2012 by Sylvain Munaut <tnt@246tNt.com>
 * (C) 2012 by Hoernchen <la@tfc-server.de>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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

#define E4K_I2C_ADDR	0xc8
#define E4K_CHECK_ADDR	0x02
#define E4K_CHECK_VAL	0x40

enum e4k_reg {
	E4K_REG_MASTER1			= 0x00,
	E4K_REG_MASTER2			= 0x01,
	E4K_REG_MASTER3			= 0x02,
	E4K_REG_MASTER4			= 0x03,
	E4K_REG_MASTER5			= 0x04,
	E4K_REG_CLK_INP			= 0x05,
	E4K_REG_REF_CLK			= 0x06,
	E4K_REG_SYNTH1			= 0x07,
	E4K_REG_SYNTH2			= 0x08,
	E4K_REG_SYNTH3			= 0x09,
	E4K_REG_SYNTH4			= 0x0a,
	E4K_REG_SYNTH5			= 0x0b,
	// gap
	E4K_REG_SYNTH7			= 0x0d,
	E4K_REG_SYNTH8			= 0x0e,
	E4K_REG_SYNTH9			= 0x0f,
	E4K_REG_FILT1			= 0x10,
	E4K_REG_FILT2			= 0x11,
	E4K_REG_FILT3			= 0x12,
	// gap
	E4K_REG_GAIN1			= 0x14,
	E4K_REG_GAIN2			= 0x15,
	E4K_REG_GAIN3			= 0x16,
	E4K_REG_GAIN4			= 0x17,
	// gap
	E4K_REG_AGC1			= 0x1a,
	E4K_REG_AGC2			= 0x1b,
	E4K_REG_AGC3			= 0x1c,
	E4K_REG_AGC4			= 0x1d,
	E4K_REG_AGC5			= 0x1e,
	E4K_REG_AGC6			= 0x1f,
	E4K_REG_AGC7			= 0x20,
	E4K_REG_AGC8			= 0x21,
	// gap
	E4K_REG_AGC11			= 0x24,
	E4K_REG_AGC12			= 0x25,
	// gap
	E4K_REG_DC1				= 0x29,
	E4K_REG_DC2				= 0x2a,
	E4K_REG_DC3				= 0x2b,
	E4K_REG_DC4				= 0x2c,
	E4K_REG_DC5				= 0x2d,
	E4K_REG_DC6				= 0x2e,
	E4K_REG_DC7				= 0x2f,
	E4K_REG_DC8				= 0x30,
	// gap
	E4K_REG_QLUT0			= 0x50,
	E4K_REG_QLUT1			= 0x51,
	E4K_REG_QLUT2			= 0x52,
	E4K_REG_QLUT3			= 0x53,
	// gap
	E4K_REG_ILUT0			= 0x60,
	E4K_REG_ILUT1			= 0x61,
	E4K_REG_ILUT2			= 0x62,
	E4K_REG_ILUT3			= 0x63,
	// gap
	E4K_REG_DCTIME1			= 0x70,
	E4K_REG_DCTIME2			= 0x71,
	E4K_REG_DCTIME3			= 0x72,
	E4K_REG_DCTIME4			= 0x73,
	E4K_REG_PWM1			= 0x74,
	E4K_REG_PWM2			= 0x75,
	E4K_REG_PWM3			= 0x76,
	E4K_REG_PWM4			= 0x77,
	E4K_REG_BIAS			= 0x78,
	// gap
	E4K_REG_CLKOUT_PWDN		= 0x7a,
	E4K_REG_CHFILT_CALIB	= 0x7b,
	// gap
	E4K_REG_I2C_REG_ADDR	= 0x7d,
	// FIXME
};

#define E4K_MASTER1_RESET		(1 << 0)
#define E4K_MASTER1_NORM_STBY	(1 << 1)
#define E4K_MASTER1_POR_DET		(1 << 2)

#define E4K_SYNTH1_PLL_LOCK		(1 << 0)
#define E4K_SYNTH1_BAND_SHIF	 1

#define E4K_SYNTH7_3PHASE_EN	(1 << 3)

#define E4K_SYNTH8_VCOCAL_UPD	(1 << 2)

#define E4K_FILT3_DISABLE		(1 << 5)

#define E4K_AGC1_LIN_MODE		(1 << 4)
#define E4K_AGC1_LNA_UPDATE		(1 << 5)
#define E4K_AGC1_LNA_G_LOW		(1 << 6)
#define E4K_AGC1_LNA_G_HIGH		(1 << 7)

#define E4K_AGC6_LNA_CAL_REQ	(1 << 4)

#define E4K_AGC7_MIX_GAIN_AUTO	(1 << 0)
#define E4K_AGC7_GAIN_STEP_5dB	(1 << 5)

#define E4K_AGC8_SENS_LIN_AUTO	(1 << 0)

#define E4K_AGC11_LNA_GAIN_ENH	(1 << 0)

#define E4K_DC1_CAL_REQ			(1 << 0)

#define E4K_DC5_I_LUT_EN		(1 << 0)
#define E4K_DC5_Q_LUT_EN		(1 << 1)
#define E4K_DC5_RANGE_DET_EN	(1 << 2)
#define E4K_DC5_RANGE_EN		(1 << 3)
#define E4K_DC5_TIMEVAR_EN		(1 << 4)

#define E4K_CLKOUT_DISABLE		0x96

#define E4K_CHFCALIB_CMD		(1 << 0)

#define E4K_AGC1_MOD_MASK		0xF

enum e4k_agc_mode {
	E4K_AGC_MOD_SERIAL				= 0x0,
	E4K_AGC_MOD_IF_PWM_LNA_SERIAL	= 0x1,
	E4K_AGC_MOD_IF_PWM_LNA_AUTONL	= 0x2,
	E4K_AGC_MOD_IF_PWM_LNA_SUPERV	= 0x3,
	E4K_AGC_MOD_IF_SERIAL_LNA_PWM	= 0x4,
	E4K_AGC_MOD_IF_PWM_LNA_PWM		= 0x5,
	E4K_AGC_MOD_IF_DIG_LNA_SERIAL	= 0x6,
	E4K_AGC_MOD_IF_DIG_LNA_AUTON	= 0x7,
	E4K_AGC_MOD_IF_DIG_LNA_SUPERV	= 0x8,
	E4K_AGC_MOD_IF_SERIAL_LNA_AUTON	= 0x9,
	E4K_AGC_MOD_IF_SERIAL_LNA_SUPERV = 0xa,
};

enum e4k_band {
	E4K_BAND_VHF2	= 0,
	E4K_BAND_VHF3	= 1,
	E4K_BAND_UHF	= 2,
	E4K_BAND_L		= 3,
};

struct e4k_pll_params {
	double fosc;
	uint32_t flo;
};

/* structure describing a field in a register */
struct reg_field {
	uint8_t reg;
	uint8_t shift;
	uint8_t width;
};

struct e4k_state {
	void *i2c_dev;
	uint8_t i2c_addr;
	enum e4k_band band;
	struct e4k_pll_params vco;
	void *rtl_dev;
};

int e4k_init(struct e4k_state *e4k);
int e4k_standby(struct e4k_state *e4k, int enable);
int e4k_if_gain_set(struct e4k_state *e4k, uint8_t stage, int8_t value);
int e4k_tune_freq(struct e4k_state *e4k, uint32_t freq);
int e4k_set_bandwidth(struct e4k_state *e4k, int bw, uint32_t *applied_bw, int apply);
int e4k_enable_manual_gain(struct e4k_state *e4k, uint8_t manual);
int e4k_set_gain(struct e4k_state *e4k, int gain);
int e4k_set_i2c_register(struct e4k_state *e4k, unsigned i2c_register, unsigned data, unsigned mask);
int e4k_get_i2c_register(struct e4k_state *e4k, uint8_t *data, int *len, int *strength);
const int *e4k_get_gains(int *len);
#endif /* _E4K_TUNER_H */
