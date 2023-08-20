#ifndef __TUNER_FC2580_H
#define __TUNER_FC2580_H

#define FC2580_I2C_ADDR		0xac
#define FC2580_CHECK_ADDR	0x01
#define FC2580_CHECK_VAL	0x56

/* 16.384 MHz (at least on the Logilink VG0002A) */
#define FC2580_XTAL_FREQ	16384000


// The following context is FC2580 tuner API source code Definitions
int fc2580_init(void *dev);
int fc2580_exit(void *dev);
int fc2580_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply);
int fc2580_set_freq(void *dev, unsigned int RfFreqHz);
int fc2580_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask);
int fc2580_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength);
int fc2580_set_gain_mode(void *dev, int manual);
int fc2580_set_gain_index(void *dev, unsigned int index);
const int *fc2580_get_gains(int *len);

#endif
