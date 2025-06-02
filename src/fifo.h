/**\file    fifo.h
 * \ingroup Samplers
 * \brief   Demodulator FIFO support
 *
 * Copyright (c) 2020 FlightAware LLC
 *
 * Rewritten to use Win-8+ SDK function; no pesky Pthreads.
 */
#pragma once

/**
 * \typedef mag_buf_flags
 *  Values for `mag_buf::flags`
 */
typedef enum mag_buf_flags {
        MAGBUF_ZERO = 0,         /**< this is a *normal* buffer */
        MAGBUF_DISCONTINUOUS = 1 /**< this buffer is discontinuous to the previous buffer */
      } mag_buf_flags;

/**
 * \typedef mag_buf
 *
 * Structure representing one magnitude buffer
 * The contained data looks like this:
 *
 * ```
 *  0                 overlap          valid_length - overlap          valid_length       total_length
 *  |                    |                     |                            |                |
 *  | overlap data from  |  new sample data    | new sample data that       |  optional      |
 *  | previous buffer    |                     | will be used as overlap    |  unused        |
 *  |                    |                     | in the next buffer         |  space         |
 *  |                    |                     |                            |                |
 *  |                    |                   [this is the position of the]  |                |
 *  |                    |                   [last message that the      ]  |                |
 *  |                    |                   [demodulator will decode    ]  |                |
 *  |                    |                     |                            |                |
 *  |                    |                     |     [partial messages that start    ]       |
 *  |  [copied here in ] |  <----------------------  [after the cutoff will be copied]       |
 *  |  [the next buffer] |                     |     [to the starting overlap of the ]       |
 *  |                    |                     |     [next buffer                    ]       |
 * ```
 *
 * The demodulator looks for signals starting at offsets `0 .. valid_length - overlap - 1`, <br>
 * with the trailing `overlap` region allowing decoding of a maximally-sized message that starts
 * at `valid_length - overlap - 1`.
 *
 * Signals that start after this point are not decoded, but they will be copied into the
 * starting `overlap` of the next buffer and decoded on the next iteration.
 */
typedef struct mag_buf {
        uint16_t       *data;             /**< Magnitude data, starting with `overlap` from the previous block */
        u_int           total_length;     /**< Maximum number of samples (allocated size of `data`) */
        u_int           valid_length;     /**< Number of valid samples in `data`, including `overlap` samples */
        u_int           overlap;          /**< Number of leading `overlap` samples at the start of `data`;
                                               also the number of trailing samples that will be preserved for next time */
        uint64_t        sample_timestamp; /**< Clock timestamp of the start of this block, 12MHz clock */
        uint64_t        sys_timestamp;    /**< Estimated system time at start of block */
        mag_buf_flags   flags;            /**< Bitwise flags for this buffer */
        double          mean_level;       /**< Mean of normalized (`0 .. 1`) signal level */
        double          mean_power;       /**< Mean of normalized (`0 .. 1`) power level */
        u_int           dropped;          /**< Approx number of dropped samples, if flag `MAGBUF_DISCONTINUOUS` is set; zero if not discontinuous */
        struct mag_buf *next;             /**< Linked list forward link. \todo use a `smartlist_t` instead */
      } mag_buf;

/**
 * \typedef demod_func
 * The demodulator function (that gets passed a buffer from `fifo_dequeue()`),
 * must match this prototype.
 */
typedef void (*demod_func) (const struct mag_buf *mag);

/**
 * Initialize the queue structures.
 * Returns true on success.
 *
 * \param in buffer_count  the number of buffers to preallocate
 * \param in buffer_size   the size of each magnitude buffer, in samples, including overlap
 * \param in overlap       the number of samples to overlap between adjacent buffers
 */
bool fifo_init (unsigned buffer_count, unsigned buffer_size, unsigned overlap);

/**
 * Cleanup the fifo structures allocated in fifo_create().
 * Not threadsafe.
 * Ensure all FIFO users are done before calling.
 */
void fifo_exit (void);

/**
 * Block until the FIFO is empty.
 */
void fifo_drain (void);

/**
 * Mark the FIFO as halted. Move any buffers in FIFO to the \ref fifo_freelist immediately.
 * Future calls to `fifo_acquire()` will immediately return NULL.
 * Future calls to `fifo_enqueue()` will immediately put the produced buffer on the \ref fifo_freelist.
 * Future calls to `fifo_dequeue()` will immediately return NULL;
 *   if there are existing calls waiting on data, they will be immediately awoken and return NULL.
 */
void fifo_halt (void);

/**
 * Get an unused buffer from the \ref fifo_freelist and return it.
 * Block up to `timeout_ms` waiting for a free buffer.
 * Return NULL if there are no free buffers available within the
 * timeout, or if the FIFO is halted.
 */
mag_buf *fifo_acquire (uint32_t timeout_ms);

/**
 * Put a filled buffer (previously obtained from `fifo_acquire()`) onto the head of the FIFO.
 * The caller should have filled:
 * ```
 *   buf->valid_length
 *   buf->data [buf->overlap .. buf->valid_length - 1]
 *   buf->sample_timestamp
 *   buf->sys_timestamp
 *   buf->flags
 *   buf->mean_level (if flags & HAS_METRICS)
 *   buf->mean_power (if flags & HAS_METRICS)
 *   buf->dropped    (if flags & DISCONTINUOUS)
 * ```
 */
void fifo_enqueue (mag_buf *buf);

/**
 * Get a buffer from the tail of the FIFO.
 * If the FIFO is halted (or becomes halted), return NULL immediately.
 * If the FIFO is empty, wait for up to `timeout_ms` milliseconds
 *   for more data; return NULL if no data arrives within the timeout.
 */
mag_buf *fifo_dequeue (uint32_t timeout_ms);

/**
 * Release a buffer previously returned by `fifo_acquire()` back to the \ref fifo_freelist.
 */
void fifo_release (mag_buf *buf);

/**
 * Print some statistics to the log-file.
 */
void fifo_stats (void);
