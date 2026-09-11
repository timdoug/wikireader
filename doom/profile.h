/* Coarse target-side timing; all per-frame samples stay in SDRAM. */
#ifndef WR_PROFILE_H
#define WR_PROFILE_H
#include <stdint.h>

enum { WR_PHASE_BSP, WR_PHASE_PLANES, WR_PHASE_MASKED, WR_PHASE_COUNT };
typedef struct {
    unsigned state, menu, demo, wipe, episode, map, skill, detail, width, height;
    unsigned gametic, leveltime, angle;
    int x, y, health;
} wr_profile_state;

extern int wr_profile_enabled;
void wr_profile_init(int argc, char **argv);
int wr_profile_locked(void);
void wr_profile_boot(const char *name);
void wr_profile_begin(void);
void wr_profile_engine_done(void);
void wr_profile_end(const wr_profile_state *state);
void wr_profile_phase_begin(unsigned phase);
void wr_profile_phase_end(unsigned phase);
void wr_profile_finish(int code);
void wr_engine_profile(wr_profile_state *state);
void wr_video_status(const char *text);
#endif
