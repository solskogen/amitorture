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
/* benchmark-only disk: free-text line `line` (0..) below the message lines;
 * a, b, c concatenated (b, c may be 0) */
void screen_bench(int line, const char *a, const char *b, const char *c);
void screen_finish(ULONG fail, ULONG aborted);   /* OS phase, after the last module */
void screen_close(void);                /* OS phase: previous View back, memory freed */

#endif
