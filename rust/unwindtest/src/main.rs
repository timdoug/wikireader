//! unwindtest - panics that unwind on C33 Linux: catch_unwind, destructors
//! run on the way out, a panicking thread's join, a panic's payload, and a
//! second panic caught after the first.

use std::panic;
use std::sync::atomic::{AtomicU32, Ordering};
use std::thread;

static DROPS: AtomicU32 = AtomicU32::new(0);

struct Guard(u32);

impl Drop for Guard {
    fn drop(&mut self) {
        DROPS.fetch_add(self.0, Ordering::SeqCst);
    }
}

#[inline(never)]
fn deep(n: u32) -> u32 {
    let _g = Guard(1 << n);
    if n == 0 {
        panic!("bottom reached");
    }
    deep(n - 1) + 1
}

fn main() {
    // The default hook would print each expected panic.
    panic::set_hook(Box::new(|_| {}));

    let caught = panic::catch_unwind(|| deep(4));
    let message = caught
        .as_ref()
        .err()
        .and_then(|p| p.downcast_ref::<&str>().copied())
        .unwrap_or("?");
    println!("unwindtest: caught {message:?}, drops {:#x}", DROPS.load(Ordering::SeqCst));

    let joined = thread::Builder::new()
        .stack_size(32 * 1024)
        .spawn(|| -> u32 { panic!("thread {}", 7) })
        .unwrap()
        .join();
    let payload = match joined {
        Ok(v) => format!("no panic: returned {v}"),
        Err(p) => match p.downcast::<String>() {
            Ok(s) => format!("{s:?}"),
            Err(p) => match p.downcast::<&str>() {
                Ok(s) => format!("&str {s:?}"),
                Err(_) => "an unknown payload".to_string(),
            },
        },
    };
    println!("unwindtest: thread panicked with {payload}");

    let again = panic::catch_unwind(|| {
        let _g = Guard(0x100);
        Vec::<u8>::new()[3]
    });
    println!(
        "unwindtest: second panic caught {}, drops {:#x}",
        again.is_err(),
        DROPS.load(Ordering::SeqCst)
    );
    println!("unwindtest: done");
}
