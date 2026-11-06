//! stdhello - Rust's standard library on C33 Linux: arguments, the
//! environment, files, threads, a channel, time and a collection.

use std::collections::BTreeMap;
use std::sync::mpsc;
use std::time::Instant;
use std::{env, fs, thread};

fn main() {
    let start = Instant::now();
    let args: Vec<String> = env::args().collect();
    println!("stdhello: {} args, first {:?}", args.len(), args.first());
    println!("stdhello: HOME={:?}", env::var("HOME").ok());

    let path = env::temp_dir().join("stdhello.txt");
    fs::write(&path, "written by Rust on the C33\n").expect("write");
    let back = fs::read_to_string(&path).expect("read");
    fs::remove_file(&path).expect("remove");
    print!("stdhello: file round trip: {back}");

    let (tx, rx) = mpsc::channel();
    let workers: Vec<_> = (0..4u32)
        .map(|i| {
            let tx = tx.clone();
            thread::Builder::new()
                .stack_size(16 * 1024)
                .spawn(move || tx.send((i, (1..=1000u32).map(|n| n * (i + 1)).sum::<u32>())).unwrap())
                .expect("spawn")
        })
        .collect();
    drop(tx);
    let sums: BTreeMap<u32, u32> = rx.iter().collect();
    for w in workers {
        w.join().unwrap();
    }
    println!("stdhello: thread sums {sums:?}");
    println!("stdhello: done in {:?}", start.elapsed());
}
