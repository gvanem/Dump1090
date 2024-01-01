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

/*
Registers

Reg		Bitmap	Description
------------------------------------------------------------------------------
R1		[7:0]	CHIP_ID, reference check point for read mode: 0x56
0x01
------------------------------------------------------------------------------
R2		[7:6]	Band switch and VCO divider
0x02			00: UHF, PLL = VCO/4
				01: L-Band, PLL = VCO/2
				10: VHF, PLL = VCO/12
		[5]		0: Use internal XTAL Oscillator, 1: Use External Clock input
		[3]		0: Lower VCO band < 2.6 GHz, 1: upper VCO band > 2.6 GHz
		[2]		0: Standby, 1: active
------------------------------------------------------------------------------
R24		[6:4]	Reference divider R, 00: /1, 01: /2, 10: /4
0x18 	[3:0]	High part of 'K' value
------------------------------------------------------------------------------
R26				Middle part of 'K' value
0x1a
------------------------------------------------------------------------------
R27				Lower part of 'K' value
0x1b
------------------------------------------------------------------------------
R28				'N' value
0x1c
------------------------------------------------------------------------------
R46				Command register for filter PLL
0x2e
------------------------------------------------------------------------------
R47		[7:6]	Filter PLL status, 11: Filter PLL in sync
0x2f	[5:0]	Filter PLL value
------------------------------------------------------------------------------
R54		[6]		Filter bandwidth course tune
0x36			0: Wide bandwidth, 4...10 MHz
				1: Small bandwidth, 1...3 MHz
------------------------------------------------------------------------------
R55				Filter bandwidth fine tune
0x37
------------------------------------------------------------------------------
R69		[5:4]	00: Manual VGA gain control
0x45			01: Internal AGC mode
				10: Voltage controlled AGC mode
------------------------------------------------------------------------------
R73		[2:0]	IF manual attenuator control * -6 dB
0x49
------------------------------------------------------------------------------
R74				IF manual gain control * 0.25 dB
0x4a
------------------------------------------------------------------------------
R75				AGC clock's pre-divide ratio
0x4b
------------------------------------------------------------------------------
R76		[1]		0: Internal IF AGC mode
0x4c			1: Voltage controlled IF AGC mode
------------------------------------------------------------------------------
R103-R106		LNA and Mixer agc power detector voltage threshold
0x67-0x6a		low and high settings
------------------------------------------------------------------------------
R107			IF agc power detector voltage threshold low setting
0x6b
------------------------------------------------------------------------------
R108			IF agc power detector voltage threshold high setting
0x6c
------------------------------------------------------------------------------
R113			Indication of LNA gain
0x71
------------------------------------------------------------------------------
R114			Indication of Mixer gain
0x72
------------------------------------------------------------------------------
R115	[2:0]	Indication of IF attenuator * -6 dB
0x73
------------------------------------------------------------------------------
R116			Indication of IF gain * 0.25 dB
0x74
------------------------------------------------------------------------------
*/

typedef enum {
	FC2580_VHF_BAND,
	FC2580_UHF1_BAND,
	FC2580_UHF2_BAND,
	FC2580_UHF3_BAND,
	FC2580_L_BAND
} fc2580_band_type;

static fc2580_band_type curr_band = FC2580_VHF_BAND;

static const struct {
	uint8_t reg;
	uint8_t val;
} fc2580_reg_val[] = {
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
	{0x25, 0xf0}, //for UHF
	{0x27, 0x77}, //for VHF and UHF
	{0x2b, 0x70}, //for L-Band
	{0x2c, 0x37}, //for L-Band
	{0x30, 0x09},
	{0x44, 0x20}, //for L-Band
	{0x50, 0x8c},
	{0x53, 0x50},
	{0x58, 0x14},
	{0x6b, 0x11}, //threshold VGA
	{0x6c, 0x13}  //threshold VGA
};

static const struct {
	uint32_t freq;
	uint8_t div_out;
	uint8_t band;
} fc2580_pll[] = {
	{ 400000000, 12, 0x80}, // VHF
	{ 538000000,  4, 0x00}, // UHF1
	{ 794000000,  4, 0x00}, // UHF2
	{ 925000000,  4, 0x00}, // UHF3
	{0xffffffff,  2, 0x40}  // L-Band
};

static const struct {
	uint8_t reg;
	uint8_t val[5];
} fc2580_freq_regs[] = {
	{0x28, {0x33,0x53,0x53,0x53,0xff}},
	{0x29, {0x40,0x60,0x60,0x60,0xff}},
	{0x2d, {0xff,0x9f,0x9f,0x8f,0xe7}}, // UHF LNA Load Cap
	{0x5f, {0x0f,0x13,0x15,0x15,0x0f}},
	{0x61, {0x07,0x07,0x03,0x07,0x0f}},
	{0x62, {0x00,0x06,0x03,0x06,0x00}},
	{0x63, {0x15,0x15,0x15,0x15,0x13}},
	{0x67, {0x03,0x06,0x03,0x07,0x00}},
	{0x68, {0x05,0x08,0x05,0x09,0x02}},
	{0x69, {0x10,0x10,0x0c,0x10,0x0c}},
	{0x6a, {0x12,0x12,0x0e,0x12,0x0e}},
	{0x6d, {0x78,0x78,0x78,0x78,0xa0}},
	{0x6e, {0x32,0x32,0x32,0x32,0x50}},
	{0x6f, {0x54,0x14,0x14,0x14,0x14}},
};


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
 * XXX: This is special routine meant only for writing fc2580_freq_regs[]
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

static const int lna_gains[][5] = {
	{  0,   0,   0,   0,   0},
	{ -6,  -6,  -6,  -6,  -6},
	{-19, -17, -17, -17, -11},
	{-24, -22, -22, -22, -16},
	{-32, -30, -30, -30, -34},
};

/*==============================================================================

  This function returns fc2580's current RSSI value.

  <input parameter>
  unsigned char *data: fc2580's registers

  <return value>
  int rssi : estimated input power in 1/10 dB.

==============================================================================*/
static int fc2580_get_rssi(unsigned char *data)
{
	#define OFS_RSSI  57
	uint8_t s_lna =   data[0x71];
	uint8_t s_rfvga = data[0x72];
	uint8_t s_cfs =   data[0x73];
	uint8_t s_ifvga = data[0x74];
	int ofs_lna = lna_gains[s_lna][curr_band];
	int ofs_rfvga = -s_rfvga+((s_rfvga>=11)? 1 : 0) + ((s_rfvga>=18)? 1 : 0);
	int ofs_csf = -6*(s_cfs & 7);
	int ofs_ifvga = s_ifvga*10/4;
	int rssi = (OFS_RSSI+ofs_lna+ofs_rfvga+ofs_csf) * 10 + ofs_ifvga;
	//printf("rssi=%d, ofs_lna=%d, ofs_rfvga=%d, ofs_csf=%d, ofs_ifvga=%d\n",
	//	rssi, ofs_lna, ofs_rfvga, ofs_csf, ofs_ifvga);
	return rssi;
}

int fc2580_get_i2c_register(void *dev, uint8_t *data, int *len, int *strength)
{
	int rc, i;

	*len = 118;
	*strength = 0;
	rc = 0;
	for(i=0; i<118; i++)
	{
		rc = fc2580_read(dev, i, &data[i]);
		if (rc < 0)
			return rc;
	}
	*strength = fc2580_get_rssi(data);
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

	for (i = 0; i < ARRAY_SIZE(fc2580_reg_val); i++) {
		ret = fc2580_write(dev, fc2580_reg_val[i].reg,
				fc2580_reg_val[i].val);
		if (ret)
			goto err;
	}
	for (i = 0; i < ARRAY_SIZE(fc2580_freq_regs); i++){
		ret = fc2580_wr_reg_ff(dev, fc2580_freq_regs[i].reg,
				fc2580_freq_regs[i].val[curr_band]);
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
	uint8_t synth_config = 0;
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
	for (i = 0; i < ARRAY_SIZE(fc2580_pll); i++) {
		if (frequency <= fc2580_pll[i].freq)
			break;
	}
	if (i == ARRAY_SIZE(fc2580_pll)) {
		ret = -1;
		goto err;
	}

	#define DIV_PRE_N 2
	div_out = fc2580_pll[i].div_out;
	f_vco = (uint64_t)frequency * div_out;
	synth_config |= fc2580_pll[i].band;
	if (f_vco < 2600000000ULL) //Select VCO Band
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
	ret |= fc2580_write(dev, 0x18, div_ref_val << 0 | k_cw >> 16); //Load 'R' value and high part of 'K' value
	ret |= fc2580_write(dev, 0x1a, (k_cw >> 8) & 0xff); //Load middle part of 'K' value
	ret |= fc2580_write(dev, 0x1b, (k_cw >> 0) & 0xff); //Load lower part of 'K' value
	ret |= fc2580_write(dev, 0x1c, div_n); //Load 'N' value
	if (ret)
		goto err;

	/* registers */
	if(curr_band != (int)i)
	{
		curr_band = i;
		for (i = 0; i < ARRAY_SIZE(fc2580_freq_regs); i++)
			ret |= fc2580_wr_reg_ff(dev, fc2580_freq_regs[i].reg, fc2580_freq_regs[i].val[curr_band]);
	}
	if (ret)
		goto err;

	return 0;
err:
	printf( "%s: failed=%d\n", __FUNCTION__, ret);
	return ret;
}

/*==============================================================================

  This function changes Bandwidth frequency of fc2580's channel selection filter

  <input parameter>
  filter_bw: bandwidth in kHz

 ==============================================================================*/
static int fc2580_set_filter(void *dev, int filter_bw)
{
	// Set tuner bandwidth mode.
	unsigned int freq_xtal = (rtlsdr_get_tuner_clock(dev) + 500) / 1000;
	unsigned char cal_mon = 0, i;
	int result = 0;

	result |= fc2580_write(dev, 0x36, 0x1C);
	result |= fc2580_write(dev, 0x37, (uint8_t)(63*freq_xtal/filter_bw/10) );
	result |= fc2580_write(dev, 0x39, 0x00);
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
	if (bw < 1050000)
		*applied_bw = 1050000;
	else if (bw <= 2900000)
		*applied_bw = bw;
	else
		*applied_bw = 2900000;
	if(!apply)
		return 0;
	return fc2580_set_filter(dev, *applied_bw/1000);
}

int fc2580_set_gain_mode(void *dev, int manual)
{
	return fc2580_write(dev, 0x45, manual ? 0 : 0x10);
}

static const int fc2580_gains[] = {
//Total dB*10
	0,30,60,90,120,150,180,210,240,270,300,330,360,390,420,450,480,510,540,570,600,630,660,690};
static const uint8_t fc2580_r73[] = {
	5, 5, 5, 5,  5,  5,  5,  5,  5,  4,  4,  3,  3,  2,  2,  1,  1,  0,  0,  0,  0,  0,  0,  0};
static const uint8_t fc2580_r74[] = {
    0,12,24,36, 48, 60, 72, 84, 96, 84, 96, 84, 96, 84, 96, 84, 96, 84, 96,108,120,132,144,156};

int fc2580_set_gain_index(void *dev, unsigned int i)
{
	int ret = fc2580_write(dev, 0x49, fc2580_r73[i]);
	ret |= fc2580_write(dev, 0x4a, fc2580_r74[i]);

	return ret;
}

const int *fc2580_get_gains(int *len)
{
	*len = sizeof(fc2580_gains);
	return fc2580_gains;
}

int fc2580_exit(void *dev)
{
	rtlsdr_set_gpio_bit(dev, 4, 1);
	return 0;
}
