//! sdprobe - the cost of a small appending write, on the card (mounted
//! synchronous, so each write reaches the card before it returns) and on
//! tmpfs: what a println! to a file on /mnt/sd costs.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::time::{Duration, Instant};

const WRITES: usize = 16;

fn probe(path: &str) -> Vec<Duration> {
    let _ = fs::remove_file(path);
    let mut file = OpenOptions::new().create(true).append(true).open(path).expect("open");
    let mut times = Vec::with_capacity(WRITES);
    for i in 0..WRITES {
        let line = format!("sdprobe line {i:02} of a typical length for a log line\n");
        let start = Instant::now();
        file.write_all(line.as_bytes()).expect("write");
        times.push(start.elapsed());
    }
    drop(file);
    fs::remove_file(path).expect("remove");
    times.sort();
    times
}

fn main() {
    for (name, path) in [("card", "/mnt/sd/sdprobe.txt"), ("tmpfs", "/tmp/sdprobe.txt")] {
        let t = probe(path);
        let us = |d: Duration| d.as_micros();
        println!(
            "sdprobe: {name} append x{WRITES}: min {} us, median {} us, max {} us, total {} us",
            us(t[0]),
            us(t[WRITES / 2]),
            us(t[WRITES - 1]),
            us(t.iter().sum())
        );
    }
}
