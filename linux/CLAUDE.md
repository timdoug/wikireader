# NEVER BYPASS GRIFO

Everything on the WikiReader runs through Grifo, so every emulator run does
too: the real MBR flash (`samo-lib/mbr/make-flash.py`), Grifo as
`kernel.elf`, `init.app`, and the application named in `init.ini`.  That holds
for regression tests, timings, profiles and one-off checks alike.

Never boot an application or kernel directly: not through NuttX's
`make_boot_fixture.py`, not through the file loader, not through a direct ELF.
That path runs at the 48 MHz reset clock instead of Grifo's 60 MHz, clocks
the card at 12 MHz instead of 15, and skips Grifo's hardware setup, so its
numbers and behaviour are not the product's.  A script that already does it
is not a reason to use it; move the script onto Grifo.

Launcher-path tools: `linux/boot-test.sh`, `linux/app-test.py`;
`linux/artifacts/perf/ask-app.sh`
(types commands on the serial shell and powers off).
