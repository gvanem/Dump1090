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

#ifndef R82XX_H
#define R82XX_H

#define R820T_I2C_ADDR		0x34
#define R828D_I2C_ADDR		0x74
#define R828D_XTAL_FREQ		16000000

#define R82XX_CHECK_ADDR	0x00
#define R82XX_CHECK_VAL		0x69

#define R82XX_IF_FREQ		3570000

#define REG_SHADOW_START	5
#define NUM_REGS			32


enum r82xx_chip {
	CHIP_R820T,
	CHIP_R620D,
	CHIP_R828D,
	CHIP_R828,
	CHIP_R828S,
	CHIP_R820C,
};

enum r82xx_tuner_type {
	TUNER_RADIO = 1,
	TUNER_ANALOG_TV,
	TUNER_DIGITAL_TV
};

struct r82xx_config {
	uint8_t i2c_addr;
	double xtal;
	enum r82xx_chip rafael_chip;
	int use_predetect;
	int	cal_imr;
};

struct r82xx_priv {
	const struct r82xx_config	*cfg;
	uint8_t						regs[NUM_REGS];
	uint32_t					int_freq;
	uint32_t					freq; //in MHz
	int16_t						abs_gain;
	uint8_t						input;
	uint8_t						old_gain;
	uint8_t						reg8[16];
	int							has_lock;
	int							imr_done;
	int							init_done;
	int							sideband;
	void 						*rtl_dev;
};

struct r82xx_freq_range {
	uint32_t	freq;
	uint8_t		rf_mux_ploy;
	uint8_t		tf_c;
};

int r82xx_standby(struct r82xx_priv *priv);
int r82xx_init(struct r82xx_priv *priv);
int r82xx_set_freq(struct r82xx_priv *priv, uint32_t freq);
int r82xx_set_gain(struct r82xx_priv *priv, int gain);
int r82xx_set_gain_mode(struct r82xx_priv *priv, int set_manual_gain);
int r82xx_set_bandwidth(struct r82xx_priv *priv, int bandwidth,  uint32_t * applied_bw, int apply);
int r82xx_set_i2c_register(struct r82xx_priv *priv, unsigned i2c_register, unsigned data, unsigned mask);
int r82xx_get_i2c_register(struct r82xx_priv *priv, unsigned char* data, int *len, int *strength);
int r82xx_set_sideband(struct r82xx_priv *priv, int sideband);
int r82xx_set_dither(struct r82xx_priv *priv, int dither);
const int *r82xx_get_gains(int *len);
#endif
