/**\file    fifo.c
 * \ingroup Samplers
 *
 * Cross-thread SDR to demodulator FIFO support
 *
 * Copyright (c) 2020 FlightAware LLC
 * Copyright (c) 2025 Gisle Vanem
 *
 * Rewritten to use Win-8+ SDK function; no pesky Pthreads.
 */
#include "misc.h"

static CRITICAL_SECTION   fifo_mutex;          /** mutex protecting the queues */
static CONDITION_VARIABLE fifo_notempty_cond;  /** condition used to signal FIFO-not-empty */
static CONDITION_VARIABLE fifo_empty_cond;     /** condition used to signal FIFO-empty */
static CONDITION_VARIABLE fifo_free_cond;      /** condition used to signal freelist-not-empty */

static mag_buf  *fifo_head;                    /** head of queued buffers awaiting demodulation */
static mag_buf  *fifo_tail;                    /** tail of queued buffers awaiting demodulation */
static mag_buf  *fifo_freelist;                /** freelist of preallocated buffers */
static u_int     overlap_length;               /** desired overlap size in samples (size of overlap_buffer) */
static uint16_t *overlap_buffer;               /** buffer used to save overlapping data */
static bool      fifo_halted = false;          /** true if queue has been halted */

/**
 * Create the queue structures. Not threadsafe.
 */
bool fifo_create (unsigned buffer_count, unsigned buffer_size, unsigned overlap)
{
  unsigned i;

  static bool done = false;

  if (!done)
  {
    InitializeCriticalSection (&fifo_mutex);
    InitializeConditionVariable (&fifo_notempty_cond);
    InitializeConditionVariable (&fifo_empty_cond);
    InitializeConditionVariable (&fifo_free_cond);
    done = true;
  }

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
  return (true);

nomem:
  fifo_destroy();
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

void fifo_destroy (void)
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
  {
    TRACE ("%s(): fifo_head: 0x%p, fifo_mutex.OwningThread: %lu",
           __FUNCTION__, fifo_head,
           GetThreadId (((RTL_CRITICAL_SECTION*)&fifo_mutex)->OwningThread));

    SleepConditionVariableCS (&fifo_empty_cond, &fifo_mutex, INFINITE);
  }
  LeaveCriticalSection (&fifo_mutex);
}

void fifo_halt (void)
{
  TRACE ("%s(): fifo_halted: %d, fifo_mutex.OwningThread: %lu",
         __FUNCTION__, fifo_halted, GetThreadId(((RTL_CRITICAL_SECTION*)&fifo_mutex)->OwningThread));

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

  TRACE ("%s(): fifo_halted: %d", __FUNCTION__, fifo_halted);

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
         TRACE ("%s(): err: %s", __FUNCTION__, win_strerror(err));
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
  LeaveCriticalSection (&fifo_mutex);
  return (result);
}

void fifo_enqueue (mag_buf *buf)
{
  assert (buf->valid_length <= buf->total_length);
  assert (buf->valid_length >= overlap_length);

  TRACE ("%s(): fifo_halted: %d, flags: %d", __FUNCTION__, fifo_halted, buf->flags);

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
  if (buf->flags & MAGBUF_DISCONTINUOUS)
  {
    /* This buffer is discontinuous to the previous, so the overlap region is not valid; zero it out
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
  if (!fifo_head)
  {
    fifo_head = fifo_tail = buf;
    WakeConditionVariable (&fifo_notempty_cond);
  }
  else
  {
    fifo_tail->next = buf;
    fifo_tail = buf;
  }

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
         TRACE ("%s(): err: %s", __FUNCTION__, win_strerror(err));
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
