/**\file    airspy.h
 * \ingroup Samplers
 */

/*
 * Copyright (c) 2012, Jared Boone <jared@sharebrained.com>
 * Copyright (c) 2013, Michael Ossmann <mike@ossmann.com>
 * Copyright (c) 2013-2016, Benjamin Vernoux <bvernoux@airspy.com>
 * Copyright (C) 2013-2016, Youssef Touil <youssef@airspy.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 *
 *     Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *     Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *     Neither the name of AirSpy nor the names of its contributors may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#pragma once

#include <stdint.h>

#define airspy_dev void

typedef void (*airspy_cb) (uint8_t *buf, uint32_t len, void *ctx);

extern int  airspy_init (const char *name, int index, airspy_dev **device);
extern int  airspy_exit (airspy_dev *device);
extern bool airspy_set_dll_name (const char *arg);
extern int  airspy_set_gain (airspy_dev *device, int gain);
extern int  airspy_cancel_async (airspy_dev *device);
extern int  airspy_read_async (airspy_dev *device,
                               airspy_cb   cb,
                               void        *ctx,
                               uint32_t     buf_num,
                               uint32_t     buf_len);

extern const char *airspy_strerror (int rc);

#if defined(INSIDE_AIRSPY_C) /* included from "airspy.c" */

#define AIRSPY_VERSION     "1.0.12"
#define AIRSPY_VER_MAJOR    1
#define AIRSPY_VER_MINOR    0
#define AIRSPY_VER_REVISION 12

/**
 * \def DEF_AIRSPY_FUNC
 * A typedef for *ALL* airspy_ functions below
 */
#define DEF_AIRSPY_FUNC(ret, name, args)  typedef ret (__cdecl *func_##name) args; \
                                          static func_##name p_##name = NULL

/* Formerly in "airspy_commands.h"
 */
typedef enum {
        RECEIVER_MODE_OFF = 0,
        RECEIVER_MODE_RX  = 1
      } receiver_mode_t;

/*
  Note: airspy_samplerate_t is now obsolete and left for backward compatibility.
  The list of supported sample rates should be retrieved at run time by calling airspy_get_samplerates().
  Refer to the Airspy Tools for illustrations.
 */
typedef enum {
        AIRSPY_SAMPLERATE_10MSPS  = 0, /* 12bits 10MHz IQ */
        AIRSPY_SAMPLERATE_2_5MSPS = 1, /* 12bits 2.5MHz IQ */
        AIRSPY_SAMPLERATE_END     = 2  /* End index for sample rate (corresponds to number of samplerate) */
      } airspy_samplerate_t;


#define AIRSPY_CONF_CMD_SHIFT_BIT (3) // Up to 3bits=8 samplerates (airspy_samplerate_t enum shall not exceed 7)

/* Commands (usb vendor request) shared between Firmware and Host.
 */
#define AIRSPY_CMD_MAX (27)

typedef enum {
        AIRSPY_INVALID                    = 0,
        AIRSPY_RECEIVER_MODE              = 1,
        AIRSPY_SI5351C_WRITE              = 2,
        AIRSPY_SI5351C_READ               = 3,
        AIRSPY_R820T_WRITE                = 4,
        AIRSPY_R820T_READ                 = 5,
        AIRSPY_SPIFLASH_ERASE             = 6,
        AIRSPY_SPIFLASH_WRITE             = 7,
        AIRSPY_SPIFLASH_READ              = 8,
        AIRSPY_BOARD_ID_READ              = 9,
        AIRSPY_VERSION_STRING_READ        = 10,
        AIRSPY_BOARD_PARTID_SERIALNO_READ = 11,
        AIRSPY_SET_SAMPLERATE             = 12,
        AIRSPY_SET_FREQ                   = 13,
        AIRSPY_SET_LNA_GAIN               = 14,
        AIRSPY_SET_MIXER_GAIN             = 15,
        AIRSPY_SET_VGA_GAIN               = 16,
        AIRSPY_SET_LNA_AGC                = 17,
        AIRSPY_SET_MIXER_AGC              = 18,
        AIRSPY_MS_VENDOR_CMD              = 19,
        AIRSPY_SET_RF_BIAS_CMD            = 20,
        AIRSPY_GPIO_WRITE                 = 21,
        AIRSPY_GPIO_READ                  = 22,
        AIRSPY_GPIODIR_WRITE              = 23,
        AIRSPY_GPIODIR_READ               = 24,
        AIRSPY_GET_SAMPLERATES            = 25,
        AIRSPY_SET_PACKING                = 26,
        AIRSPY_SPIFLASH_ERASE_SECTOR      = AIRSPY_CMD_MAX
      } airspy_vendor_request;

typedef enum {
        CONFIG_CALIBRATION = 0,
      //CONFIG_META        = 1,
      } airspy_common_config_pages_t;

typedef enum {
        GPIO_PORT0 = 0,
        GPIO_PORT1 = 1,
        GPIO_PORT2 = 2,
        GPIO_PORT3 = 3,
        GPIO_PORT4 = 4,
        GPIO_PORT5 = 5,
        GPIO_PORT6 = 6,
        GPIO_PORT7 = 7
      } airspy_gpio_port_t;

typedef enum {
        GPIO_PIN0 = 0,
        GPIO_PIN1 = 1,
        GPIO_PIN2 = 2,
        GPIO_PIN3 = 3,
        GPIO_PIN4 = 4,
        GPIO_PIN5 = 5,
        GPIO_PIN6 = 6,
        GPIO_PIN7 = 7,
        GPIO_PIN8 = 8,
        GPIO_PIN9 = 9,
        GPIO_PIN10 = 10,
        GPIO_PIN11 = 11,
        GPIO_PIN12 = 12,
        GPIO_PIN13 = 13,
        GPIO_PIN14 = 14,
        GPIO_PIN15 = 15,
        GPIO_PIN16 = 16,
        GPIO_PIN17 = 17,
        GPIO_PIN18 = 18,
        GPIO_PIN19 = 19,
        GPIO_PIN20 = 20,
        GPIO_PIN21 = 21,
        GPIO_PIN22 = 22,
        GPIO_PIN23 = 23,
        GPIO_PIN24 = 24,
        GPIO_PIN25 = 25,
        GPIO_PIN26 = 26,
        GPIO_PIN27 = 27,
        GPIO_PIN28 = 28,
        GPIO_PIN29 = 29,
        GPIO_PIN30 = 30,
        GPIO_PIN31 = 31
      } airspy_gpio_pin_t;

typedef enum airspy_error {
        AIRSPY_SUCCESS = 0,
        AIRSPY_TRUE    = 1,
        AIRSPY_ERROR_INVALID_PARAM = -2,
        AIRSPY_ERROR_NOT_FOUND = -5,
        AIRSPY_ERROR_BUSY = -6,
        AIRSPY_ERROR_NO_MEM = -11,
        AIRSPY_ERROR_UNSUPPORTED = -12,
        AIRSPY_ERROR_LIBUSB = -1000,
        AIRSPY_ERROR_THREAD = -1001,
        AIRSPY_ERROR_STREAMING_THREAD_ERR = -1002,
        AIRSPY_ERROR_STREAMING_STOPPED = -1003,
        AIRSPY_ERROR_OTHER = -9999,
      } airspy_error;

typedef enum airspy_board_id {
        AIRSPY_BOARD_ID_PROTO_AIRSPY  = 0,
        AIRSPY_BOARD_ID_INVALID = 0xFF,
      } airspy_board_id;

typedef enum airspy_sample_type {
        AIRSPY_SAMPLE_FLOAT32_IQ   = 0,  /* 2 * 32bit float per sample */
        AIRSPY_SAMPLE_FLOAT32_REAL = 1,  /* 1 * 32bit float per sample */
        AIRSPY_SAMPLE_INT16_IQ     = 2,  /* 2 * 16bit int per sample */
        AIRSPY_SAMPLE_INT16_REAL   = 3,  /* 1 * 16bit int per sample */
        AIRSPY_SAMPLE_UINT16_REAL  = 4,  /* 1 * 16bit unsigned int per sample */
        AIRSPY_SAMPLE_RAW          = 5,  /* Raw packed samples from the device */
        AIRSPY_SAMPLE_END          = 6   /* Number of supported sample types */
      } airspy_sample_type;

#define MAX_CONFIG_PAGE_SIZE (0x10000)

typedef struct airspy_device airspy_device;

typedef struct airspy_transfer_t {
        airspy_device *device;
        void                 *ctx;
        void                 *samples;
        int                  sample_count;
        uint64_t             dropped_samples;
        airspy_sample_type   sample_type;
      } airspy_transfer_t;

typedef struct {
        uint32_t  part_id[2];
        uint32_t  serial_no[4];
      } airspy_read_partid_serialno_t;

typedef struct {
        uint32_t  major_version;
        uint32_t  minor_version;
        uint32_t  revision;
      } airspy_lib_version_t;

typedef int (*airspy_sample_block_cb_fn) (airspy_transfer_t *transfer);

DEF_AIRSPY_FUNC (void, airspy_lib_version, (airspy_lib_version_t *lib_version));

DEF_AIRSPY_FUNC (int, airspy_init, (void));
DEF_AIRSPY_FUNC (int, airspy_exit, (void));

DEF_AIRSPY_FUNC (int, airspy_list_devices, (uint64_t *serials, int count));

DEF_AIRSPY_FUNC (int, airspy_open_sn, (struct airspy_device **device, uint64_t serial_number));
DEF_AIRSPY_FUNC (int, airspy_open_fd, (struct airspy_device **device, int fd));
DEF_AIRSPY_FUNC (int, airspy_open, (struct airspy_device **device));
DEF_AIRSPY_FUNC (int, airspy_close, (struct airspy_device *device));

/* Use airspy_get_samplerates(device, buffer, 0) to get the number of available sample rates. It will be returned in the first element of buffer
 */
DEF_AIRSPY_FUNC (int, airspy_get_samplerates, (struct airspy_device *device, uint32_t *buffer, const uint32_t len));

/* Parameter samplerate can be either the index of a samplerate or directly its value in Hz within the list returned by airspy_get_samplerates()
 */
DEF_AIRSPY_FUNC (int, airspy_set_samplerate, (struct airspy_device *device, uint32_t samplerate));
DEF_AIRSPY_FUNC (int, airspy_set_conversion_filter_float32, (struct airspy_device *device, const float *kernel, const uint32_t len));
DEF_AIRSPY_FUNC (int, airspy_set_conversion_filter_int16, (struct airspy_device *device, const int16_t *kernel, const uint32_t len));

DEF_AIRSPY_FUNC (int, airspy_start_rx, (struct airspy_device *device, airspy_sample_block_cb_fn callback, void *rx_ctx));
DEF_AIRSPY_FUNC (int, airspy_stop_rx, (struct airspy_device *device));

/* return AIRSPY_TRUE if success
 */
DEF_AIRSPY_FUNC (int, airspy_is_streaming, (struct airspy_device *device));

DEF_AIRSPY_FUNC (int, airspy_si5351c_write, (struct airspy_device *device, uint8_t register_number, uint8_t value));
DEF_AIRSPY_FUNC (int, airspy_si5351c_read, (struct airspy_device *device, uint8_t register_number, uint8_t *value));

DEF_AIRSPY_FUNC (int, airspy_config_write, (struct airspy_device *device, const uint8_t page_index, const uint16_t length, uint8_t *data));
DEF_AIRSPY_FUNC (int, airspy_config_read, (struct airspy_device *device, const uint8_t page_index, const uint16_t length, uint8_t *data));

DEF_AIRSPY_FUNC (int, airspy_r820t_write, (struct airspy_device *device, uint8_t register_number, uint8_t value));
DEF_AIRSPY_FUNC (int, airspy_r820t_read, (struct airspy_device *device, uint8_t register_number, uint8_t *value));

/* Parameter value shall be 0=clear GPIO or 1=set GPIO
 */
DEF_AIRSPY_FUNC (int, airspy_gpio_write, (struct airspy_device *device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t value));

/* Parameter value corresponds to GPIO state 0 or 1
 */
DEF_AIRSPY_FUNC (int, airspy_gpio_read, (struct airspy_device *device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t *value));

/* Parameter value shall be 0=GPIO Input direction or 1=GPIO Output direction
 */
DEF_AIRSPY_FUNC (int, airspy_gpiodir_write, (struct airspy_device *device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t value));
DEF_AIRSPY_FUNC (int, airspy_gpiodir_read, (struct airspy_device *device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t *value));

DEF_AIRSPY_FUNC (int, airspy_spiflash_erase, (struct airspy_device *device));
DEF_AIRSPY_FUNC (int, airspy_spiflash_write, (struct airspy_device *device, const uint32_t address, const uint16_t length,
                                              uint8_t *const data));

DEF_AIRSPY_FUNC (int, airspy_spiflash_read, (struct airspy_device *device, const uint32_t address, const uint16_t length, uint8_t *data));

DEF_AIRSPY_FUNC (int, airspy_board_id_read, (struct airspy_device *device, uint8_t *value));

/* Parameter length shall be at least 128bytes to avoid possible string clipping
 */
DEF_AIRSPY_FUNC (int, airspy_version_string_read, (struct airspy_device *device, char *version, uint8_t length));

DEF_AIRSPY_FUNC (int, airspy_board_partid_serialno_read, (struct airspy_device *device, airspy_read_partid_serialno_t *read_partid_serialno));

DEF_AIRSPY_FUNC (int, airspy_set_sample_type, (struct airspy_device *device, enum airspy_sample_type sample_type));

/* Parameter freq_hz shall be between 24000000(24MHz) and 1750000000(1.75GHz)
 */
DEF_AIRSPY_FUNC (int, airspy_set_freq, (struct airspy_device *device, const uint32_t freq_hz));

/* Parameter value shall be between 0 and 15
 */
DEF_AIRSPY_FUNC (int, airspy_set_lna_gain, (struct airspy_device *device, uint8_t value));

/* Parameter value shall be between 0 and 15
 */
DEF_AIRSPY_FUNC (int, airspy_set_mixer_gain, (struct airspy_device *device, uint8_t value));

/* Parameter value shall be between 0 and 15
 */
DEF_AIRSPY_FUNC (int, airspy_set_vga_gain, (struct airspy_device *device, uint8_t value));

/* Parameter value:
 * 0=Disable LNA Automatic Gain Control
 * 1=Enable LNA Automatic Gain Control
 */
DEF_AIRSPY_FUNC (int, airspy_set_lna_agc, (struct airspy_device *device, uint8_t value));

/* Parameter value:
 * 0=Disable MIXER Automatic Gain Control
 * 1=Enable MIXER Automatic Gain Control
 */
DEF_AIRSPY_FUNC (int, airspy_set_mixer_agc, (struct airspy_device *device, uint8_t value));

/* Parameter value: 0..21
 */
DEF_AIRSPY_FUNC (int, airspy_set_linearity_gain, (struct airspy_device *device, uint8_t value));

/* Parameter value: 0..21
 */
DEF_AIRSPY_FUNC (int, airspy_set_sensitivity_gain, (struct airspy_device *device, uint8_t value));

/* Parameter value shall be:
 *  0=Disable BiasT or
 *  1=Enable BiasT
 */
DEF_AIRSPY_FUNC (int, airspy_set_rf_bias, (struct airspy_device *dev, uint8_t value));

/* Parameter value shall be:
 * 0=Disable Packing or
 * 1=Enable Packing
*/
DEF_AIRSPY_FUNC (int, airspy_set_packing, (struct airspy_device *device, uint8_t value));

DEF_AIRSPY_FUNC (const char *, airspy_error_name, (enum airspy_error errcode));
DEF_AIRSPY_FUNC (const char *, airspy_board_id_name, (enum airspy_board_id board_id));

/* Parameter sector_num shall be between 2 & 13 (sector 0 & 1 are reserved)
 */
DEF_AIRSPY_FUNC (int, airspy_spiflash_erase_sector, (struct airspy_device *device, const uint16_t sector_num));

#endif  /* INSIDE_AIRSPY_C */
