# Battery audit, 2026-09-06

`c59f999b` changed the kernel's default to `CARD_POWER=KEEP`, bypassing
`File_PowerDown()` on suspend. The CPU still halted, clocks were gated, and
SDRAM entered self-refresh, but the SD supply stayed enabled. The 120-second
automatic shutdown timer starts again on **each** suspend: it does not bound
card uptime across repeated user input. A long reading session could keep
the card supplied throughout. Supply on does not mean continuous SD traffic;
the resulting standby current depends on the card and needs measurement.

## Changes

- Restore `CARD_POWER=OFF` by default. The next file operation reinitialises
  the card through the existing `AutoPowerUp()` path. `KEEP` remains available
  explicitly for latency comparisons. The Makefile now tracks the option so
  changing it rebuilds `suspend.o` without requiring a clean build.
- Retain the UI's established two-second idle debounce. An initial build
  skipped it when history was clean, but hardware testing reported loss of
  keyboard and button input followed by shutdown. That change is withdrawn
  pending diagnosis of the more frequent suspend/card-power transitions.
  Repainting an unchanged article position still no longer dirties history
  and starts another five-second wait. Real article additions, position
  changes, and clears retain their existing save policy.
- Report SD supply on/off time during skipped HALT intervals in `wremu`.
  This observes P32, the active-low supply enable in
  [`samo_a1.h`](../samo-lib/include/boards/samo_a1.h); chip deselection and
  disabling the bus buffer are different operations.

The internal-RAM decoder placements, overlays, caches, SDRAM retiming, and
DMA descriptor placement are retained.

## Comparison

**Historical experiment, not the recovery build:** the "Updated UI" row
below includes the immediate-suspend change withdrawn after the hardware
input failure. Its active-time savings must not be attributed to the current
build. The emulator delivers whole touch packets atomically and does not
model card supply discharge/startup, so these tests did not establish the
reliability of rapid hardware suspend/wake cycles.

Full FLASH boot, current GCC 16 `-O2` reader, calibrated emulator, Simple
English archive, identical initial card contents. Launch the ZIM reader,
type CAT, open the result, let it idle, then tap the stationary page:

```sh
./emulator/wremu -R -e samo-lib/mbr/flash.rom -c TEST_IMAGE.dmg \
  -T 40,36,100000000 -K 300000000,CAT -T 30,40,500000000 \
  -T 220,195,950000000 -n 1600000000
```

Install each comparison kernel as `kernel.elf` in its own image; the `after`
image also gets the updated app. Use full FLASH boot for this test: direct
kernel entry does not establish the same board/RAM configuration. Run in
separate directories to retain each `screen.pgm`.

| Build | Executed instructions | Time outside skipped HALT | SD supplied during HALT | SD off during HALT |
| --- | ---: | ---: | ---: | ---: |
| Previous UI, `KEEP` | 161,453,707 | 13,488.4 ms | 23,975.8 ms | 0.0 ms |
| Previous UI, `OFF` | 161,011,038 | 13,415.0 ms | 0.0 ms | 23,983.1 ms |
| Updated UI, `OFF` | 124,382,843 | 8,000.7 ms | 0.0 ms | 24,593.6 ms |

All three final Cat screens are byte-identical (SHA-1
`f94996fe9faa3998550c338927b3d05e3f775058`). No faults, unmapped accesses,
watchdog timeouts, or receive overflows were reported. With the same OFF
kernel, the UI change removes 36.6 million instructions and 5.41 seconds
outside HALT in this script. Time outside HALT is `(MCLK cycles - skipped
idle cycles) / 60 MHz`, including memory/bus waits. These are instruction
scheduled tests, not equal-duration battery discharge tests. The images are
read-only for this comparison, so successful history writes are tested
separately below.

A separate matched first-article window, from `retrieve_article` to entry
to `render_article_with_pcf`, took 776.43 ms with KEEP and 787.90 ms with OFF
using the same previous UI: **11.47 ms extra**. The driver includes 1 ms of
supply sequencing plus a 10 ms settling delay. A physical card's additional
startup time is not calibrated by the emulator, so this is not a prediction
of every card's wake penalty or a complete tap-to-painted-page measurement.

## Other recent changes

| Change | Battery assessment from code and existing timings |
| --- | --- |
| `fa2f88c9`, `9d29318f`, `79e75481`, `af765876`: decoder placement, font fast seeks, caching | Less active computation and card/SDRAM work. Favorable inference; no measured joules yet. They do not disable suspend or raise the CPU clock. |
| `43fa3b34`, `f7259eeb`: SDRAM timing and wake restore | Fewer unnecessary active-mode refreshes and less bus waiting. Suspend still uses SDRAM self-refresh. No new retention setting is introduced by this audit. |
| `d178e0d0`: incremental progress bar | Less framebuffer work per update; favorable inference. |
| `c2d20347`, `04107178`: DMA completion polling | The older HALT-based wait never woke on hardware. Production polls with a bound; emulator-only sleep savings must not be quoted as hardware battery savings. Keep the working path until a wake source is verified on silicon. |
| `fbf81b67`: 60 Hz coast and four-screen lookahead | Mixed: cached footers remove repeated rendering/I/O, but longer coasts and extra lookahead can do more work, including decoding images never viewed. The loop polls between frames. A separate workload/current comparison is needed to price this UI benefit. |
| `ZIM_BENCH=YES` | Diagnostic startup work and card logging add overhead. Production defaults to `ZIM_BENCH=NO`; use that for battery measurements. |

## Validation and next measurements

- Modern cross-compiler builds of Grifo, `zim.app`, and `wiki.app` pass,
  including the suspend/SDRAM no-stack checks. KEEP -> default OFF switches
  the compiled power-down call correctly; repeating OFF leaves `suspend.o`
  unchanged. `make -C emulator check` passes, including the added GPIO
  supply-state cases.
- A host harness linked the actual `wiki/history.c` and checked clean
  repaints, article additions, scroll debounce, saved positions, and clears.
- On a writable copy: open Cat, drag from `(120,180)` to `(120,40)`, wait for
  history to save, power off, reboot, and reopen via History. The scrolled
  page is byte-identical across the power cycle (SHA-1
  `c4b99dd84995b2586eca79230d0ab7a820ca9769`). The run writes 266 sectors,
  shuts down normally, and has no faults or watchdog resets.
- An accelerated automatic-shutdown test (`WREMU_SUSPEND_DIV=60`) powers
  off normally after approximately two seconds in suspend, with the SD
  supply disabled throughout that wait.

Remaining opportunities are the history/idle debounce (two/five seconds
of polling), inter-frame waits while coasting, and held-key/link timers.
Sleeping through those requires an event wait with a deadline: ordinary
`event_wait()` can sleep until user input or the 120-second shutdown, and
the application's `timer_get()` counter is gated during deep suspend.
Replacing polling with that call alone would strand the deferred work.

Measure battery-input current on the real board with the same SD card,
screen, and production app for OFF and KEEP: settled idle, first read after
wake, and a repeatable read/scroll/pause session. Integrate startup energy
as well as idle power. A possible future delayed-card-shutdown policy needs
the measured break-even interval
`restart energy / (powered-idle power - card-off idle power)` first.
The emulator currently measures state residency and timing, not current,
card-specific standby behavior, or battery discharge; no battery-life
percentage is established by these results.

## Optional hardware shutdown report

Create `powerlog.on` on the boot volume to enable the kernel's diagnostics
at the next boot. Counters stay in RAM; no diagnostic file is written on
suspend or resume. Normal power-button shutdown or automatic shutdown
overwrites `power.txt` with one snapshot. Removing the marker disables
collection and the shutdown write on the next boot. There is still a small
boot-time check for the marker. Use the marker only for functional testing;
its final card write adds shutdown work.

After waking and opening a few articles, expected fields are:

```text
policy card=OFF auto_off=120s refresh=0x120
suspend entries=... resumes=... timeouts=...
sd_supply on_entries=0 on_resumes=0
refresh last=0x120 mismatches=0
card reinit=... failures=0 total_ms=... max_ticks=... ticks_per_ms=60000
dma: ok, ... blocks
```

`entries`/`resumes` should be nonzero, and `timeouts` counts automatic
suspend timeouts. Supply fields sample the GPIO before and after the low
power routine; they do not measure electrical current. `card reinit` counts
file-triggered reinitialisations after boot (including file closes), excluding
the report's own restart. Divide `max_ticks` by `ticks_per_ms` for the longest
restart in milliseconds. The normal timer is gated during suspend, so this
report deliberately makes no claim about time spent asleep. The report also
contains the kernel compile date/time and compiler version. After a crash or
removed batteries it may be absent or still describe the previous completed
session; it is not a crash recorder.

The benchmark app now prints its compile date/time and compiler version to
`bench.txt` at startup. Its CPU tests are stored in SDRAM and temporarily
copied into A0 RAM, saving and restoring the decoder code. This avoids
overflowing the almost-full A0 region while retaining the same test loops.

## Hardware failure follow-up

Restoring the two-second idle debounce improved keyboard use, but a later
normal-reader test shut off about 15 seconds after scrolling Tokyo. The
card contained the new Tokyo scroll position (844), but no `power.txt` or
`dma.txt`. The programmed watchdog limit is 960,000,000 clocks, about 16
seconds at the active 60 MHz clock; the timing is consistent with a hang,
but there is no captured watchdog reset cause or instruction address.
The intended idle shutdown remains 120 seconds.

The next diagnostic kernel uses `CARD_POWER=KEEP` to isolate card power
cycling while retaining CPU suspend and the same reader apps. This is a
temporary hardware test, not a battery optimisation. The source default
remains OFF. In KEEP builds, `powerlog.on` plus `pwrtrace.on` enables
`pwrtrace.txt` checkpoints at boot, before suspend, and after resume. Each
checkpoint closes the file so the last written phase can survive a later
hang. The file also contains build identity, counts, clock/refresh registers,
and watchdog count/limit. It is a phase marker, not a crash backtrace.

Checkpoint writes deliberately add card traffic and can change race timing.
Remove `pwrtrace.on` for a subsequent comparison without that I/O; remove
both diagnostic markers for battery measurements. OFF builds ignore the
trace marker so logging cannot repower the card just before halt. A normal
shutdown still writes `power.txt`; a crash can leave it missing or stale.

The first physical KEEP/checkpoint run completed Cat -> Tokyo -> scroll ->
30-second idle -> History -> Cat -> power-button shutdown. The saved report
records 8 suspend entries and 8 resumes, no timeouts, SD supply on at every
sample, no refresh mismatches, no card reinitialisations, and healthy DMA
after 2,261 blocks. The last checkpoint is `after resume`. This establishes
that this diagnostic configuration completed the test; it does not separate
the KEEP policy from the timing effect of its checkpoint writes. The next
comparison keeps the identical kernel/apps and disables only `pwrtrace.on`,
while retaining the RAM counters and shutdown report.

That physical comparison also completed normally: 10 entries/resumes,
zero refresh mismatches, zero card restarts, and healthy DMA after 2,102
blocks. `pwrtrace.txt` was absent as expected. This strengthens the evidence
against the card power-cycle path, without proving a particular failure.

The driver had an independently testable bug: `turn_off_power()` always
selected and polled the card, even with its supply and buffer already off.
Input-only wakeups can reach another suspend without `AutoPowerUp()`, so
power-off must be idempotent. It now drains/deselects only a powered card,
then disables the supply and marks it uninitialised. The host regression
test compiles the real `mmc.c`, checks that pending writes drain before
power-off, and rejects any access to an already unpowered bus. It fails on
the old driver and passes on the fix, including 100 repeated power-offs.

The OFF kernel with this fix passes the emulator Tokyo/scroll/shutdown
sequence: 6 entries/resumes, supply off at every suspend sample, 4 successful
restarts, no refresh mismatches, and healthy DMA. CPU suspend/retiming bytes
are unchanged. The boot file-loader still links within its A0 region.
An exploratory emulator variant returning zero on an unpowered, selected
SD bus also runs the new reader; old code instead spends millions of SPI
exchanges polling and misses the scripted launcher tap. That is a fault
stimulus, not a measured voltage on this card or proof of the physical
watchdog failure.

The physical OFF-fix retest completed the same Cat -> Tokyo -> scroll ->
30-second idle -> History -> Cat -> power-button shutdown sequence without
unexpected shutdown. The new `power.txt` records:

```text
policy card=OFF auto_off=120s refresh=0x120
suspend entries=10 resumes=10 timeouts=0
sd_supply on_entries=0 on_resumes=0
refresh last=0x120 mismatches=0
card reinit=5 failures=0 total_ms=887 max_ticks=10752660 ticks_per_ms=60000
dma: ok, 2255 blocks
```

Kernel SHA-256:
`b76a08fad807f69f7bb1129ebdfac2b950ed696b17a30ce70f55607eedb89121`.
No `dma.txt` or `pwrtrace.txt` was present. History preserved Tokyo's scroll
position (839). This validates the reported workflow with the fix and SD
power-off restored; it does not establish long-duration reliability or a
measured battery-life increase. The two-second input debounce remains.

The tested reader builds also included the pre-existing compressed-input
placement change in `zim_blob.c` (SDRAM bank 1). That separate change is
outside this battery-fix commit.

On this physical card, five restarts averaged **177.4 ms**, with a maximum
of **179.21 ms**. The earlier emulator restart of roughly 11.5 ms represents
only its simplified startup model. Use the physical latency for this card
when considering the wake-time tradeoff; energy still needs current
measurement. Shutdown diagnostics remain enabled for ordinary follow-up
use, with counters in RAM and one report write at shutdown. Remove
`powerlog.on` before measuring battery consumption.
