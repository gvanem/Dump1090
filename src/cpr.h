/**\file    cpr.h
 * \ingroup Misc
 */
#pragma once

struct aircraft;

#define CPR_TRACE(...) do {                            \
          if (Modes.cpr_trace) {                       \
             char buf [1000];                          \
             snprintf (buf, sizeof(buf), __VA_ARGS__); \
             LOG_FILEONLY ("CPR: %s", buf);            \
          }                                            \
        } while (0)

int  cpr_do_global (struct aircraft *a, const struct modeS_message *mm, uint64_t now, pos_t *new_pos, unsigned *nuc);
int  cpr_do_local  (struct aircraft *a, const struct modeS_message *mm, uint64_t now, pos_t *new_pos, unsigned *nuc);
bool cpr_do_tests (void);

