/* amitorture status screen (rt/screen.c) */
#ifndef AMT_SCREEN_H
#define AMT_SCREEN_H

#include "mod.h"

int amt_module_enabled(const struct amt_module *m);   /* rt/mod.c */

void screen_init(void);                 /* OS phase, after the pools: draws and shows it */
void screen_module(const struct amt_module *m, int begin, ULONG pass, ULONG fail, ULONG skip, ULONG unt,
                   int aborted);
void screen_count(const struct amt_module *m, ULONG pass, ULONG fail, ULONG skip, ULONG unt,
                  ULONG tpass, ULONG tfail, ULONG tskip, ULONG tunt);
void screen_bare(int display_on);       /* bare phase: our copper list + BPL/COP DMA, or off */
void screen_finish(ULONG fail, ULONG aborted);   /* OS phase, after the last module */
void screen_close(void);                /* OS phase: previous View back, memory freed */

#endif
