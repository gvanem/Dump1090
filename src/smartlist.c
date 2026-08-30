/**\file    smartlist.c
 * \ingroup Misc
 * \brief   Functions for dynamic arrays.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include "smartlist.h"

/**
 * \typedef struct smartlist_t
 *
 * From Tor's `src/lib/smartlist_core/smartlist.h`:
 *
 * A resizeable list of pointers, with associated helpful functionality.
 *
 * The members of this struct are exposed only so that macros and inlines can
 * use them; all access to smartlist internals should go through the functions
 * and macros defined here.
 */
typedef struct smartlist_t {
        /**
         * `list` (of anything) has enough capacity to store exactly `capacity`
         * elements before it needs to be resized. Only the first `num_used`
         * (`<= capacity`) elements point to valid data.
         */
        void **list;
        int    num_used;  /**< the number of elements in `list[]` that is currently used */
        int    capacity;  /**< the maximum capacity for the `list[]` */
      } smartlist_t;

/**
 * \def SMARTLIST_DEFAULT_CAPACITY
 *
 * All newly allocated smartlists have this capacity.
 * I.e. room for 16 elements in `smartlist_t::list[]`.
 */
#define SMARTLIST_DEFAULT_CAPACITY  16

/**
 * \def SMARTLIST_MAX_CAPACITY
 *
 * A smartlist can hold `INT_MAX` (2147483647) number of
 * elements in `smartlist_t::list[]`.
 */
#define SMARTLIST_MAX_CAPACITY  INT_MAX

/**
 * Return the number of items in `sl`.
 */
int smartlist_len (const smartlist_t *sl)
{
  assert (sl);
  return (sl->num_used);
}

/**
 * Return the `idx`-th element of `sl`.
 */
void *smartlist_get (const smartlist_t *sl, int idx)
{
  assert (sl);
  assert (idx >= 0);
  assert (sl->num_used > idx);
  return (sl->list[idx]);
}

/**
 * Allocate, initialise and return an empty smartlist.
 */
smartlist_t *smartlist_new (void)
{
  smartlist_t *sl = malloc (sizeof(*sl));

  if (!sl)
     return (NULL);

  sl->num_used = 0;
  sl->capacity = SMARTLIST_DEFAULT_CAPACITY;
  sl->list = calloc (sizeof(void*), sl->capacity);
  if (!sl->list)
     free (sl);
  return (sl);
}

/**
 * Deallocate a smartlist. Does not release storage associated with the
 * list's elements.
 */
void smartlist_free (smartlist_t *sl)
{
  if (sl)
  {
    sl->num_used = 0;
    free (sl->list);
    free (sl);
  }
}

/**
 * Make sure that `sl` can hold at least `num` entries.
 */
void **smartlist_ensure_capacity (smartlist_t *sl, size_t num)
{
  void **new_list = sl->list;

  assert (num <= SMARTLIST_MAX_CAPACITY);

  if (num > (size_t)sl->capacity)
  {
    size_t higher = (size_t) sl->capacity;

    if (num > SMARTLIST_MAX_CAPACITY/2)
       higher = SMARTLIST_MAX_CAPACITY;
    else
    {
      while (num > higher)
        higher *= 2;
    }
    new_list = realloc (sl->list, sizeof(void*) * higher);
    if (!new_list)
       return (NULL);

    sl->list = new_list;
    memset (sl->list + sl->capacity, 0, sizeof(void*) * (higher - sl->capacity));
    sl->capacity = (int) higher;
  }
  return (sl->list);
}

/**
 * Append the pointer `element` to the end of the `sl` list.
 */
void *smartlist_add (smartlist_t *sl, void *element)
{
  if (smartlist_ensure_capacity(sl, 1 + (size_t)sl->num_used))
     sl->list [sl->num_used++] = element;
  return (element);
}

/**
 * Remove the `idx`-th element of `sl`: <br>
 *  - if `idx` is not the last element, move all subsequent elements back one
 *    space.
 */
void smartlist_del (smartlist_t *sl, int idx)
{
  assert (idx >= 0);
  assert (idx < sl->num_used);
  sl->num_used--;
  if (idx < sl->num_used)
  {
    void  *src = sl->list + idx + 1;
    void  *dst = sl->list + idx;
    size_t sz = (sl->num_used - idx) * sizeof(void*);

    memmove (dst, src, sz);
  }
  sl->list [sl->num_used] = NULL;
}

/**
 * Free all elements and free the list.
 */
void smartlist_wipe (smartlist_t *sl, void (*free_fn)(void *a))
{
  int i;

  for (i = 0; i < sl->num_used; i++)
     (*free_fn) (sl->list[i]);
  smartlist_free (sl);
}

/**
 *\typedef int (*UserCmpFunc) (const void *, const void *);
 *
 * The `__cdecl` or `__fastcall` type of user's compare function.<br>
 * Since `qsort()` needs a `__cdecl` compare function, we sort via
 * a function of this type.
 */
typedef int (*UserCmpFunc) (const void *, const void *);

/** The actual pointer to the user's compare function
 */
static UserCmpFunc user_compare;

/**
 * Sort the members of `sl` into an order defined by
 * the ordering function `compare`, which:
 *
 * \li < 0 if `a` precedes `b`.
 * \li > 0 if `b` precedes `a`.
 * \li and 0 if `a` equals `b`.
 *
 * Do it via `__cdecl compare_ascending()` or `__cdecl compare_descending()`.
 */
static int __cdecl compare_ascending (const void *a, const void *b)
{
  return (*user_compare) (a, b);
}

static int __cdecl compare_descending (const void *a, const void *b)
{
  return (*user_compare) (b, a);
}

void smartlist_sort (smartlist_t *sl, smartlist_sort_func compare, int reverse)
{
  if (sl->num_used > 0)
  {
    user_compare = (UserCmpFunc) compare;
    qsort (sl->list, sl->num_used, sizeof(void*), reverse ? compare_descending : compare_ascending);
    user_compare = NULL;
  }
}

/**
 * Assuming the members of `sl` are in order, return the index of the
 * member that matches `key`. If no member matches, return the index of
 * the first member greater than `key`, or `smartlist_len(sl)` if no member
 * is greater than `key`.
 *
 * Set `found_out` to 1 on a match, to 0 otherwise.
 * Ordering and matching are defined by a `compare` function that:
 *  \li returns 0 on a match
 *  \li less than 0 if `key` is less than member,
 *  \li and greater than 0 if `key` is greater then member.
 */
int smartlist_bsearch_idx (const smartlist_t *sl, const void *key,
                           smartlist_bsearch_func compare, int *found_out)
{
  int hi, lo, cmp, mid, len, diff;

  assert (sl);
  assert (compare);
  assert (found_out);

  len = smartlist_len (sl);

  /* Check for the trivial case of a zero-length list
   */
  if (len == 0)
  {
    *found_out = 0;

    /* We already know smartlist_len(sl) is 0 in this case
     */
    return (0);
  }

  /* Okay, we have a real search to do
   */
  assert (len > 0);
  lo = 0;
  hi = len - 1;

  /*
   * These invariants are always true:
   *
   * For all i such that 0 <= i < lo, sl[i] < key
   * For all i such that hi < i <= len, sl[i] > key
   */

  while (lo <= hi)
  {
    diff = hi - lo;

    /*
     * We want mid = (lo + hi) / 2, but that could lead to overflow, so
     * instead diff = hi - lo (non-negative because of loop condition), and
     * then hi = lo + diff, mid = (lo + lo + diff) / 2 = lo + (diff / 2).
     */
    mid = lo + (diff / 2);
    cmp = (*compare) (key, (const void**) &sl->list[mid]);
    if (cmp == 0)
    {
      /* sl[mid] == key; we found it
       */
      *found_out = 1;
      return (mid);
    }
    if (cmp > 0)
    {
      /*
       * key > sl[mid] and an index i such that sl[i] == key must
       * have i > mid if it exists.
       */

      /*
       * Since lo <= mid <= hi, hi can only decrease on each iteration (by
       * being set to mid - 1) and hi is initially len - 1, mid < len should
       * always hold, and this is not symmetric with the left end of list
       * mid > 0 test below.  A key greater than the right end of the list
       * should eventually lead to lo == hi == mid == len - 1, and then
       * we set lo to len below and fall out to the same exit we hit for
       * a key in the middle of the list but not matching.  Thus, we just
       * assert for consistency here rather than handle a mid == len case.
       */
      assert (mid < len);

      /* Move lo to the element immediately after sl[mid]
       */
      lo = mid + 1;
    }
    else
    {
      /* This should always be true in this case
       */
      assert (cmp < 0);

      /*
       * key < sl[mid] and an index i such that sl[i] == key must
       * have i < mid if it exists.
       */

      if (mid > 0)
      {
        /* Normal case, move hi to the element immediately before sl[mid] */
        hi = mid - 1;
      }
      else
      {
        /* These should always be true in this case
         */
        assert (mid == lo);
        assert (mid == 0);

        /*
         * We were at the beginning of the list and concluded that every
         * element e compares e > key.
         */
        *found_out = 0;
        return (0);
      }
    }
  }

  /*
   * lo > hi; we have no element matching key but we have elements falling
   * on both sides of it.  The lo index points to the first element > key.
   */
  assert (lo == hi + 1);  /* All other cases should have been handled */
  assert (lo >= 0);
  assert (lo <= len);
  assert (hi >= 0);
  assert (hi <= len);

  if (lo < len)
  {
    cmp = (*compare) (key, (const void**) &sl->list[lo]);
    assert (cmp < 0);
  }
  else
  {
    cmp = (*compare) (key, (const void**) &sl->list[len-1]);
    assert (cmp > 0);
  }

  *found_out = 0;
  return (lo);
}

/**
 * Assuming the members of `sl` are in order, return a pointer to the
 * member that matches `key`. Ordering and matching are defined by a
 * `compare` function that:
 *  \li returns 0 on a match
 *  \li less than 0 if key is less than member,
 *  \li and greater than 0 if key is greater then member.
 */
void *smartlist_bsearch (const smartlist_t *sl, const void *key, smartlist_bsearch_func compare)
{
  int found, idx = smartlist_bsearch_idx (sl, key, compare, &found);

  return (found ? smartlist_get(sl, idx) : NULL);
}
