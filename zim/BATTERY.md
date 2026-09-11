# Power management

The kernel defaults to `CARD_POWER=OFF`: deep suspend gates clocks, puts
SDRAM in self-refresh, and removes the SD supply. The next file operation
reinitialises the card. `CARD_POWER=KEEP` leaves the card powered during
deep suspend for latency comparisons; standby current depends on the card.
The 120-second automatic shutdown deadline restarts on each deep suspend.

## Reader idle behavior

The reader retains the two-second input debounce and five-second history
save delay. Repainting an unchanged article position does not dirty history.
While only those delays are pending, it uses 20 ms CPU HALT waits; input
interrupts end the wait immediately. Rendering, coasting, and held-key work
retain their nonblocking paths. The short waits keep application/touch
clocks and the SD supply running; subsequent deep suspend powers off the card.

`event_wait_timeout(event, microseconds)` is syscall 47. Zero polls the
queue; requests above one second are capped. Timer 2 uses MCLK/1024, and
partial input wakeups retain the original deadline. Queue rechecks close
the input-arrival race, and clock/interrupt state is restored on return.
Install matching kernel and reader builds: older kernels lack this syscall.

Two hardware findings constrain this policy:

- Removing the input debounce caused missed input and shutdowns. Retain it
  unless a replacement passes physical input and suspend/wake testing.
- Power-off must be idempotent: drain/deselect a powered card only, then
  disable its supply and mark it uninitialised. Polling an already unpowered
  card can hang. The real-driver host regression covers repeated power-off.

DMA completion uses bounded polling. Its earlier HALT-based wait worked in
the emulator but never woke on the physical device.

## Validation and measurement limits

The physical typing, Cat -> Tokyo -> scroll -> idle -> History -> Cat -> shutdown
workflow passed on 2026-09-06. The timed-idle report recorded nine successful
deep-suspend resumes, SD off at every deep-suspend sample, eight successful
card restarts, no SDRAM refresh mismatches, and healthy DMA. The 2,508 short
HALTs included timer and input wakeups, with a maximum interval of 20.005 ms.
Saved history contained both articles and Tokyo's scroll position.

Card restarts averaged 174.4 ms and peaked at 176.2 ms on that card. The
emulator's roughly 11.5 ms restart models only simplified sequencing and
settling, so it substantially understates this physical wake penalty.

Regression coverage includes the real event queue and card-power driver,
the [firmware idle-wait app](../emulator/tools/idle_wait_app/README.md), and
writable emulator history-save, reboot/reopen, and automatic-shutdown runs.
The emulator reports SD-on/off residency during HALT via the board's supply
GPIO. That is neither electrical current nor a battery discharge model.
No measured battery-life percentage is established.

For energy comparisons, measure battery-input current with the same card,
screen, and workflow under OFF and KEEP, including startup energy and
settled idle. Disable diagnostic markers first.

## Optional diagnostics

Create `powerlog.on` on the boot volume to enable RAM counters at the next
boot. Orderly power-button or automatic shutdown overwrites `power.txt`
with a snapshot; suspend/resume does not write a log. Remove the marker to
disable collection and the shutdown write on the next boot.

The report includes:

- `policy`, `suspend`, and `sd_supply`: configured behavior and samples at
  entry/resume, including automatic-suspend timeouts;
- `refresh`: last register value and mismatches;
- `card reinit`: file-triggered restarts, failures, total time and longest
  restart, excluding the report's own restart;
- `idle`: short HALT count, timer deadlines, accumulated interval and maximum;
- `dma`: backend health and block count; and
- kernel build identity and `ticks_per_ms`, used to convert tick counts.

The normal application timer is gated in deep suspend, so the report does
not time deep sleep. Following a crash or battery removal it may be missing
or stale; it is not a crash recorder.

For KEEP builds only, `powerlog.on` plus `pwrtrace.on` adds boot and
before/after-suspend checkpoints to `pwrtrace.txt`. Those writes can change
race timing. OFF builds ignore the trace marker so logging cannot repower
the card immediately before suspend. Remove both markers for energy tests.

Detailed historical experiments remain in Git at `7aa4ee84:zim/BATTERY.md`.
