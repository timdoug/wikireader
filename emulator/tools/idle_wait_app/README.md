# Firmware timed-wait regression

This app exercises syscall 47 through the actual kernel, including timer 2
programming, interrupt masking, and the 32-bit application clock. Build the
current Grifo kernel/library first, then:

```sh
make -C emulator/tools/idle_wait_app TOOLCHAIN_BIN=/path/to/toolchain/bin
```

Install `idle_wait.app` as `idle.app` in a disposable emulator card image,
alongside the new `kernel.elf` and the usual `init.app`/`zim.ico`. Set its
`init.ini` to the single line `zim.ico : idle.app`. This starts the test
automatically. Do not replace the launcher configuration on a regular card.

Find `idle_input_start` in `idle_wait.map`, and use its address as the anchor:

```sh
./emulator/wremu -e samo-lib/mbr/flash.rom -c TEST_IMAGE.dmg \
    -Z 0xADDRESS -N 2,5000000 -n 800000000
```

The test checks zero/one-microsecond waits, the one-second cap, 200 repeated
20 ms deadlines, a clock wrap during a wait, an input interrupt, and an
already queued button release. Each wait checks clock/interrupt restoration
and timer cleanup. Success prints `idle wait: ALL TESTS PASSED`, followed by
normal power-off. With `powerlog.on`, shutdown also writes the idle counters.

`make -C emulator test-event-wait` separately runs the real event queue code
with deterministic scheduling: input between the empty read and the masked
queue check, partial UART wakeups without complete events, input during the
wait, and deadline preservation. It does not model UART electrical timing.
