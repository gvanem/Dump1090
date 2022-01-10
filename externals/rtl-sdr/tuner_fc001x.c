/*
 * Fitipower FC0012/FC0013 tuner driver
 *
 * Copyright (C) 2012 Hans-Frieder Vogt <hfvogt@gmx.net>
 *
 * modified for use in librtlsdr
 * Copyright (C) 2012 Steve Markgraf <steve@steve-m.de>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtlsdr_i2c.h"
#include "rtl-sdr.h"
#include "tuner_fc001x.h"


static int fc001x_write(void *dev, uint8_t reg, uint8_t *buf, int len)
{
	int rc = rtlsdr_i2c_write_fn(dev, FC001X_I2C_ADDR, reg, buf, len);
	if (rc != len) {
		fprintf(stderr, "%s: i2c wr failed=%d reg=%02x len=%d\n",
			   __FUNCTION__, rc, reg, len);
		if (rc < 0)
			return rc;
		return -1;
	}

	return 0;
}

static inline int fc001x_writereg(void *dev, uint8_t reg, uint8_t val)
{
	return fc001x_write(dev, reg, &val, 1);
}

static int fc001x_read(void *dev, uint8_t reg, uint8_t *buf, int len)
{
	int rc = rtlsdr_i2c_read_fn(dev, FC001X_I2C_ADDR, reg, buf, len);
	if (rc != len) {
		fprintf(stderr, "%s: i2c rd failed=%d reg=%02x len=%d\n",
			   __FUNCTION__, rc, reg, len);
		if (rc < 0)
			return rc;
		return -1;
	}

	return 0;
}

static inline int fc001x_readreg(void *dev, uint8_t reg, uint8_t *val)
{
	return fc001x_read(dev, reg, val, 1);
}

static int fc001x_write_reg_mask(void *dev, uint8_t reg, uint8_t data, uint8_t bit_mask)
{
	int rc;
	uint8_t val;

	if(bit_mask == 0xff)
		val = data;
	else
	{
		rc = fc001x_read(dev, reg, &val, 1);
		if(rc < 0)
			return -1;
		val = (val & ~bit_mask) | (data & bit_mask);
	}
	return fc001x_writereg(dev, reg, val);
}

/*static int print_registers(void *dev)
{
	uint8_t data[22];
	unsigned int i;

	if (fc001x_read(dev, 0, data, sizeof(data)) < 0)
		return -1;
	for(i=0; i<22; i++)
		printf("%02x ", data[i]);
	printf("\n");
	return 0;
}*/


/* Incomplete list of FC0012 register settings:
 *
 * Name				Reg		BitsDesc
 * CHIP_ID			0x00	0-7	Chip ID (constant 0xA1)
 * RF_A				0x01	0-3	Number of count-to-9 cycles in RF
 *								divider (suggested: 2..9)
 * RF_M				0x02	0-4	Total number of cycles (to-8 and to-9)
 *								in RF divider
 * RF_K_HIGH		0x03	0-6	Bits 8..14 of fractional divider
 * RF_K_LOW			0x04	0-7	Bits 0..7 of fractional RF divider
 * RF_OUTDIV_A		0x05	0-7	Power of two required?
 * LNA_POWER_DOWN	0x06	0	Set to 1 to switch off low noise amp
 * RF_OUTDIV_B		0x06	1	Set to select 3 instead of 2 for the
 *								RF output divider
 * 					0x06	2	Set to 1 to switch off IF amp
 * VCO_SPEED		0x06	3	Select tuning range of VCO:
 *								 0 = Low range, (ca. 2.2 - 3.0GHz)
 *								 1 = High range (ca. 2.8 - 3.6GHz)
 * BANDWIDTH		0x06	6-7	Set bandwidth
 *								 6MHz=0x80, 7MHz=0x40, 8MHz=0x00
 * XTAL_SPEED		0x07	5	Set to 1 for 28.8MHz Crystal input
 *								or 0 for 36MHz
 * <agc params>		0x08	0-7
 *					0x09	0	1=loop_through on, 0=loop_through off
 * EN_CAL_RSSI		0x09	4 	Enable calibrate RSSI
 *								(Receive Signal Strength Indicator)
 *					0x09	7	1=Gap near 0 MHz
 *					0x0c	0-1	AGC Up-Down mode: 0=Realtek-mode
 *					0x0c	7	?
 * LNA_FORCE		0x0d	0	?
 *					0x0d	1	LNA Gain: 1=variable, 0=fixed
 * AGC_FORCE		0x0d	3	IF-AGC: 0=on, 1=off
 *					0x0d	4	1 = forcing rc_cal
 *					0x0d	7	1 = PLL loop off
 * VCO_CALIB		0x0e	7	Set high then low to calibrate VCO
 *								(fast lock?)
 * VCO_VOLTAGE		0x0e	0-6	Read Control voltage of VCO
 *								(big value -> low freq)
 * RC_cal value		0x10	0-3	rc_cal value
 * IF_GAIN			0x12	0-4	2 dB/step
 *							5-7	1 dB/step
 * LNA_GAIN			0x13	3-4	Low noise amp gain
 * 								 0x02=min, 0x08=middle, 0x10=max
 * LNA_COMPS		0x15	3	?
 */

/* Incomplete list of FC0013 register settings:
 *
 * Name				Reg		BitsDesc
 * CHIP_ID			0x00	0-7	Chip ID (constant 0xA3)
 * VHF_TRACK_EN		0x07	4	1= Enable VHF track filter
 * LNA_FORCE		0x0d	0	LNA Gain: 1=variable, 0=maximal
 * 					0x12	Mixer gain ?
 *								0x00=low, 0x0c=middle, 0x08=high, 0x0a=max
 * IF_GAIN			0x13	0-4	2 dB/step
 *							5-7	1 dB/step
 * LNA_GAIN			0x14	0-4	Low noise amp gain
 * 								 0x02=min, 0x08=middle, 0x10=max
 * BAND				0x14	5-6	0x00=VHF, 0x20=GPS, 0x40=UHF
 * VHF_TRACK_FILTER	0x1d	2-4 VHF track filter
 */

int fc0012_init(void *dev)
{
	uint8_t reg[] = {
		0x05,	/* reg. 0x01 */
		0x10,	/* reg. 0x02 */
		0x00,	/* reg. 0x03 */
		0x00,	/* reg. 0x04 */
		0x0f,	/* reg. 0x05: may also be 0x0a */
		0x80,	/* reg. 0x06: BW 6 Mhz, divider 2, VCO slow */
		0x20,	/* reg. 0x07: may also be 0x0f */
		0xff,	/* reg. 0x08: AGC Clock divide by 256, AGC gain 1/256,
			 				  Loop Bw 1/8 */
		0x6e,	/* reg. 0x09: Disable LoopThrough, Enable LoopThrough: 0x6f */
		0xb8,	/* reg. 0x0a: Disable LO Test Buffer */
		0x82,	/* reg. 0x0b: Output Clock is same as clock frequency,
			 				  may also be 0x83 */
		0xfc,	/* reg. 0x0c: depending on AGC Up-Down mode, may need 0xf8 */
		0x12,	/* reg. 0x0d: AGC Not Forcing & LNA Forcing, forcing rc_cal */
		0x00,	/* reg. 0x0e */
		0x00,	/* reg. 0x0f */
		0x00,	/* reg. 0x10 */
		0x00,	/* reg. 0x11 */
		0x1f,	/* reg. 0x12: Set to maximum gain */
		0x08,	/* reg. 0x13: Set to Middle Gain: 0x08,
							  Low Gain: 0x00, High Gain: 0x10, enable IX2: 0x80 */
		0x00,	/* reg. 0x14 */
		0x04,	/* reg. 0x15: Enable LNA COMPS */
	};
	return fc001x_write(dev, 1, reg, sizeof(reg));
}

int fc0013_init(void *dev)
{
	uint8_t reg[] = {
		0x09,	/* reg. 0x01 */
		0x16,	/* reg. 0x02 */
		0x00,	/* reg. 0x03 */
		0x00,	/* reg. 0x04 */
		0x17,	/* reg. 0x05 */
		0x02,	/* reg. 0x06: LPF bandwidth */
		0x2a,	/* reg. 0x07: CHECK */
		0xff,	/* reg. 0x08: AGC Clock divide by 256, AGC gain 1/256, Loop Bw 1/8 */
		0x6e,	/* reg. 0x09: Disable LoopThrough, Enable LoopThrough: 0x6f */
		0xb8,	/* reg. 0x0a: Disable LO Test Buffer */
		0x82,	/* reg. 0x0b: CHECK */
		0xfc,	/* reg. 0x0c: depending on AGC Up-Down mode, may need 0xf8 */
		0x11,	/* reg. 0x0d: AGC Not Forcing & LNA Forcing, forcing rc_cal */
		0x00,	/* reg. 0x0e */
		0x00,	/* reg. 0x0f */
		0x00,	/* reg. 0x10 */
		0x00,	/* reg. 0x11 */
		0x00,	/* reg. 0x12 */
		0x00,	/* reg. 0x13 */
		0x08,	/* reg. 0x14: VHF Middle Gain */
		0x01,	/* reg. 0x15 */
	};
	return fc001x_write(dev, 1, reg, sizeof(reg));
}

static int fc0013_set_vhf_track(void *dev, uint32_t freq)
{
	int ret;
	uint8_t tmp;

	ret = fc001x_readreg(dev, 0x1d, &tmp);
	if (ret)
		goto error_out;
	tmp &= 0xe3;
	if (freq <= 177500000) 		/* VHF Track: 7 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x1c);
	else if (freq <= 184500000)	/* VHF Track: 6 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x18);
	else if (freq <= 191500000)	/* VHF Track: 5 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x14);
	else if (freq <= 198500000)	/* VHF Track: 4 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x10);
	else if (freq <= 205500000)	/* VHF Track: 3 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x0c);
	else if (freq <= 219500000)	/* VHF Track: 2 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x08);
	else if (freq < 300000000) 	/* VHF Track: 1 */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x04);
	else						/* UHF and GPS */
		ret = fc001x_writereg(dev, 0x1d, tmp | 0x1c);

error_out:
	return ret;
}

static int fc001x_set_freq(void *dev, uint32_t freq, enum rtlsdr_tuner tuner_type)
{
	int ret = 0;
	uint8_t reg[7], am, pm, tmp;
	uint8_t multi;				//multi * freq = f_vco
	int64_t f_vco;				//VCO frequency
	double xtal_freq_div_2;	//14.4 MHz
	uint16_t xdiv;
	int16_t xin;
	int vco_select = 0;

	xtal_freq_div_2 = rtlsdr_get_tuner_clock(dev) / 2;

	if(tuner_type == RTLSDR_TUNER_FC0013)
	{
		/* set VHF track */
		ret = fc0013_set_vhf_track(dev, freq);
		if (ret)
			goto exit;

		if (freq < 300000000) {
			/* enable VHF filter */
			ret = fc001x_readreg(dev, 0x07, &tmp);
			if (ret)
				goto exit;
			ret = fc001x_writereg(dev, 0x07, tmp | 0x10);
			if (ret)
			goto exit;

			/* disable UHF & disable GPS */
			ret = fc001x_readreg(dev, 0x14, &tmp);
			if (ret)
				goto exit;
			ret = fc001x_writereg(dev, 0x14, tmp & 0x1f);
			if (ret)
				goto exit;
		} else {
			/* disable VHF filter */
			ret = fc001x_readreg(dev, 0x07, &tmp);
			if (ret)
				goto exit;
			ret = fc001x_writereg(dev, 0x07, tmp & 0xef);
			if (ret)
			goto exit;

			/* enable UHF & disable GPS */
			ret = fc001x_readreg(dev, 0x14, &tmp);
			if (ret)
				goto exit;
			ret = fc001x_writereg(dev, 0x14, (tmp & 0x1f) | 0x40);
			if (ret)
				goto exit;
		}
	}

	/* select frequency divider and the frequency of VCO */
	if (freq < 37084000) {			/* freq * 96 < 3560000000 */
		multi = 96;
		reg[5] = 0x82;
		reg[6] = 0x00;
	} else if (freq < 55625000) {	/* freq * 64 < 3560000000 */
		multi = 64;
		if(tuner_type == RTLSDR_TUNER_FC0012)
			reg[5] = 0x82;
		else
			reg[5] = 0x02;
		reg[6] = 0x02;
	} else if (freq < 74167000) {	/* freq * 48 < 3560000000 */
		multi = 48;
		reg[5] = 0x42;
		reg[6] = 0x00;
	} else if (freq < 111250000) {	/* freq * 32 < 3560000000 */
		multi = 32;
		if(tuner_type == RTLSDR_TUNER_FC0012)
			reg[5] = 0x42;
		else
			reg[5] = 0x82;
		reg[6] = 0x02;
	} else if (freq < 148334000) {	/* freq * 24 < 3560000000 */
		multi = 24;
		reg[5] = 0x22;
		reg[6] = 0x00;
	} else if (freq < 222500000) {	/* freq * 16 < 3560000000 */
		multi = 16;
		if(tuner_type == RTLSDR_TUNER_FC0012)
			reg[5] = 0x22;
		else
			reg[5] = 0x42;
		reg[6] = 0x02;
	} else if (freq < 296667000) {	/* freq * 12 < 3560000000 */
		multi = 12;
		reg[5] = 0x12;
		reg[6] = 0x00;
	} else if (freq < 445000000) {	/* freq * 8 < 3560000000 */
		multi = 8;
		if(tuner_type == RTLSDR_TUNER_FC0012)
			reg[5] = 0x12;
		else
			reg[5] = 0x22;
		reg[6] = 0x02;
	} else if (freq < 593334000) {	/* freq * 6 < 3560000000 */
		multi = 6;
		reg[5] = 0x0a;
		reg[6] = 0x00;
	} else {
		if(tuner_type == RTLSDR_TUNER_FC0012)
		{
			multi = 4;
			reg[5] = 0x0a;
		}
		else
		{
			if (freq < 950000000) {	/* freq * 4 < 3800000000 */
				multi = 4;
				reg[5] = 0x12;
			} else {
				multi = 2;
				reg[5] = 0x0a;
			}
		}
		reg[6] = 0x02;
	}

	f_vco = (int64_t)freq * multi;
	if (f_vco >= 3060000000U) {
		reg[6] |= 0x08;
		vco_select = 1;
	}

	/* From divided value (XDIV) determined the FA and FP value */
	xdiv = (uint16_t)(f_vco / xtal_freq_div_2);
	if ((f_vco - xdiv * xtal_freq_div_2) >= (xtal_freq_div_2 / 2))
		xdiv++;

	pm = (uint8_t)(xdiv / 8);
	am = (uint8_t)(xdiv - (8 * pm));

	if (am < 2) {
		am += 8;
		pm--;
	}

	if (pm > 31) {
		reg[1] = am + (8 * (pm - 31));
		reg[2] = 31;
	} else {
		reg[1] = am;
		reg[2] = pm;
	}

	if ((reg[1] > 15) || (reg[2] < 0x0b)) {
		fprintf(stderr, "[FC001X] no valid PLL combination "
				"found for %u Hz!\n", freq);
		return -1;
	}

	/* fix clock out */
	reg[6] |= 0x20;

	/* From VCO frequency determines the XIN (fractional part of Delta
	   Sigma PLL) and divided value (XDIV) */
	xin = (int16_t)(((f_vco % (int64_t)xtal_freq_div_2) << 15) / (int64_t)xtal_freq_div_2);
	if (xin >= 16384)
		xin -= 32768;
	reg[3] = xin >> 8;
	reg[4] = xin & 0xff;

	ret = fc001x_readreg(dev, 0x06, &tmp);
	if (ret)
		goto exit;
	reg[6] |= tmp & 0xc0; /* bits 6 and 7 describe the bandwidth */

	/* modified for Realtek demod */
	reg[5] |= 0x07;

	ret = fc001x_write(dev, 1, &reg[1], 6);
	if (ret)
		goto exit;

	if(tuner_type == RTLSDR_TUNER_FC0013)
	{
		ret = fc001x_readreg(dev, 0x11, &tmp);
		if (ret)
			goto exit;
		if (multi == 64)
			ret = fc001x_writereg(dev, 0x11, tmp | 0x04);
		else
			ret = fc001x_writereg(dev, 0x11, tmp & 0xfb);
		if (ret)
			goto exit;
	}

	/* VCO Calibration */
	ret = fc001x_writereg(dev, 0x0e, 0x80);
	if (!ret)
		ret = fc001x_writereg(dev, 0x0e, 0x00);

	/* VCO Re-Calibration if needed */
	if (!ret)
		ret = fc001x_writereg(dev, 0x0e, 0x00);

	if (!ret) {
		ret = fc001x_readreg(dev, 0x0e, &tmp);
	}
	if (ret)
		goto exit;

	/* vco selection */
	tmp &= 0x3f;

	if (vco_select) {
		if (tmp > 0x3c) {
			reg[6] &= ~0x08;
			ret = fc001x_writereg(dev, 0x06, reg[6]);
			if (!ret)
				ret = fc001x_writereg(dev, 0x0e, 0x80);
			if (!ret)
				ret = fc001x_writereg(dev, 0x0e, 0x00);
		}
	} else {
		if (tmp < 0x02) {
			reg[6] |= 0x08;
			ret = fc001x_writereg(dev, 0x06, reg[6]);
			if (!ret)
				ret = fc001x_writereg(dev, 0x0e, 0x80);
			if (!ret)
				ret = fc001x_writereg(dev, 0x0e, 0x00);
		}
	}
#if 1
	{
		int64_t actual_vco = xtal_freq_div_2 * xdiv + xtal_freq_div_2 * xin / 32768;
		int tuning_error = (f_vco - actual_vco) / multi;
		//printf("f_vco=%llu, xin=%d, xdiv=%u, am=%u, pm=%u\n", f_vco, xin, xdiv, am, pm);
		//printf("actual_vco = %lld, tuning_error = %d\n", actual_vco, tuning_error);
		ret = rtlsdr_set_if_freq(dev, tuning_error);
	}
#endif

exit:
	return ret;
}

int fc0012_set_freq(void *dev, uint32_t freq) {
	// select V-band/U-band filter
	rtlsdr_set_gpio_bit(dev, 6, (freq > 300000000) ? 1 : 0);
	return fc001x_set_freq(dev, freq, RTLSDR_TUNER_FC0012);
}

int fc0013_set_freq(void *dev, uint32_t freq) {
	return fc001x_set_freq(dev, freq, RTLSDR_TUNER_FC0013);
}

int fc001x_set_gain_mode(void *dev, int manual)
{
	return fc001x_write_reg_mask(dev, 0x0d, manual ? 8 : 0, 0x08);
}


static const uint8_t lna_gains[]=
//LNA dB
//0    0    0    0    3.6  7.4  7.4  7.4  17   17   17  27.8 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6 29.6
{0x02,0x02,0x02,0x02,0x00,0x1e,0x1e,0x1e,0x08,0x08,0x08,0x17,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10};
static const uint8_t if_gains[]=
//IF dB
//0    3.1  6.5 10.3 10.3 10.3 14.3 18.3 12.3 16.3 20.3 12.3 14.3 18.3 22.3 26.3 30.3 34.3 38.3 42.3 46.3 50.3 54.3 58.3 62.3 66.3 70.3
{0x80,0x40,0x20,0x01,0x01,0x01,0x03,0x05,0x02,0x04,0x06,0x02,0x03,0x05,0x07,0x09,0x0b,0x0d,0x0f,0x11,0x13,0x15,0x17,0x19,0x1b,0x1d,0x1f};
static const int fc001x_gains[] =
//Total dB*10
{  0,  31,  65, 103, 139, 177, 217, 257, 293, 333, 373, 401, 439, 479, 519, 559, 599, 639, 679, 719, 759, 799, 839, 879, 919, 959, 999 };

#define GAIN_CNT	(sizeof(fc001x_gains) / sizeof(int))

static int fc001x_set_gain(void *dev, int gain, uint8_t if_reg, uint8_t lna_reg)
{
	unsigned int i;
	uint8_t gain_mode;

	int ret = fc001x_readreg(dev, 0x0d, &gain_mode);
	if (ret < 0)
		return ret;
	if((gain_mode & 8) == 0)
		return 0; //don't set gain in AGC mode
	if(if_reg == 0x13)
		ret = fc001x_writereg(dev, 0x12, 0); //set FC0013 mixer gain to 0
	for (i = 0; i < GAIN_CNT; i++)
		if ((fc001x_gains[i] >= gain) || (i+1 == GAIN_CNT))
			break;
	ret |= fc001x_writereg(dev, if_reg, if_gains[i]);
	ret |= fc001x_write_reg_mask(dev, lna_reg, lna_gains[i], 0x1f);
	//print_registers(dev);
	return ret;
}

int fc0012_set_gain(void *dev, int gain)
{
	return fc001x_set_gain(dev, gain, 0x12, 0x13);
}

int fc0013_set_gain(void *dev, int gain)
{
	return fc001x_set_gain(dev, gain, 0x13, 0x14);
}

int fc001x_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply)
{
	uint8_t data;

	if (bw < 5300000)
	{
		*applied_bw = 5000000;
		data = 0x80;
	}
	else if (bw < 6000000)
	{
		*applied_bw = 5600000;
		data = 0x40;
	}
	else
	{
		*applied_bw = 6400000;
		data = 0x00;
	}
	if(!apply)
		return 0;

	return fc001x_write_reg_mask(dev, 0x06, data, 0xc0);
}

int fc0012_exit(void *dev) {
	// switch off tuner
	rtlsdr_set_gpio_bit(dev, 4, 1);
	return 0;
}

int fc0013_exit(void *dev) {
	// switch off LNA and IF amp
	return fc001x_write_reg_mask(dev, 0x06, 0x05, 0x05);
}

/* expose/permit tuner specific i2c register hacking! */
int fc001x_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask)
{
	return fc001x_write_reg_mask(dev, i2c_register & 0xFF, data & 0xff, mask & 0xff);
}

static const int lna_gain_table[] = {
	/* low gain */
	-63, -58, -99, -73,	-63, -65, -54, -60,
	/* middle gain */
	 71,  70,  68,  67,	 65,  63,  61,  58,
	/* high gain */
	197, 191, 188, 186,	184, 182, 181, 179,
	/* low gain */
	-28, -27, -43, -32, -27, -28, -25, -27
};
static const int if_gain_table[] = { 83, 65, 31, 48, 0, 0, 13, 0};

static const int mix_gain_table[] = { 0,  0,   0,   0, 21, 21, 21, 21,
									 60, 60, 122, 100, 42, 42, 42, 42};

static int fc001x_get_signal_strength(uint8_t vga, uint8_t lna, uint8_t mix)
{
	int if_gain = (vga & 0x1f) * 20;
	int lna_gain = lna_gain_table[lna & 0x1f];
	int mix_gain = mix_gain_table[mix & 0x0f] + ((mix >> 4) & 3) * 6;
	if_gain += if_gain_table[(vga >> 5) & 0x07];
	return if_gain + lna_gain + mix_gain + 80;
}

static int fc001x_get_i2c_register(void *dev, unsigned char* data, int *len, int *tuner_gain,
									uint8_t if_reg, uint8_t lna_reg)
{
	int rc;
	uint8_t mixer, gain_mode;

	rc = fc001x_readreg(dev, 0x0d, &gain_mode);
	if (rc < 0)
		return rc;
	if((gain_mode & 8) == 0) //AGC mode
	{
		if(if_reg == 0x13) //FC0013
		{
			rc = fc001x_writereg(dev, 0x12, 0); //mixer gain
			if (rc < 0)
				return rc;
		}
		rc = fc001x_writereg(dev, if_reg, 0);
		if (rc < 0)
			return rc;
	}
	rc = fc001x_read(dev, 0, data, *len);
	if (rc < 0)
		return rc;
	if(if_reg == 0x13) //FC0013
		mixer = data[0x12];
	else
		mixer = 0;
	*tuner_gain = fc001x_get_signal_strength(data[if_reg], data[lna_reg], mixer);
	if((gain_mode & 8) == 0) //Set LAN gain in AGC mode
	{
		int lna = data[lna_reg] & 0x1f;
		if(lna == 0x10)
		{
			if(*tuner_gain < 450)
				lna = 0x08;
		}
		else if(lna == 0x02)
		{
			if(*tuner_gain > 200)
				lna = 0x08;
		}
		else // 0x08
		{
			if (*tuner_gain > 500)
				lna = 0x10;
			else if (*tuner_gain < 150)
				lna = 0x02;
		}
		if(lna != (data[lna_reg] & 0x1f))
		{
			rc = fc001x_write_reg_mask(dev, lna_reg, lna, 0x1f);
			//printf("tuner_gain=%d, lna=%x\n",*tuner_gain,lna);
		}
		return rc;
	}
	return 0;
}

int fc0012_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength)
{
	*len = 22;
	return fc001x_get_i2c_register(dev, data, len, strength, 0x12, 0x13);
}

int fc0013_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength)
 {
	*len = 30;
	return fc001x_get_i2c_register(dev, data, len, strength, 0x13, 0x14);
}

const int *fc001x_get_gains(int *len)
{
	*len = sizeof(fc001x_gains);
	return fc001x_gains;
}
