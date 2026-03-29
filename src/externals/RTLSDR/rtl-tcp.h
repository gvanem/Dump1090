/**\file    rtl-tcp.h
 * \ingroup Samplers
 *
 * Stuff for handling a remote RTLSDR device.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \typedef RTL_TCP_cmds
 *
 * The possible RTLTCP commands.
 * Commands above 0x40 are 'rtl2_tcp' extensions
 */
typedef enum RTL_CMD_cmds {
        RTL_SET_FREQUENCY           = 0x01,
        RTL_SET_SAMPLE_RATE         = 0x02,
        RTL_SET_GAIN_MODE           = 0x03,
        RTL_SET_GAIN                = 0x04,
        RTL_SET_FREQ_CORRECTION     = 0x05,
        RTL_SET_IF_STAGE            = 0x06,
        RTL_SET_TEST_MODE           = 0x07,
        RTL_SET_AGC_MODE            = 0x08,
        RTL_SET_DIRECT_SAMPLING     = 0x09,
        RTL_SET_OFFSET_TUNING       = 0x0A,
        RTL_SET_RTL_CRYSTAL         = 0x0B,
        RTL_SET_TUNER_CRYSTAL       = 0x0C,
        RTL_SET_TUNER_GAIN_BY_INDEX = 0x0D,
        RTL_SET_BIAS_TEE            = 0x0E,
        RTL_SET_TUNER_BANDWIDTH     = 0x40,
        RTL_SET_I2C_TUNER_REGISTER  = 0x43,
        RTL_SET_SIDEBAND            = 0x46,
        RTL_REPORT_I2C_REGS         = 0x48,
        RTL_SET_DITHERING           = 0x49,
        RTL_SET_REQUEST_ALL_SERIALS = 0x80,
        RTL_SET_SELECT_SERIAL       = 0x81,
        RTL_SET_FREQ_CORRECTION_PPB = 0x83
      } RTL_CMD_cmds;

#include <packon.h>

/**
 * \typedef RTL_TCP_cmd
 *
 * The RTL_TCP command is a packed structure of 5 bytes.
 */
typedef struct RTL_TCP_cmd {
        uint8_t  cmd;     /**< The command byte == `RTL_x` */
        uint32_t param;   /**< 32-bit parameter on network order */
      } RTL_TCP_cmd;


/**
 * \def RTL_TCP_MAGIC
 *
 * The RTL_TCP_info marker.
 */
#define RTL_TCP_MAGIC "RTL0"

/**
 * \typedef RTL_TCP_info
 *
 * A info-structure for the RTLSDR dongle received on a `mg_connect()`.
 */
typedef struct RTL_TCP_info {
        char     magic [4];          /**< marker == RTL_TCP_MAGIC */
        uint32_t tuner_type;         /**< the `RTLSDR_TUNER_x` type (network order) */
        uint32_t tuner_gain_count;   /**< the number of gains supported (network order) */
      } RTL_TCP_info;

#include <packoff.h>

#ifdef __cplusplus
}
#endif


