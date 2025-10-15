/**\file    fifo.c
 * \ingroup Samplers
 * \brief   Demodulator FIFO support
 *
 * Copyright (c) 2020 FlightAware LLC
 * Copyright (c) 2025 Gisle Vanem
 *
 * Rewritten to use Win-8+ SDK function; no pesky PThreads.
 */
#include "misc.h"

static CRITICAL_SECTION   fifo_mutex;          /**< Mutex protecting the queues */
static CONDITION_VARIABLE fifo_notempty_cond;  /**< Condition used to signal FIFO-not-empty */
static CONDITION_VARIABLE fifo_empty_cond;     /**< Condition used to signal FIFO-empty */
static CONDITION_VARIABLE fifo_free_cond;      /**< Condition used to signal freelist-not-empty (by `fifo_acquire()`) */

static mag_buf  *fifo_head;                    /**< Head of queued buffers awaiting demodulation */
static mag_buf  *fifo_tail;                    /**< Tail of queued buffers awaiting demodulation */
static mag_buf  *fifo_freelist;                /**< Freelist of preallocated buffers */
static u_int     overlap_length;               /**< Desired overlap size in samples (size of overlap_buffer) */
static uint16_t *overlap_buffer;               /**< Buffer used to save overlapping data */
static bool      fifo_halted = false;          /**< True if queue has been halted */

/**
 * Create the queue structures.
 */
bool fifo_init (unsigned buffer_count, unsigned buffer_size, unsigned overlap)
{
  unsigned i;

  InitializeCriticalSection (&fifo_mutex);
  EnterCriticalSection (&fifo_mutex);

  InitializeConditionVariable (&fifo_notempty_cond);
  InitializeConditionVariable (&fifo_empty_cond);
  InitializeConditionVariable (&fifo_free_cond);

  overlap_buffer = calloc (overlap, sizeof(overlap_buffer[0]));
  if (!overlap_buffer)
      goto nomem;

  overlap_length = overlap;

  for (i = 0; i < buffer_count; ++i)
  {
    mag_buf *newbuf = calloc (1, sizeof(*newbuf));

    if (!newbuf)
       goto nomem;

    newbuf->data = calloc (buffer_size, sizeof(newbuf->data[0]));
    if (!newbuf->data)
    {
      free (newbuf);
      goto nomem;
    }
    newbuf->total_length = buffer_size;
    newbuf->next  = fifo_freelist;
    fifo_freelist = newbuf;
  }
  LeaveCriticalSection (&fifo_mutex);
  return (true);

nomem:
  LeaveCriticalSection (&fifo_mutex);
  fifo_exit();
  return (false);
}

static void free_buffer_list (mag_buf *head)
{
  while (head)
  {
    mag_buf *next = head->next;

    free (head->data);
    free (head);
    head = next;
  }
}

void fifo_exit (void)
{
  free_buffer_list (fifo_head);
  fifo_head = fifo_tail = NULL;

  free_buffer_list (fifo_freelist);
  fifo_freelist = NULL;

  free (overlap_buffer);
  overlap_buffer = NULL;

  DeleteCriticalSection (&fifo_mutex);
}

void fifo_drain (void)
{
  EnterCriticalSection (&fifo_mutex);

  while (fifo_head && !fifo_halted)
      SleepConditionVariableCS (&fifo_empty_cond, &fifo_mutex, INFINITE);

  LeaveCriticalSection (&fifo_mutex);
}

void fifo_halt (void)
{
  EnterCriticalSection (&fifo_mutex);

  /* Drain all enqueued buffers to the freelist
   */
  while (fifo_head)
  {
    mag_buf *freebuf = fifo_head;

    fifo_head = freebuf->next;
    freebuf->next = fifo_freelist;
    fifo_freelist = freebuf;
  }
  fifo_tail   = NULL;
  fifo_halted = true;

  /* Wake all waiters
   */
  WakeAllConditionVariable (&fifo_notempty_cond);
  WakeAllConditionVariable (&fifo_empty_cond);
  WakeAllConditionVariable (&fifo_free_cond);

  LeaveCriticalSection (&fifo_mutex);
}

mag_buf *fifo_acquire (uint32_t timeout_ms)
{
  mag_buf *result = NULL;

  EnterCriticalSection (&fifo_mutex);

  while (!fifo_halted && !fifo_freelist)
  {
    if (!timeout_ms)
       goto done;  /* Non-blocking */

    /* No free buffers, wait for one
     */
    if (!SleepConditionVariableCS(&fifo_free_cond, &fifo_mutex, timeout_ms))
    {
      DWORD err = GetLastError();

      if (err != ERROR_TIMEOUT)
         LOG_FILEONLY ("%s(): err: %s", __FUNCTION__, win_strerror(err));
      goto done; /* done waiting */
    }
  }

  if (!fifo_halted)
  {
    result = fifo_freelist;
    fifo_freelist = result->next;

    result->overlap          = overlap_length;
    result->valid_length     = result->overlap;
    result->sample_timestamp = 0;
    result->sys_timestamp    = 0;
    result->flags            = MAGBUF_ZERO;
    result->mean_level       = 0;
    result->mean_power       = 0;
    result->dropped          = 0;
    result->next             = NULL;
  }

done:
  if (!result)
     Modes.stat.FIFO_full++;

  LeaveCriticalSection (&fifo_mutex);
  return (result);
}

void fifo_enqueue (mag_buf *buf)
{
  assert (buf->valid_length <= buf->total_length);
  assert (buf->valid_length >= overlap_length);

  EnterCriticalSection (&fifo_mutex);

  if (fifo_halted)
  {
    /* Shutting down, just return the buffer to the freelist.
     */
    buf->next = fifo_freelist;
    fifo_freelist = buf;
    goto done;
  }

  /* Populate the overlap region
   */
  if (buf->flags == MAGBUF_DISCONTINUOUS)
  {
    /* This buffer is discontinuous to the previous, so the
     * overlap region is not valid; zero it out
     */
    memset (buf->data, '\0', overlap_length * sizeof(buf->data[0]));
  }
  else
  {
    memcpy (buf->data, overlap_buffer, overlap_length * sizeof(buf->data[0]));
  }

  /* Save the tail of the buffer for next time
   */
  memcpy (overlap_buffer, &buf->data[buf->valid_length - overlap_length], overlap_length * sizeof(overlap_buffer[0]));

  /* Enqueue and tell the main thread
   */
  buf->next = NULL;
  if (!fifo_head)   /* FIFO empty */
  {
    fifo_head = fifo_tail = buf;
    WakeConditionVariable (&fifo_notempty_cond);
  }
  else
  {
    fifo_tail->next = buf;
    fifo_tail = buf;
  }
  Modes.stat.FIFO_enqueue++;

done:
  LeaveCriticalSection (&fifo_mutex);
}

mag_buf *fifo_dequeue (uint32_t timeout_ms)
{
  mag_buf *result = NULL;

  EnterCriticalSection (&fifo_mutex);

  while (!fifo_head && !fifo_halted)
  {
    if (timeout_ms == 0)
       goto done; /* Non-blocking */

    /* No data pending, wait for some
     */
    if (!SleepConditionVariableCS(&fifo_notempty_cond, &fifo_mutex, timeout_ms))
    {
      DWORD err = GetLastError();

      if (err != ERROR_TIMEOUT)
         LOG_FILEONLY ("%s(): err: %s", __FUNCTION__, win_strerror(err));
      goto done; /* done waiting */
    }
  }

  if (!fifo_halted)
  {
    result       = fifo_head;
    fifo_head    = result->next;
    result->next = NULL;
    if (!fifo_head)
    {
      fifo_tail = NULL;
      WakeAllConditionVariable (&fifo_empty_cond);
    }
  }

done:
  if (result)
     Modes.stat.FIFO_dequeue++;

  LeaveCriticalSection (&fifo_mutex);
  return (result);
}

void fifo_release (mag_buf *buf)
{
  EnterCriticalSection (&fifo_mutex);

  if (!fifo_freelist)
     WakeConditionVariable (&fifo_free_cond);

  buf->next = fifo_freelist;
  fifo_freelist = buf;

  LeaveCriticalSection (&fifo_mutex);
}

void fifo_stats (void)
{
  static uint64_t old_full = 0ULL;
  uint64_t delta_full = Modes.stat.FIFO_full - old_full;

  if (overlap_buffer && Modes.log && delta_full > 0ULL &&
      !(Modes.debug & DEBUG_PLANE))  /* do not disturb plane details */
  {
    LOG_FILEONLY ("FIFO_full: %llu (%llu)\n", Modes.stat.FIFO_full, delta_full);
  }
  old_full = Modes.stat.FIFO_full;
}
