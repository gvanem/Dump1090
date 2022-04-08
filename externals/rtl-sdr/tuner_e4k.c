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

#include <limits.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <tuner_e4k.h>
#include <rtl-sdr.h>
#include <rtlsdr_i2c.h>

/* If this is defined, the limits are somewhat relaxed compared to what the
 * vendor claims is possible */
#define OUT_OF_SPEC

#define MHZ(x)	((x)*1000*1000)
#define KHZ(x)	((x)*1000)

extern int16_t interpolate(int16_t freq, int size, const int16_t *freqs, const int16_t *gains);


uint32_t unsigned_delta(uint32_t a, uint32_t b)
{
	if (a > b)
		return a - b;
	else
		return b - a;
}

/* look-up table bit-width -> mask */
static const uint8_t width2mask[] = {
	0, 1, 3, 7, 0xf, 0x1f, 0x3f, 0x7f, 0xff
};

/***********************************************************************
 * Register Access */

static int e4k_write_array(struct e4k_state *e4k, uint8_t reg, uint8_t *buf, int len)
{
	int rc = rtlsdr_i2c_write_fn(e4k->rtl_dev, e4k->i2c_addr, reg, buf, len);
	if (rc != len) {
		printf( "%s: i2c wr failed=%d reg=%02x len=%d\n",
			   __FUNCTION__, rc, reg, len);
		if (rc < 0)
			return rc;
		return -1;
	}

	return 0;
}

/*! \brief Write a register of the tuner chip
 *  \param[in] e4k reference to the tuner
 *  \param[in] reg number of the register
 *  \param[in] val value to be written
 *  \returns 0 on success, negative in case of error
 */
static int e4k_reg_write(struct e4k_state *e4k, uint8_t reg, uint8_t val)
{
	return e4k_write_array(e4k, reg, &val, 1);
}

static int e4k_read_array(struct e4k_state *e4k, uint8_t reg, uint8_t *buf, int len)
{
	int rc = rtlsdr_i2c_read_fn(e4k->rtl_dev, e4k->i2c_addr, reg, buf, len);
	if (rc != len) {
		printf( "%s: i2c rd failed=%d reg=%02x len=%d\n",
			   __FUNCTION__, rc, reg, len);
		if (rc < 0)
			return rc;
		return -1;
	}

	return 0;
}

/*! \brief Read a register of the tuner chip
 *  \param[in] e4k reference to the tuner
 *  \param[in] reg number of the register
 *  \returns positive 8bit register contents on success, negative in case of error
 */
static int e4k_reg_read(struct e4k_state *e4k, uint8_t reg)
{
	uint8_t data;

	if (rtlsdr_i2c_read_fn(e4k->rtl_dev, e4k->i2c_addr, reg, &data, 1) != 1)
		return -1;

	return data;
}

/*! \brief Set or clear some (masked) bits inside a register
 *  \param[in] e4k reference to the tuner
 *  \param[in] reg number of the register
 *  \param[in] mask bit-mask of the value
 *  \param[in] val data value to be written to register
 *  \returns 0 on success, negative in case of error
 */
static int e4k_reg_set_mask(struct e4k_state *e4k, uint8_t reg,
		     uint8_t mask, uint8_t val)
{
	uint8_t tmp = e4k_reg_read(e4k, reg);

	if ((tmp & mask) == val)
		return 0;

	return e4k_reg_write(e4k, reg, (tmp & ~mask) | (val & mask));
}

/***********************************************************************
 * Filter Control */

static const uint32_t rf_filt_center_uhf[] = {
	MHZ(360), MHZ(380), MHZ(405), MHZ(425),
	MHZ(450), MHZ(475), MHZ(505), MHZ(540),
	MHZ(575), MHZ(615), MHZ(670), MHZ(720),
	MHZ(760), MHZ(840), MHZ(890), MHZ(970)
};

static const uint32_t rf_filt_center_l[] = {
	MHZ(1300), MHZ(1320), MHZ(1360), MHZ(1410),
	MHZ(1445), MHZ(1460), MHZ(1490), MHZ(1530),
	MHZ(1560), MHZ(1590), MHZ(1640), MHZ(1660),
	MHZ(1680), MHZ(1700), MHZ(1720), MHZ(1750)
};

static int closest_arr_idx(const uint32_t *arr, unsigned int arr_size, uint32_t freq)
{
	unsigned int i, bi = 0;
	uint32_t best_delta = 0xffffffff;

	/* iterate over the array containing a list of the center
	 * frequencies, selecting the closest one */
	for (i = 0; i < arr_size; i++) {
		uint32_t delta = unsigned_delta(freq, arr[i]);
		if (delta < best_delta) {
			best_delta = delta;
			bi = i;
		}
	}

	return bi;
}

/* return 4-bit index as to which RF filter to select */
static int choose_rf_filter(enum e4k_band band, uint32_t freq)
{
	int rc;

	switch (band) {
		case E4K_BAND_VHF2:
		case E4K_BAND_VHF3:
			rc = 0;
			break;
		case E4K_BAND_UHF:
			rc = closest_arr_idx(rf_filt_center_uhf,
						 ARRAY_SIZE(rf_filt_center_uhf),
						 freq);
			break;
		case E4K_BAND_L:
			rc = closest_arr_idx(rf_filt_center_l,
						 ARRAY_SIZE(rf_filt_center_l),
						 freq);
			break;
		default:
			rc = -EINVAL;
			break;
	}

	return rc;
}

/* \brief Automatically select apropriate RF filter based on e4k state */
static int e4k_rf_filter_set(struct e4k_state *e4k)
{
	int rc;

	rc = choose_rf_filter(e4k->band, e4k->vco.flo);
	if (rc < 0)
		return rc;

	return e4k_reg_set_mask(e4k, E4K_REG_FILT1, 0xF, rc);
}

int e4k_set_bandwidth(struct e4k_state *e4k, int bw, uint32_t *applied_bw, int apply)
{
	uint8_t data[2];

	if (bw < 2200000)
	{
		*applied_bw = 2000000;
		data[0] = 0xff;
	}
	else if (bw < 3000000)
	{
		*applied_bw = 2400000;
		data[0] = 0xfe;
	}
	else if (bw < 3950000)
	{
		*applied_bw = 3600000;
		data[0] = 0xfd;
	}
	else
	{
		*applied_bw = 4300000;
		data[0] = 0xfc;
	}
	if(!apply)
		return 0;

	/* Mixer Filter 1900 kHz (0.2 dB Bandwidth) */
	/* IF RC Filter = 2000, 2400, 3600 or 5200 kHz */
	/* IF Channel Filter 4300 kHz */
	data[1] = 0x1f;
	return e4k_write_array(e4k, E4K_REG_FILT2, data, 2);
}


/***********************************************************************
 * Frequency Control */

#define E4K_FVCO_MIN_KHZ	2600000	/* 2.6 GHz */
#define E4K_FVCO_MAX_KHZ	3900000	/* 3.9 GHz */
#define E4K_PLL_Y			65536

#ifdef OUT_OF_SPEC
#define E4K_FLO_MIN_MHZ		50
#define E4K_FLO_MAX_MHZ		2200UL
#else
#define E4K_FLO_MIN_MHZ		64
#define E4K_FLO_MAX_MHZ		1700
#endif

struct pll_settings {
	uint32_t freq;
	uint8_t reg_synth7;
	uint8_t mult;
};

static const struct pll_settings pll_vars[] = {
	{KHZ(72400),	(1 << 3) | 7,	48},
	{KHZ(81200),	(1 << 3) | 6,	40},
	{KHZ(108300),	(1 << 3) | 5,	32},
	{KHZ(162500),	(1 << 3) | 4,	24},
	{KHZ(216600),	(1 << 3) | 3,	16},
	{KHZ(325000),	(1 << 3) | 2,	12},
	{KHZ(350000),	(1 << 3) | 1,	8},
	{KHZ(432000),	(0 << 3) | 3,	8},
	{KHZ(667000),	(0 << 3) | 2,	6},
	{KHZ(1200000),	(0 << 3) | 1,	4}
};

static int e4k_band_set(struct e4k_state *e4k, enum e4k_band band)
{
	int rc;

	switch (band) {
	case E4K_BAND_VHF2:
	case E4K_BAND_VHF3:
	case E4K_BAND_UHF:
		e4k_reg_write(e4k, E4K_REG_BIAS, 3);
		break;
	case E4K_BAND_L:
		e4k_reg_write(e4k, E4K_REG_BIAS, 0);
		break;
	}

	/* workaround: if we don't reset this register before writing to it,
	 * we get a gap between 325-350 MHz */
	rc = e4k_reg_set_mask(e4k, E4K_REG_SYNTH1, 0x06, 0);
	rc = e4k_reg_set_mask(e4k, E4K_REG_SYNTH1, 0x06, band << 1);
	if (rc >= 0)
		e4k->band = band;

	return rc;
}

/*! \brief High-level tuning API, just specify frquency
 *
 *  This function will compute matching PLL parameters, program them into the
 *  hardware and set the band as well as RF filter.
 *
 *  \param[in] e4k reference to tuner
 *  \param[in] freq frequency in Hz
 */
int e4k_tune_freq(struct e4k_state *e4k, uint32_t freq)
{
	uint8_t data[3];
	uint32_t i;
	uint8_t r = 2;		//VCO output divider
	uint64_t intended_fvco, remainder;
	int flo;			//computed frequency
	uint64_t fvco;		//computed VCO frequency
	int tuning_error;
	uint16_t x;			//sigma delta
	uint8_t z;			//feedback divider
	uint8_t r_idx = 0;	//Register Synth7
	double fosc = e4k->vco.fosc; // Quartz frequency

	for(i = 0; i < ARRAY_SIZE(pll_vars); ++i)
	{
		if(freq < pll_vars[i].freq)
		{
			r_idx = pll_vars[i].reg_synth7;
			r = pll_vars[i].mult;
			break;
		}
	}
	/* flo(max) = 1700MHz, R(max) = 48, we need 64bit! */
	intended_fvco = (uint64_t)freq * r;
	/* compute integral component of multiplier */
	z = intended_fvco / fosc;

	/* compute fractional part. this will not overflow,
	   as fosc(max) = 30MHz and z(max) = 255 */
	/* remainder(max) = 30MHz, E4K_PLL_Y = 65536 -> 64bit! */
	remainder = intended_fvco - (fosc * z);
	/* x(max) as result of this computation is 65536 */
	x = (remainder * E4K_PLL_Y) / fosc;

	/* We use the following transformation in order to
	 * handle the fractional part with integer arithmetic:
	 *  Fvco = Fosc * (Z + X/Y) <=> Fvco = Fosc * Z + (Fosc * X)/Y
	 * This avoids X/Y = 0.  However, then we would overflow a 32bit
	 * integer, as we cannot hold e.g. 26 MHz * 65536 either.
	 */
	fvco = fosc * z + (fosc * ((double)x + 0.5)) / E4K_PLL_Y;
	if (fvco == 0)
		return -EINVAL;

	flo = fvco / r;
	e4k->vco.flo = flo;
	tuning_error = freq - flo;

	/* program R + 3phase/2phase */
	e4k_reg_write(e4k, E4K_REG_SYNTH7, r_idx);
	data[0] = z; /* program Z */
	data[1] = x & 0xff; /* program X */
	data[2] = x >> 8;
	e4k_write_array(e4k, E4K_REG_SYNTH3, data, 3);

	/* set the band */
	if (flo < MHZ(140))
		e4k_band_set(e4k, E4K_BAND_VHF2);
	else if (flo < MHZ(350))
		e4k_band_set(e4k, E4K_BAND_VHF3);
	else if (flo < MHZ(1135))
		e4k_band_set(e4k, E4K_BAND_UHF);
	else
		e4k_band_set(e4k, E4K_BAND_L);

	/* select and set proper RF filter */
	e4k_rf_filter_set(e4k);

	/* check PLL lock */
	if (!(e4k_reg_read(e4k, E4K_REG_SYNTH1) & 0x01)) {
		printf( "[E4K] PLL not locked for %u Hz!\n", freq);
		return -1;
	}

	//printf( "[E4K] freq=%u, R=%u, flo=%u, tuning_error=%d\n",
	//		freq, r, flo, tuning_error);
	return rtlsdr_set_if_freq(e4k->rtl_dev, tuning_error);
}

/***********************************************************************
 * Gain Control */

static const int8_t if_stage1_gain[] = {
	0, 87
};

static const int8_t if_stage23_gain[] = {
	0, 29, 59, 88
};

static const int8_t if_stage4_gain[] = {
	0, 10, 19, 19
};

static const int8_t if_stage56_gain[] = {
	0, 30, 59, 85, 103, 0, 0, 0
};

static const int8_t *if_stage_gain[] = {
	0,
	if_stage1_gain,
	if_stage23_gain,
	if_stage23_gain,
	if_stage4_gain,
	if_stage56_gain,
	if_stage56_gain
};

static const uint8_t if_stage_gain_len[] = {
	0,
	ARRAY_SIZE(if_stage1_gain),
	ARRAY_SIZE(if_stage23_gain),
	ARRAY_SIZE(if_stage23_gain),
	ARRAY_SIZE(if_stage4_gain),
	ARRAY_SIZE(if_stage56_gain),
	ARRAY_SIZE(if_stage56_gain)
};

static const struct reg_field if_stage_gain_regs[] = {
	{ 0, 0, 0 },
	{ E4K_REG_GAIN3, 0, 1 },
	{ E4K_REG_GAIN3, 1, 2 },
	{ E4K_REG_GAIN3, 3, 2 },
	{ E4K_REG_GAIN3, 5, 2 },
	{ E4K_REG_GAIN4, 0, 3 },
	{ E4K_REG_GAIN4, 3, 3 }
};

int e4k_enable_manual_gain(struct e4k_state *e4k, uint8_t manual)
{
	if (manual) {
		/* Set IF mode to manual */
		e4k_reg_set_mask(e4k, E4K_REG_AGC1, E4K_AGC1_MOD_MASK, E4K_AGC_MOD_IF_SERIAL_LNA_AUTON);

		/* Set Mixer Gain Control to manual */
		e4k_reg_set_mask(e4k, E4K_REG_AGC7, E4K_AGC7_MIX_GAIN_AUTO, 0);
	} else {
		/* Set IF mode to auto */
		e4k_reg_set_mask(e4k, E4K_REG_AGC1, E4K_AGC1_MOD_MASK, E4K_AGC_MOD_IF_DIG_LNA_AUTON);

		/* Set Mixer Gain Control to auto */
		e4k_reg_set_mask(e4k, E4K_REG_AGC7, E4K_AGC7_MIX_GAIN_AUTO, 1);
	}
	return 0;
}

static int find_stage_gain(uint8_t stage, int8_t val)
{
	const int8_t *arr;
	int i;

	if (stage >= ARRAY_SIZE(if_stage_gain))
		return -EINVAL;

	arr = if_stage_gain[stage];

	for (i = 0; i < if_stage_gain_len[stage]; i++) {
		if (arr[i] == val)
			return i;
	}
	return -EINVAL;
}

/*! \brief Set the gain of one of the IF gain stages
 *  \param [e4k] handle to the tuner chip
 *  \param [stage] number of the stage (1..6)
 *  \param [value] gain value in dB
 *  \returns 0 on success, negative in case of error
 */
int e4k_if_gain_set(struct e4k_state *e4k, uint8_t stage, int8_t value)
{
	int rc;
	uint8_t mask;
	const struct reg_field *field;

	rc = find_stage_gain(stage, value);
	if (rc < 0)
		return rc;

	/* compute the bit-mask for the given gain field */
	field = &if_stage_gain_regs[stage];
	mask = width2mask[field->width] << field->shift;

	return e4k_reg_set_mask(e4k, field->reg, mask, rc << field->shift);
}

/***********************************************************************
 * DC Offset */

static int e4k_dc_offset_gen_table(struct e4k_state *e4k)
{
	int ret, i;
	uint8_t buf[3], i_data[4], q_data[4], tmp;

	/* disable auto mixer gain */
	ret = e4k_reg_set_mask(e4k, E4K_REG_AGC7, E4K_AGC7_MIX_GAIN_AUTO, 0);
	if (ret)
		goto err;

	/* gain control manual */
	ret = e4k_reg_write(e4k, E4K_REG_AGC1, 0x00);
	if (ret)
		goto err;

	/* DC offset */
	for (i = 0; i < 4; i++) {
		if (i == 0)
		{
			buf[0] = 0x00; //Mixer gain 4dB
			buf[1] = 0x7e; //IF stage 1 -3dB
			buf[2] = 0x24; //IF stages 2-6 maximal
			ret = e4k_write_array(e4k, E4K_REG_GAIN2, buf, 3);
		}
		else if (i == 1)
			ret = e4k_reg_write(e4k, E4K_REG_GAIN3, 0x7f); //IF stage 1 6dB
		else if (i == 2)
			ret = e4k_reg_write(e4k, E4K_REG_GAIN2, 0x01); //Mixer gain 12dB
		else
			ret = e4k_reg_write(e4k, E4K_REG_GAIN3, 0x7e); //IF stage 1 -3dB
		if (ret)
			goto err;

		ret = e4k_reg_write(e4k, E4K_REG_DC1, 0x01); //DC offset cal request
		if (ret)
			goto err;

		ret = e4k_read_array(e4k, E4K_REG_DC2, buf, 3);
		if (ret)
			goto err;

		i_data[i] = (((buf[2] >> 0) & 0x3) << 6) | (buf[0] & 0x3f);
		q_data[i] = (((buf[2] >> 4) & 0x3) << 6) | (buf[1] & 0x3f);
	}
	//swap(q_data[2], q_data[3]);
	tmp = q_data[2];
	q_data[2] = q_data[3];
	q_data[3] = tmp;
	//swap(i_data[2], i_data[3]);
	tmp = i_data[2];
	i_data[2] = i_data[3];
	i_data[3] = tmp;

	ret = e4k_write_array(e4k, E4K_REG_QLUT0, q_data, 4);
	if (ret)
		goto err;

	ret = e4k_write_array(e4k, E4K_REG_ILUT0, i_data, 4);

err:
	return ret;
}

/***********************************************************************
 * Standby */

/* Enable/disable standby mode
 */
int e4k_standby(struct e4k_state *e4k, int enable)
{
	e4k_reg_set_mask(e4k, E4K_REG_MASTER1, E4K_MASTER1_NORM_STBY,
			 enable ? 0 : E4K_MASTER1_NORM_STBY);
	return 0;
}

/***********************************************************************
 * Initialization */

int e4k_init(struct e4k_state *e4k)
{
	uint8_t data[3];

	/* make a dummy i2c read or write command, will not be ACKed! */
	e4k_reg_read(e4k, 0);

	/* Make sure we reset everything and clear POR indicator */
	e4k_reg_write(e4k, E4K_REG_MASTER1,
		E4K_MASTER1_RESET |
		E4K_MASTER1_NORM_STBY |
		E4K_MASTER1_POR_DET
	);

	/* Configure clock input */
	e4k_reg_write(e4k, E4K_REG_CLK_INP, 0x00);

	/* Disable clock output */
	e4k_reg_write(e4k, E4K_REG_REF_CLK, 0x00);
	e4k_reg_write(e4k, E4K_REG_CLKOUT_PWDN, 0x96);

	/* Write some magic values into registers */
	data[0] = 0x01;
	data[1] = 0xfe;
	e4k_write_array(e4k, 0x7e, data, 2);
	e4k_reg_write(e4k, 0x82, 0x00);
	data[0] = 0x51; /* polarity B */
	data[1] = 0x20;
	data[2] = 0x01;
	e4k_write_array(e4k, 0x86, data, 3);
	data[0] = 0x7f;
	data[1] = 0x07;
	e4k_write_array(e4k, 0x9f, data, 2);

	/* DC offset control */
	e4k_reg_write(e4k, E4K_REG_DC5, 0x1f);

	/* Enable time variant DC correction */
	data[0] = 0x01;
	data[1] = 0x01;
	e4k_write_array(e4k, E4K_REG_DCTIME1, data, 2);

	/* Set common mode voltage a bit higher for more margin 850 mv */
	e4k_reg_set_mask(e4k, E4K_REG_DC7, 7, 4);

	/* Set the most narrow filter we can possibly use */
	data[0] = 0xff;
	data[1] = 0x1f;
	e4k_write_array(e4k, E4K_REG_FILT2, data, 2);

	/* Set LNA */
	data[0] = 16;	/* High threshold */
	data[1] = 8;	/* Low threshold */
	data[2] = 0x18;	/* LNA calib + loop rate */
	e4k_write_array(e4k, E4K_REG_AGC4, data, 3);

	/* Initialize DC offset lookup tables */
	e4k_dc_offset_gen_table(e4k);

	/* Set Mixer Gain Control to auto */
	e4k_reg_write(e4k, E4K_REG_AGC7, 0x15);

	/* Enable LNA Gain enhancement */
	e4k_reg_set_mask(e4k, E4K_REG_AGC11, 0x7,
			 E4K_AGC11_LNA_GAIN_ENH | (2 << 1));

	/* Enable automatic IF gain mode switching */
	e4k_reg_set_mask(e4k, E4K_REG_AGC8, 0x1, E4K_AGC8_SENS_LIN_AUTO);

	/* Use auto-gain as default */
	e4k_enable_manual_gain(e4k, 0);

	return 0;
}

//sensitivity mode
static const uint8_t e4k_reg21[] = {0,  0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1};
static const uint8_t e4k_reg22[] = {0,  2,   0,   2,   4,   1,   3,   5,   7,0x0f,0x17,0x1f,0x1f,0x1f,0x1f,0x3f,0x3f,0x3f,0x3f,0x7f};
static const uint8_t e4k_reg23[] = {0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   2,   3,   4,0x0c,0x14,0x1c,0x24};
/*
//linearity mode
static const uint8_t e4k_reg21[] = {0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1};
static const uint8_t e4k_reg22[] = {0,  0,   0,   0,0x10,0x10,0x10,0x10,0x60,0x68,0x70,0x78,0x7a,0x7c,0x79,0x7b,0x7d,0x7f,0x7d,0x7f};
static const uint8_t e4k_reg23[] = {0,  8,0x10,0x18,0x20,0x21,0x22,0x23,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24};
*/

/* all gain values are expressed in tenths of a dB */
static const int     e4k_gains[] = {0, 29,  60,  89, 119, 147, 176, 206, 235, 264, 294, 323, 353, 382, 408, 436, 466, 495, 521, 548};

#define GAIN_CNT	(sizeof(e4k_gains) / sizeof(int))

int e4k_set_gain(struct e4k_state *e4k, int gain)
{
	uint8_t data[3];
	unsigned int i;

	for (i = 0; i < GAIN_CNT; i++)
		if ((e4k_gains[i] >= gain) || (i+1 == GAIN_CNT))
			break;
	data[0] = e4k_reg21[i];
	data[1] = e4k_reg22[i];
	data[2] = e4k_reg23[i];
	return e4k_write_array(e4k, E4K_REG_GAIN2, data, 3);
}

const int *e4k_get_gains(int *len)
{
	*len = sizeof(e4k_gains);
	return e4k_gains;
}

static const int16_t lna_freqs[] = {
	  50, 75, 100, 200, 500, 750, 1000, 1250, 1500, 1750, 2000};
static const int16_t lna_gains[][16] = {
	{-46, -48, -50, -51, -47, -46, -45, -43, -41, -42, -39},
	{-32, -32, -32, -32, -29, -29, -27, -25, -25, -25, -23},
	{  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},
	{ 23,  25,  25,  26,  24,  26,  25,  27,  28,  25,  28},
	{ 57,  57,  56,  56,  53,  55,  55,  55,  56,  59,  59},
	{ 69,  70,  69,  70,  68,  69,  68,  69,  69,  70,  69},
	{112, 110, 109, 108, 101, 104, 101, 100, 102, 100,  95},
	{124, 122, 120, 120, 115, 118, 115, 115, 119, 114, 111},
	{158, 157, 156, 156, 146, 147, 141, 137, 140, 131, 124},
	{183, 183, 182, 182, 172, 173, 165, 161, 162, 157, 145},
	{208, 207, 205, 202, 194, 198, 188, 187, 191, 183, 173},
	{257, 255, 253, 251, 242, 246, 235, 236, 237, 229, 214}};

static const int16_t mixer_freqs[] = {50,500,1000,1500,2000};
static const int16_t mixer_gains[] = {63, 61,  57,  56,  50};

static const int16_t abs_freqs[] = {
	  50,  75, 100, 125, 150, 175, 200, 225, 250, 275, 300, 325, 350, 350, 360,
 	 380, 405, 425, 450, 475, 505, 540, 575, 615, 670, 720, 760, 840, 890, 970,
	1000,1050,1090,1100,1200,1230,1250,1300,1320,1360,1410,1445,1460,1490,1530,
	1560,1590,1640,1660,1680,1700,1720,1750,1800,1850,1900,1950,2000};
static const int16_t abs_gains[] = {
	 111, 116, 116, 117, 119, 119, 118, 117, 117, 115, 115, 112, 111, 103, 104,
	 106, 108, 110, 111, 113, 114, 116, 117, 120, 122, 126, 129, 138, 149, 165,
	 160, 143, 125, 120,  70,  74,  78,  77,  77,  79,  77,  78,  80,  80,  79,
	  79,  80,  79,  78,  82,  86,  86,  85,  76,  59,  36,  13,  -5};

static int e4k_get_signal_strength(struct e4k_state *e4k, uint8_t *data)
{
	int lna_gain, if_gain, abs_gain;
	int mixer_gain = 0;
	int freq = e4k->vco.flo/1000000;
	int lna_index = data[0x14] & 0xf;

	if(lna_index > 13) lna_index = 13;
	if(lna_index > 1) lna_index -= 2;
	lna_gain = interpolate(freq, ARRAY_SIZE(lna_freqs), lna_freqs, lna_gains[lna_index]);
	abs_gain = interpolate(freq, ARRAY_SIZE(abs_gains), abs_freqs, abs_gains);

	if(data[0x15] & 1)
		mixer_gain += interpolate(freq, ARRAY_SIZE(mixer_freqs), mixer_freqs, mixer_gains);

	if_gain = if_stage1_gain[data[0x16] & 1];
	if_gain += if_stage23_gain[(data[0x16] >> 1) & 3];
	if_gain += if_stage23_gain[(data[0x16] >> 3) & 3];
	if_gain += if_stage4_gain[(data[0x16] >> 5) & 3];
	if_gain += if_stage56_gain[data[0x17] & 7];
	if_gain += if_stage56_gain[(data[0x17] >> 3) & 7];
	//printf("freq=%d,lna=%d, mix=%d, if=%d, abs=%d\n", freq, lna_gain, mixer_gain, if_gain, abs_gain);
	return abs_gain + if_gain + mixer_gain + lna_gain;
}

int e4k_set_i2c_register(struct e4k_state *e4k, unsigned i2c_register, unsigned data, unsigned mask)
{
	return e4k_reg_set_mask(e4k, i2c_register & 0xFF, mask & 0xff, data & 0xff);
}

int e4k_get_i2c_register(struct e4k_state *e4k, uint8_t *data, int *len, int *strength)
{
	int rc;

	*len = 168;
	*strength = 0;
	rc = e4k_read_array(e4k, 0, data, *len);
	if (rc < 0)
		return rc;
	*strength = e4k_get_signal_strength(e4k, data);

	return 0;
}

