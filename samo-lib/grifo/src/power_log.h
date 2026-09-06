#ifndef POWER_LOG_H
#define POWER_LOG_H

#include "standard.h"

/* Optional hardware diagnostics: powerlog.on enables RAM counters at boot;
 * power.txt is written once at orderly shutdown, never on an idle wake.
 * KEEP builds can also enable pwrtrace.txt checkpoints with pwrtrace.on. */
void PowerLog_initialise(void);
bool PowerLog_enabled(void);
void PowerLog_suspend(void);
void PowerLog_resume(bool timeout);
void PowerLog_card_init(unsigned long ticks, bool ready);
void PowerLog_idle(unsigned long ticks, bool timeout);
void PowerLog_report(void);

#endif
