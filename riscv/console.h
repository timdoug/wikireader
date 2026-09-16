/* console.h - the guest's terminal on the WikiReader's panel.
 *
 * Output is a scrolling text region; input comes from either the serial
 * port (which is how the emulator types into it) or an on-screen keyboard
 * on the bottom of the panel, which is the only way in on the device.
 */

#ifndef RV_CONSOLE_H
#define RV_CONSOLE_H

void console_init(void);

/* One byte from the guest to the panel. */
void console_put(int c);

/* Drain pending touch and key events into the input queue.  Called between
   batches of guest instructions. */
void console_poll(void);

/* The next byte for the guest, or -1 when nothing has been typed. */
int console_get(void);

#endif
