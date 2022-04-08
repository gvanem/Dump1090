// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FCI FC2580 silicon tuner driver
 *
 * Copyright (C) 2012 Antti Palosaari <crope@iki.fi>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "rtlsdr_i2c.h"
#include "rtl-sdr.h"
#include "tuner_fc2580.h"

typedef enum {
	FC2580_NO_BAND,
	FC2580_VHF_BAND,
	FC2580_UHF_BAND,
	FC2580_L_BAND
} fc2580_band_type;

struct fc2580_reg_val {
	uint8_t reg;
	uint8_t val;
};

static const struct fc2580_reg_val fc2580_init_reg_vals[] = {
	{0x00, 0x00},
	{0x12, 0x86},
	{0x14, 0x5c},
	{0x16, 0x3c},
	{0x1f, 0xd2},
	{0x09, 0xd7},
	{0x0b, 0xd5},
	{0x0c, 0x32},
	{0x0e, 0x43},
	{0x21, 0x0a},
	{0x22, 0x82},
	{0x45, 0x10}, //internal AGC
	{0x4c, 0x00},
	{0x3f, 0x88},
	{0x02, 0x0e},
	{0x58, 0x14},
	{0x6b, 0x11}, //threshold VGA
	{0x6c, 0x13}  //threshold VGA
};

struct fc2580_pll {
	uint32_t freq;
	uint8_t div_out;
	uint8_t band;
};

static const struct fc2580_pll fc2580_pll_lut[] = {
	/*                            VCO min    VCO max */
	{ 400000000, 12, 0x80}, /* .......... 4800000000 */
	{1000000000,  4, 0x00}, /* 1600000000 4000000000 */
	{0xffffffff,  2, 0x40}, /* 2000000000 .......... */
};

struct fc2580_freq_regs {
	uint32_t freq;
	uint8_t r25_val;
	uint8_t r27_val;
	uint8_t r28_val;
	uint8_t r29_val;
	uint8_t r2b_val;
	uint8_t r2c_val;
	uint8_t r2d_val;
	uint8_t r30_val;
	uint8_t r44_val;
	uint8_t r50_val;
	uint8_t r53_val;
	uint8_t r5f_val;
	uint8_t r61_val;
	uint8_t r62_val;
	uint8_t r63_val;
	uint8_t r67_val; //threshold LNA
	uint8_t r68_val; //threshold LNA
	uint8_t r69_val;
	uint8_t r6a_val;
	uint8_t r6d_val;
	uint8_t r6e_val;
	uint8_t r6f_val;
};

/* XXX: 0xff is used for don't-care! */
static const struct fc2580_freq_regs fc2580_freq_regs_lut[] = {
	{ 400000000,
		0xff, 0x77, 0x33, 0x40, 0xff, 0xff, 0xff, 0x09, 0xff, 0x8c,
		0x50, 0x0f, 0x07, 0x00, 0x15, 0x03, 0x05, 0x10, 0x12, 0x78,
		0x32, 0x54},
	{ 538000000,
		0xf0, 0x77, 0x53, 0x60, 0xff, 0xff, 0x9f, 0x09, 0xff, 0x8c,
		0x50, 0x13, 0x07, 0x06, 0x15, 0x06, 0x08, 0x10, 0x12, 0x78,
		0x32, 0x14},
	{ 794000000,
		0xf0, 0x77, 0x53, 0x60, 0xff, 0xff, 0x9f, 0x09, 0xff, 0x8c,
		0x50, 0x15, 0x03, 0x03, 0x15, 0x03, 0x05, 0x0c, 0x0e, 0x78,
		0x32, 0x14},
	{1000000000,
		0xf0, 0x77, 0x53, 0x60, 0xff, 0xff, 0x8f, 0x09, 0xff, 0x8c,
		0x50, 0x15, 0x07, 0x06, 0x15, 0x07, 0x09, 0x10, 0x12, 0x78,
		0x32, 0x14},
	{0xffffffff,
		0xff, 0xff, 0xff, 0xff, 0x70, 0x37, 0xe7, 0x09, 0x20, 0x8c,
		0x50, 0x0f, 0x0f, 0x00, 0x13, 0x00, 0x02, 0x0c, 0x0e, 0xa0,
		0x50, 0x14},
};

static uint8_t band = FC2580_NO_BAND;

/* glue functions to rtl-sdr code */

/*
 * TODO:
 * I2C write and read works only for one single register. Multiple registers
 * could not be accessed using normal register address auto-increment.
 * There could be (very likely) register to change that behavior....
 */
static int fc2580_write(void *dev, unsigned char reg, unsigned char val)
{
	int rc = rtlsdr_i2c_write_fn(dev, FC2580_I2C_ADDR, reg, &val, 1);
	if (rc != 1) {
		printf( "%s: i2c wr failed=%d reg=%02x len=1\n",
			   __FUNCTION__, rc, reg);
		if (rc < 0)
			return rc;
		return -1;
	}

	return 0;
}

/* write single register conditionally only when value differs from 0xff
 * XXX: This is special routine meant only for writing fc2580_freq_regs_lut[]
 * values. Do not use for the other purposes. */
static int fc2580_wr_reg_ff(void *dev, uint8_t reg, uint8_t val)
{
	if (val == 0xff)
		return 0;
	else
		return fc2580_write(dev, reg, val);
}

static int fc2580_read(void *dev, unsigned char reg, unsigned char *data)
{
	int rc = rtlsdr_i2c_read_fn(dev, FC2580_I2C_ADDR, reg, data, 1);
	if (rc != 1) {
		printf( "%s: i2c wr failed=%d reg=%02x len=1\n",
			   __FUNCTION__, rc, reg);
		if (rc < 0)
			return rc;
		return -1;
	}

	return 0;
}

static int fc2580_write_reg_mask(void *dev, uint8_t reg, uint8_t data, uint8_t bit_mask)
{
	int rc;
	uint8_t val;

	if(bit_mask == 0xff)
		val = data;
	else
	{
		rc = fc2580_read(dev, reg, &val);
		if(rc < 0)
			return -1;
		val = (val & ~bit_mask) | (data & bit_mask);
	}
	return fc2580_write(dev, reg, val);
}

int fc2580_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask)
{
	return fc2580_write_reg_mask(dev, i2c_register & 0xFF, data & 0xff, mask & 0xff);
}

/*==============================================================================
       fc2580 RSSI function

  The following context is source code provided by FCI.

  This function returns fc2580's current RSSI value.

  <input parameter>
  unsigned char *data

  <return value>
  int rssi : estimated input power.

==============================================================================*/
static int fc2580_get_rssi(unsigned char *data)
{
	#define OFS_RSSI  57
	uint8_t s_lna =   data[0x71];
	uint8_t s_rfvga = data[0x72];
	uint8_t s_cfs =   data[0x73];
	uint8_t s_ifvga = data[0x74];
  	int ofs_lna =
			(band==FC2580_VHF_BAND)?
				(s_lna==0)? 0 :
				(s_lna==1)? -6 :
				(s_lna==2)? -19 :
				(s_lna==3)? -24 : -32 :
			(band==FC2580_UHF_BAND)?
				(s_lna==0)? 0 :
				(s_lna==1)? -6 :
				(s_lna==2)? -17 :
				(s_lna==3)? -22 : -30 :
			(band==FC2580_L_BAND)?
				(s_lna==0)? 0 :
				(s_lna==1)? -6 :
				(s_lna==2)? -11 :
				(s_lna==3)? -16 : -34 :
			0; //FC2580_NO_BAND
	int ofs_rfvga = -s_rfvga+((s_rfvga>=11)? 1 : 0) + ((s_rfvga>=18)? 1 : 0);
	int ofs_csf = -6*(s_cfs & 7);
	int ofs_ifvga = s_ifvga/4;
	int rssi = ofs_lna+ofs_rfvga+ofs_csf+ofs_ifvga+OFS_RSSI;
	//printf("rssi=%d, ofs_lna=%d, ofs_rfvga=%d, ofs_csf=%d, ofs_ifvga=%d\n",
	//	rssi, ofs_lna, ofs_rfvga, ofs_csf, ofs_ifvga);
	return rssi;
}

int fc2580_get_i2c_register(void *dev, uint8_t *data, int *len, int *strength)
{
	int rc, i;

	*len = 128;
	*strength = 0;
	rc = 0;
	for(i=0; i<128; i++)
	{
		rc = fc2580_read(dev, i, &data[i]);
		if (rc < 0)
			return rc;
	}
	*strength = 10 * fc2580_get_rssi(data);
	return 0;
}

/*static int print_registers(void *dev)
{
	uint8_t data = 0;
	unsigned int i, j;

	printf("   0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	for(i=0; i<8; i++)
	{
		printf("%01x ", i);
		for(j=0; j<16; j++)
		{
			fc2580_read(dev, i*16+j, &data);
			printf("%02x ", data);
		}
		printf("\n");
	}
	return 0;
}*/

int fc2580_init(void *dev)
{
	int ret;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fc2580_init_reg_vals); i++) {
		ret = fc2580_write(dev, fc2580_init_reg_vals[i].reg,
				fc2580_init_reg_vals[i].val);
		if (ret)
			goto err;
	}
	//print_registers(dev);
	return 0;
err:
	printf( "%s: failed=%d\n", __FUNCTION__, ret);
	return ret;
}

int fc2580_set_freq(void *dev, unsigned int frequency)
{
	unsigned int i, uitmp, div_ref, div_ref_val, div_n, k_cw, div_out;
	uint64_t f_vco;
	uint8_t synth_config;
	int ret = 0;
	double freq_xtal = rtlsdr_get_tuner_clock(dev);

	/*
	 * Fractional-N synthesizer
	 *
	 *                      +---------------------------------------+
	 *                      v                                       |
	 *  Fref   +----+     +----+     +-------+         +----+     +------+     +---+
	 * ------> | /R | --> | PD | --> |  VCO  | ------> | /2 | --> | /N.F | <-- | K |
	 *         +----+     +----+     +-------+         +----+     +------+     +---+
	 *                                 |
	 *                                 |
	 *                                 v
	 *                               +-------+  Fout
	 *                               | /Rout | ------>
	 *                               +-------+
	 */
	band = (frequency > 1000000000UL)? FC2580_L_BAND : (frequency > 400000000UL)? FC2580_UHF_BAND : FC2580_VHF_BAND;
	for (i = 0; i < ARRAY_SIZE(fc2580_pll_lut); i++) {
		if (frequency <= fc2580_pll_lut[i].freq)
			break;
	}
	if (i == ARRAY_SIZE(fc2580_pll_lut)) {
		ret = -1;
		goto err;
	}

	#define DIV_PRE_N 2
	div_out = fc2580_pll_lut[i].div_out;
	f_vco = (uint64_t)frequency * div_out;
	synth_config = fc2580_pll_lut[i].band;
	if (f_vco < 2600000000ULL)
		synth_config |= 0x06;
	else
		synth_config |= 0x0e;

	/* select reference divider R (keep PLL div N in valid range) */
	#define DIV_N_MIN 76
	if (f_vco >= (uint64_t)(DIV_PRE_N * DIV_N_MIN * freq_xtal)) {
		div_ref = 1;
		div_ref_val = 0x00;
	} else if (f_vco >= (uint64_t)(DIV_PRE_N * DIV_N_MIN * freq_xtal / 2)) {
		div_ref = 2;
		div_ref_val = 0x10;
	} else {
		div_ref = 4;
		div_ref_val = 0x20;
	}

	/* calculate PLL integer and fractional control word */
	uitmp = DIV_PRE_N * freq_xtal / div_ref;
	div_n = f_vco / uitmp;
	k_cw = (f_vco % uitmp) * 0x100000 / uitmp;

#if 0
	printf(	"frequency=%u f_vco=%llu freq_xtal=%.1f div_ref=%u div_n=%u div_out=%u k_cw=%0x\n",
		frequency, f_vco, freq_xtal, div_ref, div_n, div_out, k_cw);
#endif

	ret |= fc2580_write(dev, 0x02, synth_config);
	ret |= fc2580_write(dev, 0x18, div_ref_val << 0 | k_cw >> 16);
	ret |= fc2580_write(dev, 0x1a, (k_cw >> 8) & 0xff);
	ret |= fc2580_write(dev, 0x1b, (k_cw >> 0) & 0xff);
	ret |= fc2580_write(dev, 0x1c, div_n);
	if (ret)
		goto err;

	/* registers */
	for (i = 0; i < ARRAY_SIZE(fc2580_freq_regs_lut); i++) {
		if (frequency <= fc2580_freq_regs_lut[i].freq)
			break;
	}
	if (i == ARRAY_SIZE(fc2580_freq_regs_lut)) {
		ret = -1;
		goto err;
	}
	ret |= fc2580_wr_reg_ff(dev, 0x25, fc2580_freq_regs_lut[i].r25_val);
	ret |= fc2580_wr_reg_ff(dev, 0x27, fc2580_freq_regs_lut[i].r27_val);
	ret |= fc2580_wr_reg_ff(dev, 0x28, fc2580_freq_regs_lut[i].r28_val);
	ret |= fc2580_wr_reg_ff(dev, 0x29, fc2580_freq_regs_lut[i].r29_val);
	ret |= fc2580_wr_reg_ff(dev, 0x2b, fc2580_freq_regs_lut[i].r2b_val);
	ret |= fc2580_wr_reg_ff(dev, 0x2c, fc2580_freq_regs_lut[i].r2c_val);
	ret |= fc2580_wr_reg_ff(dev, 0x2d, fc2580_freq_regs_lut[i].r2d_val);
	ret |= fc2580_wr_reg_ff(dev, 0x30, fc2580_freq_regs_lut[i].r30_val);
	ret |= fc2580_wr_reg_ff(dev, 0x44, fc2580_freq_regs_lut[i].r44_val);
	ret |= fc2580_wr_reg_ff(dev, 0x50, fc2580_freq_regs_lut[i].r50_val);
	ret |= fc2580_wr_reg_ff(dev, 0x53, fc2580_freq_regs_lut[i].r53_val);
	ret |= fc2580_wr_reg_ff(dev, 0x5f, fc2580_freq_regs_lut[i].r5f_val);
	ret |= fc2580_wr_reg_ff(dev, 0x61, fc2580_freq_regs_lut[i].r61_val);
	ret |= fc2580_wr_reg_ff(dev, 0x62, fc2580_freq_regs_lut[i].r62_val);
	ret |= fc2580_wr_reg_ff(dev, 0x63, fc2580_freq_regs_lut[i].r63_val);
	ret |= fc2580_wr_reg_ff(dev, 0x67, fc2580_freq_regs_lut[i].r67_val);
	ret |= fc2580_wr_reg_ff(dev, 0x68, fc2580_freq_regs_lut[i].r68_val);
	ret |= fc2580_wr_reg_ff(dev, 0x69, fc2580_freq_regs_lut[i].r69_val);
	ret |= fc2580_wr_reg_ff(dev, 0x6a, fc2580_freq_regs_lut[i].r6a_val);
	ret |= fc2580_wr_reg_ff(dev, 0x6d, fc2580_freq_regs_lut[i].r6d_val);
	ret |= fc2580_wr_reg_ff(dev, 0x6e, fc2580_freq_regs_lut[i].r6e_val);
	ret |= fc2580_wr_reg_ff(dev, 0x6f, fc2580_freq_regs_lut[i].r6f_val);
	if (ret)
		goto err;

	return 0;
err:
	printf( "%s: failed=%d\n", __FUNCTION__, ret);
	return ret;
}

/*==============================================================================
       fc2580 filter BW setting

  This function changes Bandwidth frequency of fc2580's channel selection filter

  <input parameter>
  filter_bw
    1 : 1.53MHz(TDMB)
	6 : 6MHz   (Bandwidth 6MHz)
	7 : 6.8MHz (Bandwidth 7MHz)
	8 : 7.8MHz (Bandwidth 8MHz)
==============================================================================*/
static int fc2580_set_filter(void *dev, unsigned char filter_bw)
{
	// Set tuner bandwidth mode.
	unsigned int freq_xtal = (rtlsdr_get_tuner_clock(dev) + 500) / 1000;
	unsigned char cal_mon = 0, i;
	int result = 0;

	switch (filter_bw) {
	case 1: //1530 kHz
		result |= fc2580_write(dev, 0x36, 0x1C);
		result |= fc2580_write(dev, 0x37, (unsigned char)(4151*freq_xtal/1000000) );
		result |= fc2580_write(dev, 0x39, 0x00);
		break;
	case 2: //2000 kHz
		result |= fc2580_write(dev, 0x36, 0x1C);
		result |= fc2580_write(dev, 0x37, (unsigned char)(3000*freq_xtal/1000000) );
		result |= fc2580_write(dev, 0x39, 0x00);
		break;
	case 5: //5400 kHz
		result |= fc2580_write(dev, 0x36, 0x18);
		result |= fc2580_write(dev, 0x37, (unsigned char)(4400*freq_xtal/1000000) );
		result |= fc2580_write(dev, 0x39, 0x00);
		break;
	case 6: //6300 kHz
		result |= fc2580_write(dev, 0x36, 0x18);
		result |= fc2580_write(dev, 0x37, (unsigned char)(3910*freq_xtal/1000000) );
		result |= fc2580_write(dev, 0x39, 0x80);
		break;
	default:
	case 7: //7200 kHz
		result |= fc2580_write(dev, 0x36, 0x18);
		result |= fc2580_write(dev, 0x37, (unsigned char)(3300*freq_xtal/1000000) );
		result |= fc2580_write(dev, 0x39, 0x80);
		break;
	}
	result |= fc2580_write(dev, 0x2E, 0x09);

	for(i=0; i<5; i++)
	{
		result &= fc2580_read(dev, 0x2F, &cal_mon);
		if( (cal_mon & 0xC0) != 0xC0)
		{
			result |= fc2580_write(dev, 0x2E, 0x01);
			result |= fc2580_write(dev, 0x2E, 0x09);
		}
		else
			break;
	}
	result |= fc2580_write(dev, 0x2E, 0x01);

	return result;
}

int fc2580_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply)
{

	if (bw < 1800000)
		*applied_bw = 1530000;
	else if (bw < 3000000)
		*applied_bw = 2100000;
	else if (bw < 6000000)
		*applied_bw = 5400000;
	else if (bw < 7000000)
		*applied_bw = 6300000;
	else
		*applied_bw = 7200000;
	if(!apply)
		return 0;
	return fc2580_set_filter(dev, *applied_bw/1000000);
}

int fc2580_exit(void *dev)
{
	rtlsdr_set_gpio_bit(dev, 4, 1);
	return 0;
}

