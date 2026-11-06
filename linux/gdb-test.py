#!/usr/bin/env python3
"""Debug a program on C33 Linux with GDB, through Grifo, in wremu.

    gdb-test.py [--gdbserver PATH] [--rootfs IMAGE --symbols DIR]

Boots MBR -> Grifo -> init.app -> linux.app with the console also served on
a local TCP port (wremu --uart-tcp), starts gdbserver on the console with
debug/gdb-target.c's program, and runs the host's c33-linux-musl-gdb
against it over that port: breakpoints, backtraces, stepping into the
program and through the PLT into libc, finish, a watchpoint, a signal
handler's frame, a second thread, and the program's exit.

gdbserver talks on its stdin and stdout (gdbserver -), the console, in
raw mode; the program's own output goes to gdbserver's stderr, a file.
Given a tty to debug on, gdbserver would hand that terminal to the
program, as a shell does, and then read it from the background.  The
kernel console is quieted first, so nothing else writes on the line.

--gdbserver puts that gdbserver on the card instead of the image's.
--symbols names the host ELFs that match --rootfs's image.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
GDB = root / "host-tools/toolchain-c33/work/install/bin/c33-linux-musl-gdb"

# GDB's commands, and what its output must show, in order.  {name} is the
# line of debug/gdb-target.c marked "@name".
SESSION = [
    ("break add", r"Breakpoint 1 at 0x[0-9a-f]+: file .*gdb-target.c, line {add}"),
    ("continue", r"Breakpoint 1, add \(a=0, b=0\) at .*gdb-target.c:{add}"),
    ("bt", r"#0  add \(a=0, b=0\).*\n#1  0x[0-9a-f]+ in main \(argc=1, argv="),
    ("next", r"{add-next}\t\tcounter\+\+;"),
    ("print sum", r"\$1 = 0"),
    ("finish", r"in main \(argc=1.*Value returned is \$2 = 0"),
    ("delete", r""),
    ("info sharedlibrary", r"ld-musl-c33.so.1"),
    # Stepping through the PLT into libc, out again, and into a function
    # of the program.
    ("tbreak {strlen}", r"Temporary breakpoint \d+ at .*line {strlen}"),
    ("continue", r"main \(argc=1, .*\) at .*:{strlen}"),
    ("step", r"strlen \(s=0x[0-9a-f]+ \"wikireader\"\)"),
    ("finish", r"Value returned is \$3 = 10"),
    ("next", r"{depth-call}\t\ttotal \+= depth\(4\);"),
    ("step", r"depth \(n=4\) at .*gdb-target.c:{depth}"),
    # A software watchpoint, single-stepping until the value changes: the
    # C33 has no watchpoint hardware.
    ("set can-use-hw-watchpoints 0", r""),
    ("watch counter", r"Watchpoint \d+: counter"),
    ("continue", r"Old value = 3\s+New value = 7"),
    ("delete", r""),
    ("break depth if n == 1", r"Breakpoint \d+ at"),
    ("continue", r"Breakpoint \d+, depth \(n=1\)"),
    ("bt", r"#0  depth \(n=1\).*\n#1 .*depth \(n=2\).*\n#2 .*depth \(n=3\)"
           r".*\n#3 .*depth \(n=4\).*\n#4 .*main"),
    ("print counter", r"\$4 = 12"),
    ("info registers pc sp psr", r"pc\s+0x[0-9a-f]+\s+0x[0-9a-f]+ <depth\+\d+>\s*\n"
                                 r"sp\s+0x[0-9a-f]+.*\npsr\s+0x[0-9a-f]+\s+\[ .*IE"),
    ("delete", r""),
    # Out of a signal handler, through the trampoline in the signal frame.
    ("handle SIGUSR1 nostop noprint pass", r""),
    ("break on_signal", r"Breakpoint \d+ at .*line {on-signal}"),
    ("continue", r"on_signal \(sig=10\) at .*:{on-signal}"),
    ("bt", r"#0  on_signal \(sig=10\).*\n#1  <signal handler called>"
           r".*main \(argc=1, .*:{raise}"),
    ("delete", r""),
    # A second thread: stopped in it, both listed, each backtraced.
    ("break worker", r"Breakpoint \d+ at .*line {worker}"),
    ("continue", r"Thread .* hit Breakpoint \d+, worker \(arg=0x[0-9a-f]+\) at .*:{worker}"),
    ("info threads", r"  1 +Thread .*\n\* 2 +Thread .*worker \(arg="),
    ("thread apply all bt 1", r"Thread 2 .*\n#0  worker.*Thread 1 .*\n#0 "),
    ("print *(int *)arg", r"\$5 = 100"),
    # The thread's first frame is __clone's child side, whose return
    # address is undefined: the backtrace ends there, with nothing after.
    ("bt", r"#0  worker .*\n#1 .* in start .*\n#2 .* in __clone \(\)[^\n]*\s*$"),
    # The main thread, stopped in libc's assembly, unwinds to main.
    ("thread 1", r"Switching to thread 1"),
    ("bt", r"#\d+ .*main \(argc=1, .*:({join}|\d+)"),
    ("delete", r""),
    # Through ld.so's lazy binding: the resolver's frame and the PLT stub's
    # back to the call in main, the first to an unbound function.
    ("break __c33_lazy_bind", r"Breakpoint \d+ at"),
    ("continue", r"__c33_lazy_bind"),
    ("bt", r"#0 .*__c33_lazy_bind.*\n#1 .*__c33_lazy_resolve.*"
           r"main \(argc=1, .*:({join}|{sigsetjmp})"),
    ("delete", r""),
    # setjmp called by sigsetjmp, which has popped its return address and
    # keeps it in env across the call.
    ("break ___setjmp", r"Breakpoint \d+ at"),
    ("continue", r"Breakpoint \d+, .*setjmp"),
    ("bt", r"#0 .*setjmp \(\).*\n#1 .* in sigsetjmp \(\).*\n#2 .*main \(argc=1, .*:{sigsetjmp}"),
    ("continue", r"\[Inferior 1 \(process \d+\) exited normally\]"),
]


def marked_lines():
    lines = {}
    source = root / "linux/debug/gdb-target.c"
    for number, text in enumerate(source.read_text().splitlines(), 1):
        for name in re.findall(r"/\* @([\w-]+) \*/", text):
            lines[name] = str(number)
    return lines


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kernel", type=Path, default=root / "linux/artifacts/linux.app")
    parser.add_argument("--rootfs", type=Path, default=root / "linux/artifacts/linux.img")
    parser.add_argument("--gdbserver", type=Path,
                        help="a gdbserver for the card, instead of the image's")
    parser.add_argument("--output", type=Path, default=root / "linux/artifacts/gdb-test")
    parser.add_argument("--symbols", type=Path, default=root / "linux/artifacts/symbols",
                        help="the host ELFs matching the image, GDB's sysroot")
    args = parser.parse_args()

    spec = importlib.util.spec_from_file_location("app", root / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(root)
    files = {name: app.require(path).read_bytes() for name, path in {
        "kernel.elf": root / "samo-lib/grifo/grifo.elf",
        "init.app": root / "samo-lib/grifo/applications/init/init.app",
        "linux.app": args.kernel, "linux.img": args.rootfs,
        "linux.ico": root / "linux/artifacts/linux.ico",
        "gdbtgt.bin": root / "linux/artifacts/gdbtgt.bin",
    }.items()}
    files["init.ini"] = b"linux.ico : linux.app\n" * 2
    server = "gdbserver"
    if args.gdbserver:
        files["gdbsrv.bin"] = app.require(args.gdbserver).read_bytes()
        server = "/tmp/gdbserver"
    app.require(GDB)
    target = app.require(args.symbols / "mnt/sd/gdbtgt.bin")

    args.output.mkdir(parents=True, exist_ok=True)
    log_path = args.output.resolve() / "serial.log"
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="wr-gdb-test-") as temporary:
        out = Path(temporary)
        card, flash = out / "card.img", out / "flash.rom"
        fat.make_image(card, files, 64)
        subprocess.run([sys.executable, str(root / "samo-lib/mbr/make-flash.py"), str(flash)],
                       check=True, stdout=subprocess.DEVNULL)
        with log_path.open("w") as log:
            emulator = subprocess.Popen(
                [str(app.require(root / "emulator/wremu")), "-n", "200000000000",
                 "-T", "40,36,100000000", "--uart-tcp", str(port),
                 "--uart-start", "850000000", "-c", str(card), "-e", str(flash)],
                cwd=out, stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, "WREMU_HOLD_MS": "33"})
            try:
                return session(emulator, port, server, args.output, log_path,
                               args.symbols, target)
            finally:
                emulator.kill()
                emulator.wait()


def session(emulator, port, server, output, log_path, symbols, target):
    # Wait for the shell on the console, then start gdbserver on it.
    deadline = time.monotonic() + 900
    while True:
        try:
            console = socket.create_connection(("127.0.0.1", port), timeout=5)
            break
        except OSError:
            if emulator.poll() is not None or time.monotonic() > deadline:
                raise SystemExit(f"wremu never served the console; see {log_path}")
            time.sleep(0.5)
    with console:
        seen = b""
        console.settimeout(1)
        while b"/ # " not in seen:
            if emulator.poll() is not None or time.monotonic() > deadline:
                raise SystemExit(f"no shell prompt; see {log_path}")
            try:
                seen += console.recv(4096)
            except socket.timeout:
                console.sendall(b"\n")
        copy = ("cp /mnt/sd/gdbsrv.bin /tmp/gdbserver && chmod +x /tmp/gdbserver && "
                if server.startswith("/tmp") else "")
        console.sendall((f"dmesg -n 1; {copy}stty raw -echo; echo GDBSERVER-UP; "
                         f"{server} --once --no-startup-with-shell - /mnt/sd/gdbtgt.bin "
                         "2>/tmp/gdbserver.log; stty sane\n").encode())
        seen = b""
        while b"GDBSERVER-UP" not in seen.replace(b"echo GDBSERVER-UP", b""):
            if emulator.poll() is not None or time.monotonic() > deadline:
                raise SystemExit(f"gdbserver did not start; see {log_path}")
            try:
                seen += console.recv(4096)
            except socket.timeout:
                pass

    command = [str(GDB), "-batch", "-nx",
               "-ex", f"set sysroot {symbols}",
               "-ex", "set remotetimeout 120",
               "-ex", f"file {target}",
               "-ex", f"target remote 127.0.0.1:{port}"]
    lines = marked_lines()
    session = [(re.sub(r"\{([\w-]+)\}", lambda m: lines[m[1]], c),
                re.sub(r"\{([\w-]+)\}", lambda m: lines[m[1]], p)) for c, p in SESSION]
    for gdb_command, _ in session:
        command += ["-ex", f"echo \\n@@ {gdb_command}\\n", "-ex", gdb_command]
    result = subprocess.run(command, capture_output=True, text=True, timeout=3600)
    transcript = result.stdout + result.stderr
    (output / "gdb.log").write_text(transcript)

    failures = []
    parts = re.split(r"^@@ (.*)$", transcript, flags=re.M)
    replies = dict()
    for i in range(1, len(parts) - 1, 2):
        replies.setdefault(parts[i], []).append(parts[i + 1])
    seen_count = {}
    for gdb_command, pattern in session:
        n = seen_count.get(gdb_command, 0)
        seen_count[gdb_command] = n + 1
        reply = replies.get(gdb_command, [])
        text = reply[n] if n < len(reply) else ""
        if pattern and not re.search(pattern, text, re.S):
            failures.append(f"{gdb_command!r}: wanted /{pattern}/, got:\n{text.strip()}")
    if failures:
        print("\n\n".join(failures))
        raise SystemExit(f"GDB session failed; see {output / 'gdb.log'} and {log_path}")
    print(f"GDB PASS: {len(session)} commands over gdbserver on the console, through Grifo")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
