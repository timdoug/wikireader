//! hello - a Rust application for the Grifo kernel.
//!
//! Grifo jumps to `grifo_main` (application.lds names it as the entry) with
//! argc/argv, and the return value is the exit type: 0 chains init.app.  The
//! picture stays up until the screen is tapped.

#![no_std]
#![no_main]

use core::ffi::{c_char, c_int, c_void};
use core::fmt::{self, Write};
use core::panic::PanicInfo;

const EXIT_RESTART_INIT: c_int = 0;
const LCD_WHITE: c_int = 0;
const LCD_BLACK: c_int = 1;
const LCD_WIDTH: c_int = 240;
const LCD_HEIGHT: c_int = 208;
const EVENT_TOUCH_DOWN: c_int = 2;
const EVENT_TOUCH_UP: c_int = 4;

/// grifo.h's event_t: the type, a time stamp and a union whose largest
/// member is the touch position.
#[repr(C)]
struct Event {
    item_type: c_int,
    time_stamp: u32,
    data: [c_int; 2],
}

type EventCallback = unsafe extern "C" fn(arg: *mut c_void) -> bool;

unsafe extern "C" {
    fn debug_print_char(c: c_int) -> c_int;
    fn lcd_print_char(c: c_char) -> c_int;
    fn lcd_clear(colour: c_int);
    fn lcd_set_pixel(x: c_int, y: c_int, colour: c_int);
    fn event_flush();
    fn event_wait(event: *mut Event, callback: EventCallback, arg: *mut c_void) -> c_int;
}

/// Called by event_wait after two idle minutes: keep waiting rather than
/// power off.
unsafe extern "C" fn keep_waiting(_arg: *mut c_void) -> bool {
    true
}

/// Wait for a tap: a touch down followed by its release, so the release
/// does not reach the launcher.
fn wait_for_tap() {
    let mut event = Event { item_type: 0, time_stamp: 0, data: [0; 2] };
    let mut down = false;
    unsafe { event_flush() };
    loop {
        let item = unsafe { event_wait(&mut event, keep_waiting, core::ptr::null_mut()) };
        match item {
            EVENT_TOUCH_DOWN => down = true,
            EVENT_TOUCH_UP if down => return,
            _ => {}
        }
    }
}

/// Grifo's serial console.
struct Serial;

impl Write for Serial {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { debug_print_char(b as c_int) };
        }
        Ok(())
    }
}

/// Grifo's text console on the LCD.
struct Lcd;

impl Write for Lcd {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { lcd_print_char(b as c_char) };
        }
        Ok(())
    }
}

/// The 32-bit FNV-1a hash of `bytes`: a fixed answer the test can check.
fn fnv1a(bytes: &[u8]) -> u32 {
    bytes
        .iter()
        .fold(0x811c_9dc5, |h, &b| (h ^ b as u32).wrapping_mul(0x0100_0193))
}

/// Draw the Sierpinski triangle under the text.
fn sierpinski() {
    for y in 64..LCD_HEIGHT {
        for x in 0..LCD_WIDTH {
            let (u, v) = (x - (LCD_WIDTH - 128) / 2, y - 64);
            if (0..128).contains(&u) && (0..128).contains(&v) && u & v == 0 {
                unsafe { lcd_set_pixel(x, y, LCD_BLACK) };
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn grifo_main(argc: c_int, argv: *const *const c_char) -> c_int {
    unsafe { lcd_clear(LCD_WHITE) };
    let primes = (2u32..200)
        .filter(|&n| (2..n).take_while(|d| d * d <= n).all(|d| n % d != 0))
        .count();
    let hash = fnv1a(b"WikiReader");
    let _ = writeln!(Lcd, "Hello from Rust!");
    let _ = writeln!(Lcd, "{primes} primes below 200");
    let _ = writeln!(Lcd, "fnv1a = {hash:#010x}");
    let _ = writeln!(Lcd, "Tap to return.");
    let _ = writeln!(Serial, "rust hello: argc={argc} argv={argv:p}");
    let _ = writeln!(Serial, "rust hello: primes={primes} fnv1a={hash:#010x}");
    sierpinski();
    let _ = writeln!(Serial, "rust hello: waiting for a tap");
    wait_for_tap();
    EXIT_RESTART_INIT
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let _ = writeln!(Serial, "rust panic: {info}");
    loop {}
}
