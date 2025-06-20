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

