/* Native Grifo application entry, GPL-3.0-or-later. */
#include <grifo.h>
#include "wr_doom.h"
#include "profile.h"

/* Stable symbol for emulator input scripts and frame profiling. */
void __attribute__((noinline)) doom_frame_ready(void) { asm volatile("nop"); }

int grifo_main(int argc, char **argv)
{
    wr_profile_init(argc, argv);
    wr_platform_init();
    lcd_clear(LCD_WHITE);
    lcd_print("DOOM\n\nLoading /doom ...");
    debug_print("WikiReader Doom starting\n");
    /* Engine initialization may draw a frame and fill its texture cache
       in the LCD window buffer before wr_video_init runs. */
    lcd_window_disable();
    wr_engine_init(argc, argv);
    wr_profile_boot("engine_ready");
    wr_video_init();
    event_flush();
    debug_print("Doom ready: touch to move; Random fire, Search use, History menu\n");
    for (;;) {
        if (wr_profile_locked()) {
            event_t e;
            while (event_get(&e) != EVENT_NONE)
                if (e.item_type == EVENT_BATTERY_LOW) power_off();
        } else wr_controls_poll();
        if (wr_profile_enabled) wr_profile_begin();
        if (wr_engine_step()) wr_controls_frame_done();
        if (wr_profile_enabled) wr_profile_engine_done();
        wr_video_draw(wr_engine_frame(), wr_engine_palette());
        doom_frame_ready();
        if (wr_profile_enabled) {
            wr_profile_state state;
            wr_engine_profile(&state);
            wr_profile_end(&state);
        }
        watchdog(WATCHDOG_KEY);
    }
}
