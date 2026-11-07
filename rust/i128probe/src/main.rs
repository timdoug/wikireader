//! i128probe - the 128-bit operations core's num::i128 tests use, each on a
//! thread with a watchdog, so one that does not return is named.

#![feature(signed_bigint_helpers)]

use std::hint::black_box as bb;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

fn check(name: &'static str, f: fn() -> String) {
    let (tx, rx) = mpsc::channel();
    thread::Builder::new()
        .stack_size(64 * 1024)
        .spawn(move || tx.send(f()).ok())
        .unwrap();
    match rx.recv_timeout(Duration::from_secs(10)) {
        Ok(r) => println!("i128probe: {name} = {r}"),
        Err(_) => println!("i128probe: {name} HANG"),
    }
}

fn main() {
    check("u128 mul", || format!("{:x}", bb(u128::MAX / 3) * bb(3u128)));
    check("u64 widening", || format!("{:?}", bb(u64::MAX).carrying_mul(bb(u64::MAX), 0)));
    check("u128 carrying_mul", || format!("{:?}", bb(u128::MAX).carrying_mul(bb(u128::MAX), 0)));
    check("i128 carrying_mul", || format!("{:?}", bb(i128::MAX).carrying_mul(bb(i128::MAX), 0)));
    check("i128 carrying_mul_add", || {
        format!("{:?}", bb(i128::MAX).carrying_mul_add(bb(i128::MAX), 0, 0))
    });
    check("u128 div", || format!("{}", bb(u128::MAX) / bb(12345u128)));
    check("i128 div", || format!("{}", bb(i128::MIN + 1) / bb(-7i128)));
    println!("i128probe: done");
}
