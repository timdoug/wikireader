/* Touch movement plus the three front buttons, GPL-3.0-or-later. */
#include <grifo.h>
#include "wr_doom.h"

static unsigned char keys[256], wanted[256], pressed[256];
static int touch, tx, ty, buttons[3], running;

static void reconcile(void)
{
    for (int k = 0; k < 256; ++k) wanted[k] = 0;
    int menu = wr_engine_menu();
    if (buttons[BUTTON_RANDOM]) wanted[menu ? WR_ENTER : WR_FIRE] = 1;
    if (buttons[BUTTON_SEARCH]) wanted[menu ? WR_DOWN : WR_SPACE] = 1;
    if (buttons[BUTTON_HISTORY]) wanted[WR_ESCAPE] = 1;
    if (touch && ty < WR_HEIGHT) {
        if (tx < 80) wanted[WR_LEFT] = 1;
        if (tx >= 160) wanted[WR_RIGHT] = 1;
        if (ty < 60) wanted[WR_UP] = 1;
        if (ty >= 120) wanted[WR_DOWN] = 1;
        if (menu && tx >= 80 && tx < 160 && ty >= 60 && ty < 120)
            wanted[WR_ENTER] = 1;
    } else if (touch) {
        switch (tx / 40) {
        case 0: wanted[WR_ESCAPE] = 1; break;
        case 1: wanted[WR_TAB] = 1; break;
        case 2: wanted[','] = 1; break;
        case 3: wanted['.'] = 1; break;
        }
    }
    if (running && !menu) wanted[WR_SHIFT] = 1;
    for (int k = 0; k < 256; ++k) {
        /* A complete tap can arrive while rendering a slow frame. Keep
           its down state through one simulation step before releasing it. */
        if (!wanted[k] && pressed[k]) continue;
        if (keys[k] != wanted[k]) {
            wr_engine_key(k, wanted[k]);
            if (wanted[k]) pressed[k] = 1;
        }
        keys[k] = wanted[k];
    }
}

void wr_controls_event(int type, int code, int x, int y)
{
    if (type == EVENT_TOUCH_DOWN || type == EVENT_TOUCH_MOTION) {
        if (x < 0 || x >= 240 || y < 0 || y >= 208) { touch = 0; }
        else {
            touch = 1; tx = x; ty = y;
            if (type == EVENT_TOUCH_DOWN && y >= WR_HEIGHT) {
                if (x >= 200) running = !running;
                else if (x >= 160) wr_engine_weapon();
            }
        }
    } else if (type == EVENT_TOUCH_UP) touch = 0;
    else if ((type == EVENT_BUTTON_DOWN || type == EVENT_BUTTON_UP) && code >= 0 && code < 3)
        buttons[code] = type == EVENT_BUTTON_DOWN;
    else if (type == EVENT_BATTERY_LOW) power_off();
    reconcile();
}

void wr_controls_frame_done(void)
{
    for (int k = 0; k < 256; ++k) pressed[k] = 0;
}

void wr_controls_poll(void)
{
    event_t e;
    reconcile();
    while (event_get(&e) != EVENT_NONE) {
        wr_controls_event(e.item_type,
            (e.item_type == EVENT_BUTTON_DOWN || e.item_type == EVENT_BUTTON_UP) ? e.button.code : 0,
            (e.item_type == EVENT_TOUCH_DOWN || e.item_type == EVENT_TOUCH_MOTION) ? e.touch.x : 0,
            (e.item_type == EVENT_TOUCH_DOWN || e.item_type == EVENT_TOUCH_MOTION) ? e.touch.y : 0);
    }
    reconcile();
}
