/**\file
 * \ingroup GNS-HULC
 *
 * Private definitions for gns-hulc.c and gns-serial.c
 */
#pragma once

#include "misc.h"

/**
 * \def COM_BAUD_RATE
 * The bitrate the GNS-Hulc antenna uses.
 */
#define COM_BAUD_RATE   921600

/**
 * \def COM_DEAD_COUNT
 * The count of zero-reads to detect a dead and stuck COM-port.
 */
#define COM_DEAD_COUNT  20

/**
 * \def COM_KEY_NAME
 * The HKLM Registry bracnh for Serial-COM to Device mapping
 */
#define COM_KEY_NAME   "Hardware\\Devicemap\\SerialCOMM"

/**
 * \def COM_RX_SIZE
 * A short-hand macro for the size of allocated rx-buffer
 */
#define COM_RX_SIZE  g_data.sio_buf_size

/*
 * Largest message assuming all bytes were "stuffed" (0x1A, 0x1A).
 * And add some slack:
 *   2 * sizeof(status_msg) with a 4 byte header:
 *   0x1A, 0x48, 0x01, <len>
 */
#define RX_MAX_SIZE ((2 * sizeof(status_msg)) + 20)

/**
 * \def WHICH_THREAD()
 * Which thread are we currently executing in?
 */
#define WHICH_THREAD() (GetCurrentThreadId() == Modes.reader_thread_id ? \
                        "data_thread_fn" : "main")

/**
 * \def DEBUG1()
 * \def DEBUG2()
 *
 * Add these more compact macros to avoid the long `__FILE__` name
 * used in misc.h's `DEBUG()`.
 */
#define DEBUG1(fmt, ...)                            \
        do {                                        \
          if (Modes.debug & DEBUG_GNS_HULC)         \
             modeS_flogf (stdout, GNS_FILE "(%u): " \
               fmt, __LINE__, ## __VA_ARGS__);      \
        } while (0)

#define DEBUG2(fmt, ...)                            \
        do {                                        \
          if (Modes.debug & DEBUG_GNS_HULC2)        \
             modeS_flogf (stdout, GNS_FILE "(%u): " \
               fmt, __LINE__, ## __VA_ARGS__);      \
        } while (0)

/**
 * \typedef COM_settings
 * Settings for the COM-port
 */
typedef struct COM_settings {
        char       dev_name [256];    /**< The device-name; like `\\.\COM1` */
        char       name_space [256];  /**< The Registry mapping of port; e.g. `COM1 -> \Device\VCP0` */
        bool       port_set;          /**< `Modes.gns_hulc.port` was already set from `parse_cmd_line()` */
        uint32_t   baud_rate;         /**< The port baudrate. Fixed at `COM_BAUD_RATE == 921600` */
        uint16_t   RTS_ctrl;          /**< Default "Request-to-Send" control. Always enabled */
        uint16_t   DTR_ctrl;          /**< Default "Data-Terminal-Ready" control. Always enabled */
        uint32_t   dead_count;        /**< Consecutive number of `COM_read() == 0` */
      } COM_settings;

#include <packon.h>

/**
 * \typedef status_msg
 *
 * GNS HULC status message (HULC_STATUS == 0x48).
 * It must be packed. 16 and 32-bit values are on
 * Network order; MSB first.
 */
typedef struct status_msg {
        uint32_t  serial;
        uint16_t  flags;
        uint16_t  reserved;
        uint32_t  xTime;
        int32_t   latitude;
        int32_t   longitude;
        int16_t   altitude;
        uint8_t   satellites;
        uint8_t   hdop;
      } status_msg;

/**
 * typedef header_32_33
 *
 * 7 byte packed header structure common to `decode_msg_32()` and `decode_msg_33()`.
 *
 * \ref https://github.com/firestuff/adsb-tools/blob/master/protocols/beast.md
 *
 * It has no relation to the ASCII protocol "SBS-BaseStation" described at
 * http://woodair.net/sbs/article/barebones42_socket_data.htm
 */
typedef struct header_32_33 {
        uint32_t  ts1;
        uint16_t  ts2;   /**< ts1 + ts2 === 6 bytes big-endian */
        uint8_t   RSSI;  /**< "Received signal strength indicator", uncalibrated */
      } header_32_33;

#include <packoff.h>

/**
 * \def MODES_SHORT_SQ_SZ
 * For a  MODES_SHORT_SQ packet, the `header_32_33` is followed
 * by 7 byte of raw data.
 */
#define MODES_SHORT_SQ_SZ 7

/**
 * \def MODES_EXT_SQ_SZ
 * For a  MODES_EXT_SQ packet, the `header_32_33` is followed
 * by 14 byte of raw data.
 */
#define MODES_EXT_EX_SZ 14

/**
 * \typedef enum msg_types
 */
typedef enum msg_types {
        MODES_SHORT_SQ = 0x32,
        MODES_EXT_SQ   = 0x33,
        HULC_MSG_34    = 0x34,   /* What is this? */
        HULC_STATUS    = 0x48
      } msg_types;

/**
 * \typedef RX_packet
 * What to add to `g_data.pkt_list`.
 */
typedef struct RX_packet {
        uint8_t           msg [RX_MAX_SIZE];
        uint32_t          msg_marker;        /**< End-marker to detect overflow */
        uint8_t           msg_type;          /**< enum msg_types */
        uint32_t          msg_len;           /**< The length of msg */
        uint32_t          unstuffed;         /**< Number of unstuffed x1A bytes in this packet */
        double            usec;              /**< The micro-sec timestamp at enqueue */
        struct RX_packet *next;
      } RX_packet;

#define MARKER_MAGIC 0xDEAFBABE

/**
 * \typedef GPS_status
 * Settings and information of the GNS-HULC built-in GPS.
 * This gets parsed in `decode_msg_48()`; the HULC Status message.
 */
typedef struct GPS_status {
        bool   have_fix;
        bool   detected;
        bool   enable;

       /**
        * HDOP; Horizontal Dilution of Precision.
        * \ref https://gnss.ae/understanding-gnss-accuracy-metrics-pdop-hdop-and-vdop/
        * \ref https://en.wikipedia.org/wiki/Dilution_of_precision
        */
        double   HDOP;
        int      satellites;
        int      altitude;
        pos_t    pos;
      } GPS_status;

/**
 * \typedef Beast_settings
 */
typedef struct Beast_settings {
        bool   enable;
        bool   filter_DF045;
        bool   filter_DF1117;
        bool   mode_AC;
        bool   mlat_timestamp;
        bool   FEC;
        bool   CRC;
      } Beast_settings;

/**
 * \typedef GNS_statistics
 * Statistics for GNS-Hulc.
 */
typedef struct GNS_stats {
        uint64_t  rx_bytes;
        uint64_t  rx_packets_32;
        uint64_t  rx_packets_33;
        uint64_t  rx_packets_34;
        uint64_t  rx_packets_48;
        uint64_t  rx_packets_unknown;
        uint64_t  tx_bytes;
        uint64_t  tx_packets;
        uint64_t  rx_errors;
        uint64_t  tx_errors;
        uint64_t  pkt_junk;
        uint64_t  pkt_enqueued;
        uint64_t  pkt_enqueued_bytes;
        uint64_t  pkt_dequeued;
        uint64_t  pkt_OOM;
        uint64_t  pkt_too_big;
        uint64_t  pkt_too_short;
        uint64_t  pkt_list_sleep;      /**< Count of `Sleep()` calls in `gns_hulc_poll()` */
        uint64_t  pkt_unstuffed;       /**< Count of unstuffed packets */
        uint64_t  old_data_cnt;        /**< Count of processing old-data in `hulc_read()` */
        uint64_t  pkt_bad_marker;      /**< Count of destroyed `g_data.pkt_current.msg_marker` */
        uint32_t  pkt_max_len;         /**< Maximum number of enqueued packets to `g_data.pkt_list` */
        uint32_t  mode_S_errors;       /**< Count of errors from `decode_mode_S_message()` */
      } GNS_statistics;

/**
 * \typedef state_func
 */
typedef void (*state_func) (uint8_t ch);


/**
 * \typedef struct GNS_priv
 * Data private to GNS-Hulc.
 */
typedef struct GNS_priv {
        uint8_t         *sio_buf;        /**< Buffer for `COM_read()`. \todo Use a ring-buffer instead */
        int              sio_buf_size;   /**< And it's size */
        int              sio_len;        /**< Bytes read from last `COM_read()` call */
        int              old_idx;        /**< Start index for processing old `sio_buf` data */
        bool             old_data;       /**< Are we still processing old data in `sio_buf []`? */
        int              old_ch;         /**< Previous `ch` in `hulc_read()` */
        bool             got_x1A;        /**< Have we got the first 0x1A sync-byte? */
        state_func       state;          /**< Current FSM-state function */

        RX_packet        pkt_current;    /**< Current packet we're processing */
        RX_packet       *pkt_list;       /**< Linked-list of `enum msg_types` packets. \todo make it a fixed-size list */
        const char      *pkt_junk;      /**< To track junk-data in the `decode_msg_x()` functions */
        CRITICAL_SECTION crit;           /**< For accessing `pkt_list` from 2 threads */

//      parser_cfg       parser;
        GPS_status       GPS;
        COM_settings     COM;
        Beast_settings   Beast;
        GNS_statistics   stat;
      } GNS_priv;

extern GNS_priv g_data;

bool COM_setup (HANDLE handle);
int  COM_read  (HANDLE hnd, uint8_t *data, size_t len);
int  COM_write (HANDLE hnd, const uint8_t *data, size_t len);

