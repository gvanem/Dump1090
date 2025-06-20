/** \file smartlist.h
 *  \ingroup Misc
 */
#pragma once

typedef struct smartlist_t smartlist_t;  /* Opaque struct; defined in smartlist.c */

typedef int  (*smartlist_sort_func) (const void **a, const void **b);
typedef void (*smartlist_free_func) (void *a);

smartlist_t *smartlist_new (void);
void       **smartlist_ensure_capacity (smartlist_t *sl, size_t num);
int          smartlist_len (const smartlist_t *sl);
void        *smartlist_get (const smartlist_t *sl, int idx);

void         smartlist_free (smartlist_t *sl);
void        *smartlist_add (smartlist_t *sl, void *element);
void         smartlist_del (smartlist_t *sl, int idx);
void         smartlist_wipe (smartlist_t *sl, smartlist_free_func free_fn);
void         smartlist_sort (smartlist_t *sl, smartlist_sort_func compare, int reverse);

