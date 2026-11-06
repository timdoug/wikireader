//! abitest - the Rust half of the Rust/C calling-convention test.
//!
//! c/abi.c, compiled by the C33 GCC, and this file exchange structs of every
//! shape around GCC's register/stack rule, narrow and 64-bit scalars, floats
//! and variadic arguments, in both directions.  Both sides derive values
//! from a seed the same way and hash what they receive, so an argument in
//! the wrong register or stack slot changes the hash.  The result goes to
//! the serial port and the panel, and the application returns to the
//! launcher.

#![no_std]
#![no_main]

use core::ffi::{c_char, c_int};
use core::fmt::{self, Write};
use core::panic::PanicInfo;

unsafe extern "C" {
    fn debug_print_char(c: c_int) -> c_int;
    fn lcd_print_char(c: c_char) -> c_int;
    fn lcd_clear(colour: c_int);
}

struct Serial;

impl Write for Serial {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { debug_print_char(b as c_int) };
        }
        Ok(())
    }
}

struct Lcd;

impl Write for Lcd {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            unsafe { lcd_print_char(b as c_char) };
        }
        Ok(())
    }
}

fn mix(h: u32, v: u32) -> u32 {
    h.wrapping_mul(131).wrapping_add(v)
}

fn field(seed: u32, k: u32) -> u32 {
    seed.wrapping_mul(2654435761) ^ k.wrapping_mul(40503)
}

/// A struct member: made from the seed's next field(s), and hashed.
trait Field: Copy {
    fn make(seed: u32, k: &mut u32) -> Self;
    fn mix_into(self, h: u32) -> u32;
}

macro_rules! int_field {
    ($($t:ty)*) => {$(
        impl Field for $t {
            fn make(seed: u32, k: &mut u32) -> Self {
                *k += 1;
                field(seed, *k) as $t
            }
            fn mix_into(self, h: u32) -> u32 {
                mix(h, self as u32)
            }
        }
    )*};
}
int_field!(u8 u16 u32);

impl Field for f32 {
    fn make(seed: u32, k: &mut u32) -> Self {
        *k += 1;
        (field(seed, *k) & 0xffff) as f32 + 0.5
    }
    fn mix_into(self, h: u32) -> u32 {
        mix(h, self.to_bits())
    }
}

impl Field for f64 {
    fn make(seed: u32, k: &mut u32) -> Self {
        *k += 1;
        (field(seed, *k) & 0xfffff) as f64 + 0.25
    }
    fn mix_into(self, h: u32) -> u32 {
        let bits = self.to_bits();
        mix(mix(h, bits as u32), (bits >> 32) as u32)
    }
}

impl<const N: usize> Field for [u8; N] {
    fn make(seed: u32, k: &mut u32) -> Self {
        core::array::from_fn(|_| u8::make(seed, k))
    }
    fn mix_into(self, h: u32) -> u32 {
        self.iter().fold(h, |h, &b| mix(h, b as u32))
    }
}

/// Each struct, the C functions taking and returning it, and the Rust
/// functions C calls with it.
macro_rules! abi_structs {
    ($($name:ident { $($f:ident: $t:ty),* }
       $c_take:ident $c_take4:ident $c_ret:ident
       $r_take:ident $r_take4:ident $r_ret:ident;)*) => {
        $(
            #[repr(C)]
            #[derive(Clone, Copy)]
            pub struct $name { $($f: $t),* }

            impl $name {
                fn make(seed: u32) -> Self {
                    let mut k = 0;
                    $name { $($f: <$t as Field>::make(seed, &mut k)),* }
                }
                fn mix(&self) -> u32 {
                    let h = 0;
                    $(let h = self.$f.mix_into(h);)*
                    h
                }
            }

            #[unsafe(no_mangle)]
            pub extern "C" fn $r_take(x: u32, s: $name, y: u32) -> u32 {
                mix(mix(mix(0, x), s.mix()), y)
            }

            #[unsafe(no_mangle)]
            pub extern "C" fn $r_take4(a: u32, b: u32, c: u32, s: $name, y: u32) -> u32 {
                mix(mix(mix(mix(mix(0, a), b), c), s.mix()), y)
            }

            #[unsafe(no_mangle)]
            pub extern "C" fn $r_ret(seed: u32) -> $name {
                $name::make(seed)
            }

            unsafe extern "C" {
                fn $c_take(x: u32, s: $name, y: u32) -> u32;
                fn $c_take4(a: u32, b: u32, c: u32, s: $name, y: u32) -> u32;
                fn $c_ret(seed: u32) -> $name;
            }
        )*

        fn call_c(t: &mut Tally) {
            let (x, y, seed) = (0x1122_3344, 0x5566_7788, 77);
            $(
                let s = $name::make(seed);
                t.check(concat!("take ", stringify!($name)),
                        unsafe { $c_take(x, s, y) }, $r_take(x, s, y));
                t.check(concat!("take4 ", stringify!($name)),
                        unsafe { $c_take4(1, 2, 3, s, y) }, $r_take4(1, 2, 3, s, y));
                t.check(concat!("ret ", stringify!($name)),
                        unsafe { $c_ret(seed + 1) }.mix(), $name::make(seed + 1).mix());
            )*
        }
    };
}

abi_structs! {
    S1 { a: u8 } c_take_S1 c_take4_S1 c_ret_S1 rust_take_S1 rust_take4_S1 rust_ret_S1;
    S2 { a: u16 } c_take_S2 c_take4_S2 c_ret_S2 rust_take_S2 rust_take4_S2 rust_ret_S2;
    S2b { a: u8, b: u8 } c_take_S2b c_take4_S2b c_ret_S2b rust_take_S2b rust_take4_S2b rust_ret_S2b;
    S3 { a: u8, b: u8, c: u8 } c_take_S3 c_take4_S3 c_ret_S3 rust_take_S3 rust_take4_S3 rust_ret_S3;
    S4 { a: u32 } c_take_S4 c_take4_S4 c_ret_S4 rust_take_S4 rust_take4_S4 rust_ret_S4;
    S4b { a: u16, b: u16 } c_take_S4b c_take4_S4b c_ret_S4b rust_take_S4b rust_take4_S4b rust_ret_S4b;
    S5 { a: [u8; 5] } c_take_S5 c_take4_S5 c_ret_S5 rust_take_S5 rust_take4_S5 rust_ret_S5;
    S8 { a: u32, b: u32 } c_take_S8 c_take4_S8 c_ret_S8 rust_take_S8 rust_take4_S8 rust_ret_S8;
    S8b { a: u32, b: u8 } c_take_S8b c_take4_S8b c_ret_S8b rust_take_S8b rust_take4_S8b rust_ret_S8b;
    S8c { a: u16, b: u16, c: u16, d: u16 } c_take_S8c c_take4_S8c c_ret_S8c rust_take_S8c rust_take4_S8c rust_ret_S8c;
    S12 { a: u32, b: u32, c: u32 } c_take_S12 c_take4_S12 c_ret_S12 rust_take_S12 rust_take4_S12 rust_ret_S12;
    S16 { a: u32, b: u32, c: u32, d: u32 } c_take_S16 c_take4_S16 c_ret_S16 rust_take_S16 rust_take4_S16 rust_ret_S16;
    SF { a: f32 } c_take_SF c_take4_SF c_ret_SF rust_take_SF rust_take4_SF rust_ret_SF;
    SFF { a: f32, b: f32 } c_take_SFF c_take4_SFF c_ret_SFF rust_take_SFF rust_take4_SFF rust_ret_SFF;
    SD { a: f64 } c_take_SD c_take4_SD c_ret_SD rust_take_SD rust_take4_SD rust_ret_SD;
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_scalars(a: i8, b: u8, c: i16, d: u16, e: i64, f: f64, g: f32, h: u32) -> u32 {
    let m = mix(mix(mix(mix(0, a as u32), b as u32), c as u32), d as u32);
    let m = mix(mix(m, e as u32), (e >> 32) as u32);
    mix(g.mix_into(f.mix_into(m)), h)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_d4(a: u32, b: u32, c: u32, d: f64, e: u32) -> u32 {
    mix(d.mix_into(mix(mix(mix(0, a), b), c)), e)
}

fn rust_l4(a: u32, b: u32, c: u32, d: i64, e: u32) -> u32 {
    mix(mix(mix(mix(mix(mix(0, a), b), c), d as u32), (d >> 32) as u32), e)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_ret_i8() -> i8 {
    -5
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_ret_i16() -> i16 {
    -300
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_ret_i64() -> i64 {
    -0x1234_5678_9abc
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_ret_f64() -> f64 {
    -1234.5
}

unsafe extern "C" {
    fn c_scalars(a: i8, b: u8, c: i16, d: u16, e: i64, f: f64, g: f32, h: u32) -> u32;
    fn c_d4(a: u32, b: u32, c: u32, d: f64, e: u32) -> u32;
    fn c_l4(a: u32, b: u32, c: u32, d: i64, e: u32) -> u32;
    fn c_ret_i8() -> i8;
    fn c_ret_u8() -> u8;
    fn c_ret_i16() -> i16;
    fn c_ret_i64() -> i64;
    fn c_ret_f64() -> f64;
    fn c_ret_f32() -> f32;
    fn c_va_ints(n: c_int, ...) -> u32;
    fn c_va_mixed(tag: c_int, ...) -> u32;
    fn c_call_rust(count: *mut c_int) -> c_int;
}

struct Tally {
    checks: u32,
    failures: u32,
}

impl Tally {
    fn check(&mut self, what: &str, got: u32, want: u32) {
        self.checks += 1;
        if got != want {
            self.failures += 1;
            let _ = writeln!(Serial, "abi: Rust->C {what}: got {got:08x} want {want:08x}");
        }
    }
}

fn call_c_scalars(t: &mut Tally) {
    unsafe {
        t.check("scalars", c_scalars(-7, 200, -1000, 60000, -0x11_2233_4455, 2.5, -0.75, 9),
                rust_scalars(-7, 200, -1000, 60000, -0x11_2233_4455, 2.5, -0.75, 9));
        t.check("d4", c_d4(1, 2, 3, 6.125, 4), rust_d4(1, 2, 3, 6.125, 4));
        t.check("l4", c_l4(1, 2, 3, -0x55_6677_8899, 4), rust_l4(1, 2, 3, -0x55_6677_8899, 4));
        t.check("ret i8", c_ret_i8() as u32, -5i32 as u32);
        t.check("ret u8", c_ret_u8() as u32, 250);
        t.check("ret i16", c_ret_i16() as u32, -300i32 as u32);
        let l = c_ret_i64();
        t.check("ret i64", l as u32 ^ (l >> 32) as u32,
                -0x1234_5678_9abci64 as u32 ^ (-0x1234_5678_9abci64 >> 32) as u32);
        t.check("ret f64", c_ret_f64().mix_into(0), (-1234.5f64).mix_into(0));
        t.check("ret f32", c_ret_f32().to_bits(), 3.25f32.to_bits());
        t.check("va ints", c_va_ints(4, 10u32, 20u32, 0xdead_beefu32, 40u32),
                [10, 20, 0xdead_beef, 40].iter().fold(0, |h, &v| mix(h, v)));
        let (l, d, i) = (-0x99_8877_6655i64, 0.125f64, -3i32);
        t.check("va mixed", c_va_mixed(5, l, d, i),
                mix(d.mix_into(mix(mix(mix(0, 5), l as u32), (l >> 32) as u32)), i as u32));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn grifo_main(_argc: c_int, _argv: *const *const c_char) -> c_int {
    let mut t = Tally { checks: 0, failures: 0 };
    call_c(&mut t);
    call_c_scalars(&mut t);
    let mut c_checks = 0;
    let c_failures = unsafe { c_call_rust(&mut c_checks) };
    let checks = t.checks + c_checks as u32;
    let failures = t.failures + c_failures as u32;
    let verdict = if failures == 0 { "ABI PASS" } else { "ABI FAIL" };
    unsafe { lcd_clear(0) };
    let _ = writeln!(Lcd, "Rust/C ABI: {checks} checks,\n{failures} failures\n{verdict}");
    let _ = writeln!(Serial, "abi: {checks} checks, {failures} failures: {verdict}");
    0
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let _ = writeln!(Serial, "rust panic: {info}");
    loop {}
}
