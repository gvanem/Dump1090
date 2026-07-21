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
 * \def COM_RX_SIZE
 * A short-hand macro for the size of allocated rx-buffer
 */
#define COM_RX_SIZE  g_data.sio_buf_size

/**
 * \def IOBUF_SIZE
 * Default size for g_data.sio_buf_size
 */
#define IOBUF_SIZE   2048

/*
 * Largest message assuming all bytes were "stuffed" (0x1A, 0x1A).
 * And add some slack:
 *   2 * sizeof(status_msg) with a 4 byte header:
 *   0x1A, 0x48, 0x01, <len>
 */
#define RX_MAX_SIZE  ((2 * sizeof(status_msg)) + 20)   /* == 60 */

/**
 * \def PKT_MIN_SIZE
 * The minimum size of a packet should be 14 (7 + 7) bytes
 * plus 2 for the 0x1A + msg-type.
 */
#define RX_MIN_SIZE  (sizeof(header_32_33) + MODES_SHORT_MSG_BYTES + 2)

/**
 * \def GNS_HULC_SLEEP
 *
 * Sleep() time in `gns_hulc_read_loop()`.
 */
#define GNS_HULC_SLEEP  50

/**
 * \def WHICH_THREAD()
 * Which thread are we currently executing in?
 * Not used.
 */
#define WHICH_THREAD()  (GetCurrentThreadId() == Modes.reader_thread_id ? \
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
        char        *dev_name;          /**< The device-name; like `\\.\COM1` */
        char         name_space [256];  /**< The Registry mapping of port; e.g. `COM1 -> \Device\VCP0` */
        bool         port_set;          /**< `Modes.gns_hulc.port` was already set from `parse_cmd_line()` */
        uint32_t     baud_rate;         /**< The port baudrate. Fixed at `COM_BAUD_RATE == 921600` */
        uint32_t     dead_count;        /**< Consecutive number of `COM_read() == 0` */
        DCB          old_DCB;           /**< The initial Device Control Block */
        COMMTIMEOUTS old_CTO;           /**< The initial COM timeouts */
      } COM_settings;

#include <packon.h>

/**
 * \typedef status_msg
 *
 * GNS HULC status message (HULC_STATUS == 0x48, ID == 1).
 * It is a packed structure of 20 bytes.
 * 16 and 32-bit values are on Network order; MSB first.
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
 * \typedef enum msg_types
 * The GNS-Hulc message-types we handle.
 */
typedef enum msg_types {
        MODES_SHORT_SQ = 0x32,   /**< Mode-S Short Squitter */
        MODES_EXT_SQ   = 0x33,   /**< Mode-S Extended Squitter */
        HULC_MSG_34    = 0x34,   /**< What is this? */
        HULC_STATUS    = 0x48    /**< HULC Message; status_msg */
      } msg_types;

/**
 * \def MSG_TYPES(pkt)
 * The message-type is stored at `pkt->msg[1]`.
 */
#define MSG_TYPE(pkt)  ((pkt)->msg[1])

/**
 * \typedef GPS_status
 * Settings and information for the GNS-HULC built-in GPS.
 * This gets parsed in `decode_msg_48()`; the HULC Status message.
 */
typedef struct GPS_status {
        bool   have_fix;    /**< Have we got a good GPS fix? Needs >= 20 satelittes */
        bool   detected;    /**< true if `decode_msg_48()` flags reported so */
        bool   enable;      /**< true if config "hulc-gps-enable = yes" */

       /**
        * HDOP; Horizontal Dilution of Precision.
        * \ref https://gnss.ae/understanding-gnss-accuracy-metrics-pdop-hdop-and-vdop/
        * \ref https://en.wikipedia.org/wiki/Dilution_of_precision
        */
        double   HDOP;
        int      satellites;  /**< Number of satellites reported in `decode_msg_48()` */
        int      altitude;    /**< Our altitude reported in `decode_msg_48()` */
        pos_t    pos;         /**< Our position reported in `decode_msg_48()` */
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
        uint64_t  rx_packets_32;       /**< Count of msg-type == MODES_SHORT_SQ packets */
        uint64_t  rx_packets_33;       /**< Count of msg-type == MODES_EXT_SQ packets */
        uint64_t  rx_packets_34;       /**< Count of msg-type == HULC_MSG_34 packets */
        uint64_t  rx_packets_48;       /**< Count of msg-type == HULC_STATUS packets */
        uint64_t  rx_packets_unknown;  /**< Count of unknown msg-type packets */
        uint64_t  tx_packets;          /**< Count of `COM_write()` calls */
        uint64_t  rx_errors;           /**< Count of `COM_read()` errors */
        uint64_t  rx_overruns;         /**< Count of `CE_OVERRUN` errors from `ClearCommError()` */
        uint64_t  tx_errors;           /**< Count of `COM_write()` errors */
        uint64_t  pkt_junk;            /**< Count of junk detected in a packet */
        uint64_t  pkt_enqueued;        /**< Count of packets enqueued in `pkt_enqueue()` */
        uint64_t  pkt_dequeued;        /**< Count of packets dequeued in `gns_hulc_poll()` */
        uint64_t  pkt_OOM;             /**< Count of times where `malloc()` in `pkt_enqueue()` failed */
        uint64_t  pkt_too_large;       /**< Count of too large packets; more than `RX_MAX_SIZE == 50`. Cannot happen */
        uint64_t  pkt_too_short;       /**< Count of too short packets; less than `RX_MIN_SIZE == 14` */
        uint64_t  mode_S_errors;       /**< Count of errors from `decode_mode_S_message()` */
        uint64_t  GPS_fix_lost;        /**< Count of times where we lost GPS-fix */
        uint64_t  GPS_fix_regained;    /**< Count of times where we regained GPS-fix */
      } GNS_statistics;

/**
 * \typedef state_func
 * The state-machine function used in `hulc_read()`.
 */
typedef void (*state_func) (uint8_t ch);

/**
 * \typedef RX_packet
 * What to add to `g_data.pkt_list`.
 */
typedef struct RX_packet {
        uint8_t           msg [RX_MAX_SIZE]; /**< Packet data created in `hulc_read()` */
        int               msg_len;           /**< The length of msg */
        double            usec;              /**< The micro-sec timestamp at `pkt_enqueue()` */
        struct RX_packet *next;              /**< The next packet when added to `g_data.pkt_list` */
      } RX_packet;

/**
 * \typedef struct GNS_priv
 * Data private to GNS-Hulc.
 */
typedef struct GNS_priv {
        uint8_t            *sio_buf;        /**< Buffer for `COM_read()`. \todo Use a ring-buffer instead */
        int                 sio_buf_size;   /**< And it's size */
        int                 sio_len;        /**< Bytes read from last `COM_read()` call */
        int                 old_idx;        /**< Start index for processing old `sio_buf` data */
        bool                old_data;       /**< Are we still processing old data in `sio_buf []`? */
        int                 old_ch;         /**< Previous `ch` in `hulc_read()` */
        bool                got_x1A;        /**< Have we got the first 0x1A sync-byte? */
        state_func          state;          /**< Current FSM-state function */
        CRITICAL_SECTION    crit;           /**< For accessing `g_data.pkt_list` from 2 threads */

        RX_packet           pkt_current;    /**< The packet we're processing now */
        volatile RX_packet *pkt_list;       /**< Linked-list of packets. \todo make it a fixed-size list */
        const char         *pkt_junk;       /**< To track junk-data in the `decode_msg_x()` functions */
        FILE               *gps_file;       /**< File for tracing GPS data */
        volatile bool       fatal_exit;     /**< Exit due to a fatal condition. */

        GPS_status          GPS;
        COM_settings        COM;
        Beast_settings      Beast;
        GNS_statistics      stat;
      } GNS_priv;

extern GNS_priv g_data;

HANDLE COM_init  (uint16_t port);
void   COM_exit  (HANDLE handle);
int    COM_read  (HANDLE hnd, uint8_t *data, size_t len);
int    COM_write (HANDLE hnd, const uint8_t *data, size_t len);
bool   COM_poll_events (void);
void   COM_poll_error (void);

