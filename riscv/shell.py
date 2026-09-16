#!/usr/bin/env python3
"""Boot Linux on the emulated WikiReader and talk to its shell.

Waits for the prompt rather than guessing when it will appear -- the boot
takes tens of millions of guest instructions and the number is not stable
between builds -- then sends each command and prints what comes back.
Input goes in the way a person's would: bytes on the serial line, which
Grifo hands to the application, which hands them to the guest's 8250.
"""
import argparse
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time

import run as harness

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path,
                        default=HERE / 'linux/Image')
    parser.add_argument('--dtb', type=Path,
                        default=HERE / 'linux/wr-sh.dtb')
    parser.add_argument('--app', type=Path, default=HERE / 'riscv.app')
    parser.add_argument('--command', action='append', default=[])
    parser.add_argument('--prompt', default='# ')
    parser.add_argument('--timeout', type=int, default=2400)
    args = parser.parse_args()

    for needed in (args.image, args.dtb):
        if not needed.exists():
            raise SystemExit(f'{needed} is missing; run ./fetch-linux.sh first')

    out = Path(tempfile.mkdtemp(prefix='wr-linux-'))
    card = out / 'card.img'
    harness.make_card(card, {
        'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'INIT.APP': args.app.read_bytes(),
        'RVBENCH.BIN': args.image.read_bytes(),
        'RVBENCH.DTB': args.dtb.read_bytes(),
    })
    flash = out / 'flash.rom'
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(flash)], check=True, stdout=subprocess.DEVNULL)

    cmd = [str(ROOT / 'emulator/wremu'), '-R', '-c', str(card), '-e', str(flash),
           '-n', '200000000000', '--uart-input', '-', '--uart-start', '1',
           # One byte per 400k instructions.  The emulated UART holds six
           # bytes and the application drains it once per batch of guest
           # instructions, which is far longer; at a tighter gap the first
           # characters of a typed line are lost to FIFO overrun.
           '--uart-gap', '400000']
    print(' '.join(cmd), flush=True)
    log = (out / 'run.log').open('wb')
    proc = subprocess.Popen(cmd, cwd=out, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # Read in chunks.  A byte at a time backs the pipe up and throttles the
    # emulator to a crawl -- the boot stalled at seven guest seconds.
    pending = list(args.command)
    tail = b''
    deadline = time.time() + args.timeout
    sent_at = 0.0
    fd = proc.stdout.fileno()
    while time.time() < deadline:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        log.write(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.flush()
        tail = (tail + chunk)[-256:]
        # The prompt is only a prompt when nothing has been sent recently:
        # the shell echoes what it is given, prompt included.
        if tail.rstrip(b'\r\n ').endswith(args.prompt.strip().encode()) \
                and time.time() - sent_at > 3:
            if pending:
                proc.stdin.write(pending.pop(0).encode() + b'\n')
                proc.stdin.flush()
                sent_at = time.time()
                tail = b''
            elif sent_at:
                break
    # Ask for a clean stop: the emulator writes the panel out and prints
    # its counters on the way, which a kill would lose.
    proc.terminate()
    end = time.time() + 30
    while time.time() < end:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        log.write(chunk)
    proc.wait(timeout=30)
    log.close()
    print(f'\nfull log: {out / "run.log"}')
    print(f'panel:    {out / "screen.pgm"}')
    return 0 if not pending else 1


if __name__ == '__main__':
    sys.exit(main())
