// hang.c — a page that never returns from lw_frame; the browser must stop it.
#include "lowweb.h"
static unsigned fb[64 * 64];
LW_EXPORT(lw_start) void lw_start(void) { lw_present(fb, 64, 64); }
LW_EXPORT(lw_frame) void lw_frame(double t) { volatile int x = 0; (void)t; for (;;) x++; }
