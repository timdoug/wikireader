# wremu timing accuracy — current handoff

Updated 2026-11-05 after two new physical probe runs and the subsequent review. See `README.md` for
parameters and `tools/pipeline_bench/README.md` for the instruction measurements.

Latest disposition: both physical reports are archived, the card is ejected,
and no hardware action is pending. The SELDO experiment below was rejected
after a real Grifo suspend regression and reverted. Current `wremu` is again
`8dafbeab7a48923721a08de67efb8a6a5e74a4d8274a6a9346fc85816d06a235`.
The maintained `make test-suspend` now checks deep sleep, wake, configuration
restoration and retained RAM; it is part of `make check`. This is a functional
check, not a physical wake-timing qualification. Overall accuracy remains
incomplete; there is no new promoted timing model from either returned probe.

The executing CPU has an **opt-in** local fetch/decode/data-port scheduler.
The reference five-suite calibration before the IQB trials is:
`e087d459dcf544ce7bdce45569694c037948838809f5015385b7b268cb3191c6`.
Default and experimental (`local_frontend=1,itc_wait=4,dma_write_issue=2`)
runs pass all 738 native cases plus functional/122-case SD DMA checks.
Evidence: `build/wr128/write-width-review/verification.json`,
`final-default-accuracy.json` and `final-accuracy.json`. The experimental
counts within 10% are SPI transfer 125/128, SPI solo 120/128, memory 107/120,
NuttX 75/90, RISC-V 39/40 and pipeline 329/360. Memory maximum is now 16.07%.
Timing tradeoffs and workflow gaps remain; the frontend/DMA overrides are
not promoted. Narrow SDRAM write beat accounting is the default correction.

The working binary executes asynchronous DMA phases and SPI edges on a shared
timeline, samples source data at read completion and commits IDMA descriptors
after the payload. It retains the fetch-response, clock-accounting and accepted-
write coherence fixes, with deferred instruction fills off by default:
`8dafbeab7a48923721a08de67efb8a6a5e74a4d8274a6a9346fc85816d06a235`.
See the final section for current settings and live validation handles.
The preceding default baseline `bc75d10a` and its complete evidence remain in
`build/wr128/dma-write-review/verification.json` and `accuracy.json`.
The earlier c4cc7459 binary removes the host's false stop after nine
valid NOPs and passes the full functional/560-case variable-unroll checks.
DMA destination/data-wait fixes pass functional, reader/bus and complete
five-suite native checks. Timing changes are mixed and the SPI aggregate
scores regress; see the final section for the complete assessment.
A fetch-response ordering correction has completed qualification; the latest
word-boundary experiment is described in the last section. The new port probe is installed on the
physical card, old probes were backed up/removed at the user's request, and
the card was verified after remount then safely ejected for its hardware run.
The overall milestone is incomplete. The 264-case hardware bus probe has now
been collected and verified in `tools/bus_bench/device.json`. Its matched
controls reject a generic load-result interlock; see the new findings below.
The reader hardware report is now collected and compared in
`tools/workflow_bench/device.json` and
`build/wr128/workflow-calibration/hardware-comparison.json`. The returned card
is safely ejected. No further hardware input is required to investigate the
current frontend and shared-bus gaps.

## Use matching workloads

`python3 emulator/tools/score_model.py` runs five calibration sets through
FLASH -> Grifo -> launcher -> application: SPI/DMA, memory copies, NuttX
ubench, four current RISC-V placements and the 360-case pipeline probe.
It preserves binaries, reports and input hashes. Ubench checks code size and
instruction counts; pipeline and fresh RISC-V reports have application/boot
hashes. RISC-V also checks every kernel's counts and native checksums.

The original four `riscv/rv*-device.txt` placement reports are **not valid
calibration for today's binaries**. They include console time and predate
changes to division and dispatch. `riscv/README.md` documented this, but
`fit-model.py` still treated them as an oracle. The apparent roughly 2x
assembly division error disappears with matching binaries. `fit-model.py`
now uses fresh calibration by default; `--historical` labels the old
comparison as diagnostic. `riscv/run.py` now includes the real launcher.

Fresh reports and their manifest are in `tools/riscv_bench/device.json`.
The four-run probe chains after closing each report, captures checksums in
the untimed console region, and powers off after the last application.
All 40 kernel checksums pass on hardware and in the emulator. The collector
finds the returned card itself; never ask the user for the log's path.

## Established fixes

- `cycle_cost()` charged extended immediate/register loads an extra memory
  execution cycle. Core manual 6.2.2 note 4 applies that cycle to memory
  addressing; immediate `ld.w` remains one execution cycle (page 123).
  Operand shapes now distinguish them. Four regressions fail on the old
  core and pass now; the SPI lookup loop moves from about 61 to 57 cycles/pass.
- A0 data accesses are cheaper with SDRAM code. Subtracting the move-loop
  control measures 1.5 cycles/halfword load, 2/word load and 1/store. The
  older model charged 2/2.5/1.5 for both code placements. One half-MCLK is
  now hidden for SDRAM code (`a0_sdram_overlap=1`; zero restores the old
  behavior). Internal-code costs retain their separate calibration. The
  new model matches those access slopes; a common cycle/pass remains in
  the SDRAM loop control/fetch cost.
- CMU clock/gate writes settle timers and watchdog at the actual write
  boundary before changing the configuration. Listener wiring survives
  reset. Watchdog MMIO settles elapsed time first, so reads are current,
  starting excludes stopped time, and a late kick/disable/period extension
  cannot erase an already due timeout. Nine regressions fail on the old
  watchdog; timer tests cover fractional clocks, unpolled changes and reset.

`make check test-mem-dma test-sd-dma-driver` passes, including GUI launcher
boot, watchdog reset and all 122 SD DMA cases. The optional binutils capture
comparison skips without `ref-*.txt`. Reports and executable snapshots are
in `build/wr128/pipeline-review/`; the paired memory refresh-phase sweep is
in `build/wr128/pipeline/phase-sweep/`. Record all metrics and their tradeoffs;
do not refit instruction costs to cancel one short copy's refresh phase.

## Remaining work

- Prefix frontend: eight single-prefix AND pairs from A0 take 14.029
  cycles/pass against a move control of 13.029; double-prefix pairs take
  21.029 or 22.029 depending on halfword alignment. Freeing every prefix
  loses restart, throughput and shared fetch/data effects. Use the recorded
  ALU/load/store/branch and mixed probes at all eight alignments.
- Load/use effects: immediate consumption and one/two independent moves
  have different alignment patterns. Core manual 6.2.2 explicitly says
  there is no interlock cycle. Do not infer a generic core result delay
  from bus-dependent measurements.
- SPI DMA contention, especially SDRAM code beside a stream, still disagrees.
- Verify the kernel card-read phases (CRC alone, check, poll and total)
  after material timing changes, always through the real launcher.

A free-prefix/fetch-ahead plus one-cycle load-result-delay experiment was
retested against fresh hardware: RISC-V RMS 0.07094 -> 0.05878 and pipeline
RMS 0.24149 -> 0.14364, but SPI solo 2.57492 -> 3.23398 cycles/pass and
memory RMS 0.07050 -> 0.07224. It remains rejected. The fresh data corrects
the earlier reason for rejecting it; cross-workload and architectural
objections remain.

## Matched bus probe and DMA follow-up

`tools/bus_bench` builds 44 sequences at three code placements and two data
placements (264 cases). Eleven dependent/independent pairs change only the
consumer register while retaining code size and instruction count. XOR,
compare, move, extended ALU, overwrites and adjacent loads distinguish load
dependencies from instruction-fetch/data sharing. The runner passed all
264 cases through FLASH -> Grifo -> launcher. The collector automatically
finds the returned card, verifies installed identities and all cases, then
preserves reports under content hashes. `analyze.py` checks matching clocks,
placements and build/boot inputs and reports 66 paired contrasts plus all
264 timing errors, keeping the three raw sample ranges visible.

The validated application (`bd02227836939a5d9b4a6dcf2c111f0a5786a0aa799c11f7324b432005b4957a`)
was installed on WRBOOT24750 as launcher entry 9, bottom right. Existing
entries were preserved and `init.ini` backed up under
`build/wr128/buspipe/card-backup-20261105-015733`. All installed files were
hashed and flushed. Normal unmount was vetoed by loginwindow; after checking
both filesystems had no open files, a flushed forced unmount succeeded.
The card disconnected during remount, so post-remount verification and the
final explicit eject did not complete. Do not describe that final check as
passed. Installation identities are in `build/wr128/buspipe/installed.json`;
the returned hardware files subsequently passed every identity check.
The hardware results and reader installation are described below.

A separate DMA experiment changes synchronous read service from `wait + 1`
to `max(wait, 1)`, testing overlap of the base read phase. It is **not in the
default emulator**. Source, binary and FLASH/Grifo-run reports are preserved
in `build/wr128/dma-review/read-service` (emulator SHA-256
`701fb30a053f540faf5425ee432d6f844e09eb0bddf7d542d5e693962fd6b880`).
Memory log RMS improves 0.07023 -> 0.05803 and cases within 10% rise
105/120 -> 111/120. Large same-/different-bank DMA32 errors improve from
5.75%/11.08% to 0.03%/0.29%; SPI metrics are unchanged. But 5-KiB IVRAM DMA
gets worse, -9.67% -> -19.97%, and narrow DMA still misses substantially.
Reject the blanket change. Investigate controller/bus phase overlap and
destination-path arbitration instead of applying a destination exception
solely to improve the score.

The existing ZIM startup and article benchmark instrumentation already
measures several workflow phases (`zim_startup.c`, `zim_page.c`,
`zim_bench.c`). Reuse it with matching binaries/content for qualification;
do not invent a second timing path or treat historical timings as evidence
for the current build. Search, scrolling and suspend/wake still need matching
repeatable end-to-end measurements.

## SDRAM refresh and reset corrections

The subsequent review found two established state/timing bugs independent
of the pending hardware probe. `service_refresh()` examined four row slots
even though the current geometry has eight physical banks and row-port
bookkeeping puts DMA writes in slot 4. It also omitted write recovery before
the refresh precharge. Refresh now observes every physical bank's tRAS and
write recovery (or every legacy row slot when `bank_floors=0`).

Read turnaround used a file-static `last_was_write`, shared between controller
instances and surviving reset. It used `bus_free` as the write timestamp, so
an intervening refresh could charge turnaround a second time. It now uses
the controller's own `last_write_end`, cleared with reset/reconfiguration.
The old fetch-path differential test manually swapped that global between
the two controllers; that workaround is removed, and the comparison now
also sweeps turnaround and write recovery.

Eight added assertions fail against the original SDRAM source and pass with
the fix: upper-bank tRAS/write recovery under both row-bookkeeping modes,
refresh absorbing elapsed turnaround, reset history and cross-controller
isolation. The complete functional suite, memory DMA and all 122 Grifo SD
DMA cases pass. Source/binary identities, original failures and functional
logs are in `build/wr128/sdram-review/verification.json`. A new full five-suite
calibration is complete in `build/wr128/sdram-review/qualification`: all
738 cases pass and every aggregate score is exactly unchanged from the
previous baseline. The binary SHA-256 is
`551a4b3ec3d966931fa2e5d34984c16e51f6b9cba7ad81b22aa5f63d7bf24aa0`;
`build/wr128/sdram-review/accuracy.json` preserves the per-case errors.
This corrects boundary/reset behavior; it does not resolve the existing
frontend/contention errors or meet the broader milestone.

The timing-only frontend hypothesis is now reviewable at
`tools/experiments/frontend.py`, with limitations and results in the adjacent
`FRONTEND.md`. It checks all 160 A0-code/A0-data shapes/alignments; steady
predictions for the 40 offset-0/2 cases converge at 160 and 320 iterations.
The register/prefix/branch controls agree closely, but freezing decode during
memory still worsens plain loads. Neither experiment is the production
frontend or evidence that its qualification requirements have been met.

## Reader measurement probe

`tools/workflow_bench` builds a separate reader app and clones the existing
local `build/wr-card.dmg`, which has matching Grifo/launcher and the Simple
English/Wikivoyage archives. `zim_bench.c` gains an optional
`bench LABEL calibrate` mode; ordinary reader outputs were preserved during
the isolated build. Calibration saves clock context, raw phase ticks,
lookup/retrieval I/O counts, native article identities/sizes and composited
240x208 display checksums. It exercises active-gesture rendering with four
actual 104-pixel viewport steps and restores the original archive selection
before powering off.

The final app is
`44362cfd278820af4ab07be1c2d0c7c64d18b0901722074333f870a75ab8668a`.
The full FLASH/Grifo/launcher run at
`build/wr128/workflow-calibration/ready-run` verifies 27 phase vectors with
three samples each, nine article loads and 36 viewport movements. Checksums
match across repetitions, and the startup report is healthy. Initial and
later passes differ in filesystem I/O; do not pool them without retaining
their work counts. The report comparison rejects changed clock configuration,
native output or per-pass I/O workload before scoring. There are now 26
passing tests in `make test-score-model` across the calibration validators.

This is measurement infrastructure, not hardware timing qualification.
Typing/debounce, coasting, repeated cold boots and suspend/wake remain open.
The probe has now been installed after collecting the bus evidence. Its
physical card layout was not captured; file identities and reader resources
were saved. Do not describe an emulator fixture as an exact physical replay.
`tools/workflow_bench/README.md` describes the artifacts and limits. The
single-entry development fixture must not be presented as a matching
physical-card comparison.

## CPU data-access event ordering

The current emulator settles due SPI/DMA events before each CPU data access,
using `c33_bus.before_data`. An instruction's fetch wait can cross a receive
completion after the outer loop polled. Previously the later load still read
old RAM; a later CPU store could be overwritten when the outer loop finally
applied the earlier DMA event. Data-region caches now also follow mapping
changes made by the hook. Interrupts are still accepted at instruction
boundaries. This is an access-order correction, not a complete bus scheduler.

`test_cpu_dma_order.c` drives real SPI/HSDMA devices while isolated C33 loads
and stores wait for fetch. Nine checks fail with the original core and pass
with the fix, covering generic/specialized fetch timing, direct/uncached data
and a mapping change. Full `make check test-mem-dma test-sd-dma-driver` passes.
All five calibration suites pass all 738 cases. That review's binary SHA-256 is
`95a3ce6792515254b6ce2bea2730a7e78ffaee45b03d5dcae5ec82e63d31a109`.

`build/wr128/event-order-review/verification.json` records source identities,
original failures, full functional logs and before/after scores. SPI contention
loss RMS improves 6.45012 -> 6.13907 cycles/pass (4.82%); transfer log RMS
0.027123 -> 0.026993, solo RMS is essentially unchanged (2.030846 -> 2.030892).
NuttX log RMS improves 0.075744 -> 0.075238. Memory, RISC-V and pipeline scores
are exactly unchanged. Preserve the tiny per-case regressions as well as the
aggregate improvement; the largest contention regression is 0.07 cycles/pass.

The same reader fixture was replayed at
`build/wr128/event-order-review/workflow`. All 27 phase vectors retain the
same native pixels and per-pass I/O counts. Screen medians change by roughly
-0.02%; this is a comparison between emulator versions, not hardware accuracy.

Large SDRAM-fetch contention misses remain. The controller currently schedules
an entire instruction-queue line ahead of time. The manual fixes SDRAM reads
at two halfwords per burst (II.4.1.5), while an IQB slot holds eight halfwords
(II.4.2.2); DMA has higher bus priority (II.4.3.1). A future scheduler needs to
represent those requests and peripheral deadlines on a common timeline,
rather than merely tuning a flat CPU or DMA hold. Both physical bus and reader reports are now available; see the findings below.

## IDMA source-read deadline correction

An asynchronous IDMA write was scheduled at `start + idma_extra` even when
the descriptor/source reads took longer. Its destination could change, or
SPI could start transmitting, before the required read phases completed.
The write is now floored at source-read completion plus its own write bus
phase, as required by the B1--B4, C, D sequence in manual II.2.4.1. The normal
calibrated allowance remains unchanged when it already covers those phases.

`test_idma_deadline.c` checks fast/slow descriptors and source memory, zero
and fitted allowances, RAM destinations and actual SPI transmission. Six
cases fail with the original DMA source; all 21 checks pass with the fix.
Full functional checks and all 122 Grifo SD DMA cases pass. All five timing
suites pass all 738 cases with exactly the previous scores, and all 27 reader
workflow sample vectors, pixels and per-pass I/O counts are exactly unchanged.

Current evidence, original failures, source identities, full logs and binary
snapshots are in `build/wr128/idma-deadline-review`. The remaining accuracy
gaps and missing physical workflow qualification have not been waived.

## Returned bus probe and installed reader probe

`tools/bus_bench/device-2026-11-05-894a7d225e66.txt` is the verified physical
264-case report (all native results and guards pass). All five installed
inputs matched. `build/wr128/buspipe/current-baseline` replays the same app
with the current production emulator through FLASH/Grifo/launcher;
`current-comparison.json` retains all timing errors and 66 contrasts.
The current model has median absolute error 7.17%, p90 40.86%, maximum
64.17%, and 176/264 cases within 10%. This is additional evidence, not a
replacement for or improvement to the five-suite qualification.

64/66 dependent/independent contrasts are below 0.0001 MCLK per pair in
magnitude, across XOR, compare, move and prefixed consumers, both load widths
and both data placements. The two exceptions are SDRAM `word_use2` versus
`w_indep2`: about 0.9553 MCLK/pair on hardware, 0.3344 in the model. The ELF
map/disassembly shows the control loop at 0x10040760--0x100407a2, within one
1-KiB row; the dependent loop at 0x100407c0--0x10040802 crosses a row at
0x10040800. Their A0 copies have zero contrast. A code-placement effect is
the supported explanation; the precise extra row/fetch cost still needs
isolation. Do not add a dependency penalty to account for these two cases.

The reader app, icon, list and logging marker were installed as launcher
entry 9, preserving the other eight entries. Existing files and bus metadata
were backed up under `build/wr128/workflow-calibration/card-backup-20261105-111059`.
Stale bus metadata was removed only after collection. `installed.json` records
application/boot/list/launcher and archive hashes. All seven installed inputs
and the on-card metadata verified after a normal remount; `diskutil eject`
succeeded. No background capture or emulator job remains running.

A raw card capture was attempted for identical filesystem allocation replay.
macOS rejected raw reads, including administrator dd and its image service.
The user asked to stop the sudo escalation. Stop these attempts; do not ask
for a Terminal command, more sudo, privacy changes or a raw image. No image
was produced and `capture.py` was removed. Reader resources and relevant
starting files are saved in `hardware-start-files`; immutable input/content
identities remain verified. The collector accepts this file-verified run and
explicitly records `physical_layout_verified=false`. Any comparison requiring
identical physical allocation remains unqualified. The bottom-right reader
entry runs automatically, saves its results and powers off; collect on return.

## Returned reader measurement

`tools/workflow_bench/device-2026-11-05-ed9028c8f18e.txt` and its `-boot.txt`
are the physical article/viewport and startup reports. The collector verified
all installed inputs and both archive hashes. Seven bitmap fonts and the
language resource also match the saved starting files and the emulator image.
All nine article loads and 36 viewport movements match the production emulator's
native output pixels, identities and per-pass I/O counts. Startup has no
read errors, timeouts or DMA fallback. The card was safely ejected after reads.

`tools/workflow_bench/analyze.py` verifies build/report/resource hashes and
executed input files before comparing the current production run at
`build/wr128/idma-deadline-review/workflow`. Its output is
`build/wr128/workflow-calibration/hardware-comparison.json`. Across 27 phase
medians, 21 are within 10%, none within 5%; median absolute error is 7.89%.
Finished article screens take 6.08--7.89% longer in the model; downward viewport
steps take 9.18--9.95% longer. Return steps take 14.28--14.73% less time, about
0.44 ms versus hardware's 0.51--0.52 ms. Keep this small absolute scale visible.
App-to-keyboard is 165.298 ms versus 156.227 ms (+5.81%).

Retrieval time outside card reads is 6.58--6.86% too long across all three
articles. Time inside reads differs by only +0.46--3.48%. This points toward
CPU/fetch execution as the next investigation; it does not establish a global
cycle scale or justify a fitted multiplier. Backward scrolling reuses rows
already rendered; downward steps also generate new rows. Both paths report
actual viewport movement and matching composited output.

This remains a diagnostic phase comparison: physical filesystem allocation
is unmatched. Kernel boot totals are excluded because menu contents and
file-read work differ (even with matching reader/boot executables). There is
one measured cold boot, not three. Input-driven search, coasting and suspend/
wake remain open; the overall milestone is incomplete. The default five-suite
accuracy bundle still describes only its own runs; the new workflow evidence
is separate. No emulator core/default model was changed in this collection.

The FAT fixture reader now traverses directories to verify `zim/wiki.nls`;
its regression covers nested files, directory/file confusion and traversal
rejection as well as previous allocation checks. All 26 report-validation
checks and the FAT fixture regression pass. The `work*` ignore rule previously
hid the entire workflow harness; a narrow directory exception now exposes
its sources and archived hardware evidence to git.

## Frontend work resumed after reader collection

The user resumed the accuracy goal and `get_goal` now confirms `active`.
The preceding goal turn made progress: it corrected experiment scheduling and
extended the verified hardware controls. No additional card work is needed
for the current investigation. Keep the overall scope in ACCURACY.md; no
external blocker has been established in the resumed run.

The offline frontend experiment now extracts all 44 bus instruction shapes
from the frozen source, verifies source/report hashes and checks both A0 code
alignments with both data placements. Consumer dependencies are not modeled;
register-only changes naturally produce identical timing shapes. Separate
SDRAM data uses explicit two-cycle read/four-cycle posted-write hypotheses,
without pretending to model SDRAM queues, refresh or arbitration.

Review found an event-order bug in the experiment: E-stage dispatch created
a data request after the request-service phase, inserting an artificial
quarter-cycle per transfer even on an idle separate port. Request service now
runs after dispatch. Two regression tests pass, including the exact independent-
port formula across three read latencies and both alignments, and convergence
when doubling simulation iterations.

`build/wr128/frontend-lab/returned-controls-queue.json` preserves the old
ordering; `returned-controls-dispatch.json` has the correction. Separate-port
bus cases improve from 82/88 to 86/88 within 10%, with corrected median error
0.10%, p90 1.41%, maximum 15.14%. The two large remaining cases are `mixed`
at offsets 0/2, predicted 53/54 cycles against hardware 46.029/47.029. Shared-
A0 controls still have only 38/88 within 10%, median 16.28%, p90 27.08%,
maximum 31.16%. Original 160 pipeline A0 cases have 88/160 within 10%, median
5.86%, p90 27.08%, maximum 31.16%. The worse shared-port score exposes that
the old artificial delay partly concealed the missing mechanism. Do not keep
that delay for fit. Next investigate shared A0 port/backpressure and mixed
prefix lookahead with these separate controls. Production core/model and its
qualification scores are unchanged; this is experimental progress only.

## Prefix lookahead and whole-reader profiling

The optional offline `--trailing-ext` hypothesis allows a queued EXT to be
predecoded alongside a preceding unextended register operation, using the
extension latch for the next operation. It leaves an extended preceding
operation alone. This explains the two remaining separate-port `mixed`
misses without a constant subtraction or dependency penalty: 53/54 becomes
46/47 against hardware 46.029/47.029. All 88 separate-port controls now have
maximum error 3.43%, median 0.10%, p90 0.23%. Shared-A0 errors remain unchanged.
Artifacts are `build/wr128/frontend-lab/trailing-prefix.json` and the adjacent
`trailing-prefix-traces.json`. Three experiment regressions pass, including
base-instruction address/extension-count conservation across all original
shapes, alignments and port arrangements, and convergence on doubled runs.
This is still a hypothesis; no production prefix rule has been changed.

The real reader was profiled with the existing cbe66835 production binary,
through FLASH/Grifo/launcher, at `build/wr128/frontend-lab/reader-profile`.
`workflow_bench/run.py --profile` records the command and snapshots normally.
Every raw article and startup report byte is identical to the original run.
All 26 report-validator checks, FAT fixture regression and three frontend
checks pass. No executing CPU/model source has changed in this resumed turn.

The full PC profile has two-byte resolution, not the stale “1K” heading in
its console summary. Whole-run cycle shares are A0 15.80%, IVRAM overlay
window 25.74%, reader SDRAM 21.08%, kernel/other 37.37%. Untimed serial output
alone occupies about 21% of total cycles; do not confuse this with timed
article work. `profile-summary.json` preserves partial object attribution,
map/profile hashes and these limitations. Multiple overlays share
0x81a00--0x83000, so address-only samples cannot name one resident function.
The actual hardware reader executes these overlays successfully; do not add
an area-3 execution trap merely from the E07 manual's restriction note.

A0 branch timing also has an unresolved scalar compensation: production's
pure-register bus loop is 15.031 cycles/pass against hardware 13.029, while
its local halfword-load loop matches about 23.031 vs 23.029 because the larger
branch allowance cancels missing shared-port overhead. The frontend model
must remove those compensations together. As a diagnostic, full five-suite
and reader runs with ONLY `branch_taken_iram=4` are in progress under
`build/wr128/frontend-lab/branch-four`. This is a parameter experiment, not
a default change. Score tool session 51010 and workflow session 77572 were
returned live; poll these exact sessions and inspect completion markers.
Do not start duplicates because an observation expires. Compare every suite
and reader native/I/O workload before drawing conclusions or changing defaults.

The branch-four reader run has finished and passed native/I/O comparison in
`branch-four/workflow-comparison.json`: finished screens become +6.17%,
+6.60%, +4.51% versus hardware (previous +7.47%, +7.89%, +6.08%). Viewport
steps are unchanged. The standalone branch reduction is rejected: completed
SPI solo RMS worsens 2.03089 -> 2.74524, transfer log RMS 0.026993 -> 0.028579,
and memory log RMS 0.070235 -> 0.072252; contention loss RMS improves slightly
6.13907 -> 5.98199. Full scoring session 51010 is still live in NuttX ubench;
workflow session 77572 is complete. `build/wr128/frontend-lab/continuation.json`
records this state. Finish the same score job and preserve all suite tradeoffs;
do not promote a branch-only reduction or restart the job on an observation
expiry. Default emulator hash and production timing remain unchanged.

## Executing CPU address-phase and fetch-accounting corrections

The preceding resumed turn made progress (prefix hypothesis, real-reader
profiling, completed branch experiment). The branch-four five-suite run is
now complete: all 738 native cases pass, but the standalone change remains
rejected. SPI solo/transfer and memory regress as recorded above; NuttX RMS
also worsens 0.075238 -> 0.078710. RISC-V improves 0.068920 -> 0.066126 and
pipeline 0.233469 -> 0.178365. `frontend-lab/branch-four/accuracy.json` retains
all errors; do not hide the losing suites behind reader/pipeline improvements.

The completed address-phase/fetch-accounting baseline is
`6d17536e23f87482869963ecc3d3b6d98e7109ea87515e29834b02a42ddde93d`.
All five timing suites are complete. Model defaults have not been retuned.

`extended_address_phase()` moves the existing extra execution cycle for an
extended register-indirect/DP memory address ahead of its bus transaction.
It also covers extended memory bit operations, whose extra cycle formed the
same address. Previously that cycle was charged after rd/wr. A DMA write
becoming due during address calculation could therefore be invisible to a
load, or overwrite a later CPU store. Total no-wait instruction clocks remain
unchanged. This does not claim a complete instruction/bus phase model.

`test_cpu_dma_order.c` now exercises real SPI/HSDMA completion at MCLK 34,
with the base instruction starting at 33, across load/store widths, memory
bit operations, zero/one/two prefixes, direct/uncached data and generic/
specialized fetch waits. The uncached-data variant deliberately keeps code
fetch mapped, isolating address timing from the separate fetch bug below.
The original source fails 184 assertions; all 445 checks pass with the fix,
including unprefixed controls and unchanged instruction-clock totals.

A separate bug used rd() for an uncached instruction fetch even though the
fetch wait had already been charged. That added a CPU data wait, data-event
callback and data touch. Fallback instruction reads now call bus.read directly.
`test_fetch_accounting.c` compares an absent mapping, a rejected mapping and
a direct mapping, with generic/specialized fetch timing and NOP/load bodies.
All 84 assertions pass; the old path fails 32. The target is included in check.

Both fixes pass full `make check test-mem-dma test-sd-dma-driver`, including
launcher GUI, watchdog resets, memory DMA and all 122 SD DMA cases. Full logs,
source/binary identities and original failures are under
`build/wr128/address-phase-review/verification.json`. These are causal fixes,
not fitting parameters. The final candidate also passes all 264 bus cases
and all 27 reader phase vectors with matching hardware pixels and per-pass
I/O counts. The reader comparison remains limited by unmatched physical
allocation and incomplete input/boot/suspend coverage.

Intermediate address-only binary d04b97e7 and the final binary have byte-
identical SPI, memory, NuttX, pipeline, bus, reader and startup reports.
The final reader screens differ from hardware by
+7.37%, +7.77%, +6.03%; forward viewport steps by +8.86--9.57%; return steps
by about -14.4--14.8%. Improvement is small; the frontend/contention gaps remain.

Both complete calibration jobs (8607 and 80327) and reader/bus jobs are
terminal. The final combined scores are SPI solo RMS 2.030040 cycles/pass,
contention RMS 6.141073, transfer log RMS 0.026892; memory 0.070567,
NuttX 0.074854, RISC-V 0.068777 and pipeline 0.233520. Changes are small
and mixed. Memory case `82:same:dma16:4096` regresses from +9.904% to
+10.004%; the cause of that small timing shift is not separately isolated.
The complete milestone in ACCURACY.md remains unmet.

## Shared-port experiment and variable-unroll probe

The offline frontend now separates directional port turnaround from result
latency and constrains CPU stages to clock edges. It also fixes a numerical
queue-refill delay: decoding can free FIFO space for fetch at the same time,
without an artificial simulation tick. Quarter/half-clock resolutions now
produce identical event traces when all physical delays are divisible by two.
An optional prefix/data resource-sharing hypothesis plus trailing-prefix
lookahead, half-clock port turns and full-clock stage starts gives 140/160
original shared-A0 cases and 78/88 additional shared-A0 controls within 10%.
All 88 separate-port controls remain within 3.43%. Neither shared set reaches
90%, and this is not an executing CPU or qualified timing change. See
`tools/experiments/FRONTEND.md` and `frontend-lab/port-turnaround/prefix-resource.json`.

`tools/port_bench` builds 560 cases spanning 20 instruction shapes, seven
unroll counts (1/2/3/4/8/12/16), two A0 code alignments and two data placements.
Varying loop length distinguishes per-copy service from restart overhead.
Actual consumer checksums and memory guards are checked in every repetition.
The frozen pipeline/bus applications are preserved. The new probe exposed
the host's invalid nine-NOP runaway heuristic, removed in c4cc7459 above.
Original failure and corrected real FLASH/Grifo/launcher run are retained in
`build/wr128/portq/validation` and `validation-nop-fixed`; all 560 corrected
cases and full functional/122 SD DMA checks pass. Report/comparison tests
also cover stale workloads, contexts and preservation of failed hardware logs.

The app is staged, not installed or hardware-measured yet. No card is mounted;
no raw-device/admin operations are pending. Continue frontend and SDRAM burst
arbitration work independently of a future card return. Any eventual install
uses ordinary file copies, preserves the launcher menu and backs up replaced
files. Do not repeat raw-card capture attempts.

## DMA destination acceptance and CPU data-wait boundaries

The SDRAM arbitration review found concrete event-order faults before the
larger burst scheduler rewrite. An asynchronous DMA destination used to
charge mem_wait but change memory and raise its HSDMA flag at the request
time. The payload now remains queued until the interface acceptance time,
with destination timing charged once. A waiting AHB phase holds the DMA
engine, preserving later SPI triggers until it ends. CPU reads/stores settle
asynchronous devices again after a nonzero data wait, before the memory
effect; otherwise a read can wait beyond completion yet see stale RAM.
Posted CPU writes also respect existing DMA ownership before buffer handoff.
Interrupt acceptance remains at instruction boundaries; scalar defaults are
unchanged. These are not a complete common-timeline/burst scheduler.

`test_dma_write_deadline.c` has 311 assertions across byte/halfword/word,
IDMA/HSDMA, zero/short/long destination waits, early/late polling, real SDRAMC
buffer acceptance, cached/uncached CPU accesses, and back-to-back SPI triggers.
All pass. Isolating the old DMA implementation fails 83 of the earlier 295
assertions, the old CPU data boundary fails 16, and the old posted handoff
fails its independent controller test. Omitting the active-engine hold fails
three assertions in the final 311-case regression. Original failures,
source snapshots, executable identities and logs are under
`build/wr128/dma-write-review`.

Do not assume DMA cannot use the posted-write buffer. That experiment
(`qualification`, binary 889b1eca) completed all native suites but worsened
memory log RMS from 0.070567 to 0.126618 and was rejected. The current version
retains buffering and defines completion at interface acceptance, not final
external SDRAM drain. The current memory score returns to 0.070567.

Full functional/122 SD DMA checks pass on current bc75d10a. The additional
264-case bus report and all 27 reader phase vectors pass with matching native
pixels and per-pass I/O work. Reader errors remain roughly +6--8% for screens,
+9% for forward steps and -14--15% for short returns. Current SPI contention
RMS is 6.367898 versus baseline 6.141073; transfer log RMS 0.028207 versus
0.026892: preserved regressions, not evidence of accuracy improvement.

All runs are terminal: current full assessment 85927 (`engine-qualification`),
earlier buffer-preserving comparison 48243 (`final-qualification`), current
variable-unroll replay 48912 (`engine-portq`), and rejected nonposted experiment
61810. All 738 native cases pass for current bc75d10a. The comparison command
exits 1 because SPI contention/transfer and NuttX RMS regress, not because
guest validation failed. Final log RMS: memory 0.070567, NuttX 0.075158,
RISC-V 0.068777, pipeline 0.233520. All guest/device identities match the
preceding 6d17536e baseline. `dma-write-review/accuracy.json` records raw
per-case errors; within-10% counts stay 127/128 SPI transfer, 118/128 SPI solo,
104/120 memory, 74/90 NuttX, 39/40 RISC-V and 169/360 pipeline. SPI transfer
p90 grows from 4.17% to 5.07%. Do not retune scalar costs to conceal this.

The user reconnected the card and explicitly requested cleanup of old tests.
`tools/port_bench/install.py` verified its UUID, archived reader evidence,
native-validated app/source, boot inputs and copied files before selecting
portq in the menu. Then 29 obsolete SPI/pipeline/bus/RISC-V/reader-probe files
were copied to `build/wr128/portq/obsolete-probes-20261105-131912-662560ee`
and removed from the card. Five ordinary apps (reader, Doom, Game Boy, NuttX,
Linux), icons, boot files and archives remain. The menu now has six entries:
portq is the rightmost icon on the second row. All five probe input hashes,
metadata, cleanup and normal-app hashes were verified after remount. The card
is safely ejected. `build/wr128/portq/installed.json` preserves installation,
backup, cleanup and verification identities. No raw-device/admin commands
were used; normal filesystem copies and diskutil mount/eject required host
sandbox escalation, which was authorized.

The user has been instructed to run that icon; it powers off automatically,
then they return the card. Find it and collect `portq.log` autonomously;
never ask for a log path. The collector preserves failed native reports too.
No other hardware action is pending. Frontend/shared-port and SDRAM burst
arbitration remain the next mechanism work, independent of the card return.

## Frozen port-probe predictions and queue capacity review

The resumed turn made progress: it completed native prospective mapping and
numerical-resolution checks for all 560 new cases, froze six timing hypotheses
before collecting hardware data, and compared raw FIFO capacity against the
existing physical probes. No executing CPU/default code changed.

`tools/experiments/port_predictions.py` verifies the generated source/native
replay, parses actual unroll values and freezes predictions with exact source
snapshots. `frontend-lab/port-prospective.json` is the initial five-hypothesis
snapshot; `port-prospective-queues.json` adds eight raw halfwords. Never replace
these after hardware collection. Response latency versus data-to-fetch turn
remains unidentifiable: the alternate split predicts identical times in all
560 cases. Queue depth changes 61 shared cases by at most two MCLK/pass.

The 2/3/4/6/8-halfword capacity review is under
`build/wr128/frontend-lab/queue-capacity-review`. Eight predicts 152/160 original
shared cases and 83/88 additional shared controls within 10%; all 88 separate
controls remain within 3.43%. These are laboratory predictions, not executing
firmware accuracy. The manual's four instruction queues do not state their
storage width/allocation; four 32-bit raw entries are only one hypothesis.
Both existing data sets informed this choice, so the new physical variable-
unroll report is needed to challenge it rather than calling it a holdout win.
Ten laboratory tests pass, including template coverage/size/unroll rejection.
The production emulator remains bc75d10a, with the previously completed scores.

## Native frontend scheduling foundation (preceding stage)

The next resumed turn implemented `src/frontend.[ch]`, a C streaming fetch/
decode/port scheduler corresponding to the fixed laboratory hypotheses.
At that stage it was not connected to c33.c; bc75d10a and its accuracy scores
were unchanged. The integration described below supersedes that stage.

`make test-frontend-native` passes five test groups, including 778 exact
Python/C trace comparisons, with instruction-word order, all E-issue times,
prefix counts and steady-loop clocks checked. Native lifecycle checks cover
cold reset, redirects, code/mapping flush, old prefetched word snapshots,
rejected regions, multi-phase E execution and long external waits. Flushing
speculation preserves a RAM phase already in flight. Full evidence, source
and driver snapshots are in
`build/wr128/frontend-lab/native-frontend/verification.json`.

The API returns EXT semantic events without inventing an E operation;
`frontend_complete` explicitly resolves each non-EXT E stage. Data reservations
do not independently retire E, allowing multiple rd/wr phases. `frontend_advance`
continues fetch/decode during a wait; `frontend_flush` drops speculative state
while preserving port occupancy. The future C33 integration must retain ISA/
prefix/interrupt/exception/reset/coherence behavior and exclude duplicate
legacy timing. See `tools/experiments/FRONTEND.md` for constraints. The C test
driver uses declarative categories, executes no guest instructions, and is
not firmware qualification. The physical port report remains outstanding;
collect it automatically when the card returns, then test the frozen six
hypotheses before default decisions. No process remains live.

## Executing CPU frontend integration

The scheduler now drives real C33 instructions for mapped area-0 code
(including its IVRAM alias), with legacy timing retained outside that region.
Default `local_frontend=0` preserves the established timing path. Experimental
`local_frontend=1,itc_wait=4` uses eight raw halfwords, one completed decode
slot, whole-clock stage starts and quarter-clock local port accounting.
The queue capacity and shared prefix resource remain hypotheses awaiting the
prospective variable-unroll hardware report.

Data phases use actual rd/wr waits and settle asynchronous devices before the
access, including after waits. The old local-data surcharge, flat prefix cost
and taken-branch approximation are excluded when the scheduler owns timing.
Prefixes still retire separately; IRQ deferral, third-prefix faults and delay
slots retain their architectural behavior. Reset, exceptions, code stores,
host patches and region epochs invalidate speculative words appropriately.
Mapped prefetch never performs speculative MMIO. A copied CPU state rebinds
the scheduler callback to its own instance before execution.

The joint ITC override follows the independent polling control: with manual
branch execution and modeled F restart the loop is 9 + itc_wait clocks;
hardware is 13.01. It is not a new default or a CRC-specific adjustment.

All 738 calibration cases, 264 additional bus cases, 560 emulator port cases
and reader pixel/I/O vectors pass. Default full functional checks and the
experimental 122-case SD DMA/GUI boot/49-reset checks pass. Isolated tests
include 3,850 paired architectural states, alias/mapping/copy checks, real
SPI/DMA completion during frontend data waits, and 778 scheduler trace
comparisons. Firmware evidence always uses FLASH -> Grifo -> init -> app.

Cases within 10%, compared with the default baseline:

| Suite | Default | Experimental |
| --- | ---: | ---: |
| SPI transfer | 127/128 | 127/128 |
| SPI solo | 118/128 | 120/128 |
| Memory | 104/120 | 104/120 |
| NuttX | 74/90 | 74/90 |
| RISC-V | 39/40 | 39/40 |
| Pipeline stress | 169/360 | 329/360 |
| Additional bus | 176/264 | 243/264 |

Pipeline median error falls from 13.32% to 0.01%, RISC-V from 7.44% to 4.30%.
SPI solo's maximum falls from 36.00% to 12.19%, but absolute-cycle RMS grows
2.03004 -> 2.33210. Contention RMS improves 6.36790 -> 5.98203; transfer log
RMS improves 0.028207 -> 0.027421. Memory log RMS grows 0.070567 -> 0.072271
and median relative error grows 4.46% -> 5.64%. Keep this opt-in pending the
physical probe and remaining store/controller analysis.

Reader screens are +3.34--5.50% versus +6.03--7.77% before. Eleven of 27
phase medians are now within 5% (previously zero); 21 remain within 10%.
Forward steps remain about +9% and short returns about -15%. This remains
diagnostic with unmatched physical allocation and incomplete input/boot/wake
coverage. Seven five-suite cases still exceed 20% without a proven cause;
memory and NuttX remain below the representative threshold. SDRAM instruction
queue arbitration and posted-store timing remain practical next targets.

All calibration jobs have finished. Current evidence and exact source/binary
snapshots: `build/wr128/frontend-integration/verification.json`.
The physical card remains ejected for the portq run; no new report is mounted.

## Conditional restart and posted-write sensitivity

The following turn tested three complete 608-case SPI/memory/pipeline trials
in separate binaries. All native cases pass, but none warrants a default
change. `build/wr128/conditional-restart/three-suite-assessment.json` preserves
every relative error; individual runs retain inputs, device hashes and binaries.

Making every taken conditional restart the old SDRAM fetch path reduces the
extended-branch error but changes the pure SDRAM move control from 14.03 to
18.03 clocks/pass against 13.03 on hardware. Memory drops from 104/120 to
89/120 within 10%. Pairing restart with manual three-clock branch execution
still gives a 16.03-clock control and 91/120 memory passes. Do not apply the
simple flag change: the legacy taken cost already mixes execution/fetch costs,
and the fetch floor is applied after E rather than overlapped with it.
The local scheduler handles this structurally; extending that treatment to
SDRAM needs timed fetch requests and a common CPU/DMA timeline.

Raising the shared store handoff from four to six half-clocks improves pipeline
to 345/360 within 10% and SPI solo to 128/128, but worsens memory to 98/120,
maximum 51.64%. Holding DMA acceptance at the existing four half-clocks while
varying CPU handoff retains the pipeline/solo gain and recovers memory to
101/120, maximum 36.41%. Memory log RMS is 0.072231 versus 0.072271 with the
first frontend, despite three additional threshold misses. SPI transfer stays
127/128 but log RMS grows 0.027421 -> 0.028842. Reader screens become
+4.40--6.43% versus +3.34--5.50%; forward steps approach +10%. Preserve these
tradeoffs; a single best aggregate is not a promotion criterion.

The working code now exposes `dma_write_issue` (zero inherits `write_issue`),
preserving all prior defaults/overrides. It separates experimental acceptance
overheads without changing external write reservations. A 16-write buffered
DMA stream is invariant when only CPU handoff changes; the negative control
with the old coupling fails. Current working binary:
`d0a57a5b5236198f42812b6a72d42ad7dd03ceccd5526636076d29a0096017c2`.
Default full functional/122-case SD DMA and focused controller checks pass.
The experimental settings also pass all 122 SD DMA cases, GUI boot and 49
watchdog resets.

Both the separate causal and integrated five-suite runs have now completed;
all 738 native cases pass and their scores agree exactly. Integrated counts
are SPI transfer 127/128, SPI solo 128/128, memory 101/120, NuttX 75/90,
RISC-V 39/40, pipeline 345/360. The CPU handoff trial is not promoted.
`build/wr128/conditional-restart/verification.json` closes that assessment.

## Width-aware external writes and current work

The working SDRAMC now separates fixed controller overhead from the actual
16-bit write beats. The old `wr_ticks=8` applied the same four-clock duration
to bytes, halfwords and words. Its calibration came from word stores. A word
uses two beats; a byte/halfword uses one. The default word duration is retained
at the calibration clock (SDCLK=MCLK), with four half-clock ticks of fixed
overhead. Only the physical beats scale with SDCLK/DBF. `wr_ticks=0` retains
manual timing. This corrects timing for narrow stores in the default model;
the local frontend and DMA handoff overrides remain opt-in.

Current binary is `e087d459dcf544ce7bdce45569694c037948838809f5015385b7b268cb3191c6`.
Controller tests exercise both masters, byte/halfword/word sizes, two clock
rates, DBF and four calibrations, including the physical minimum. The original
flat scheduler fails 52 external-duration assertions; an initial clock-dependent
overhead prototype fails 24. The final code passes full default regressions
and experimental 122-case SD DMA/GUI/49-reset checks. Serialize Make invocations: two concurrent
invocations raced on `.build-flags.tmp` and invalidated objects during an
earlier attempt. Those failed logs are retained; sequential retries passed.

Complete isolated 608-case trials are recorded in
`build/wr128/write-width-review/three-suite-assessment.json`. With the reference
frontend and shared handoff, memory improves from 104/120 to 106/120 within
10%, log RMS 0.072271 -> 0.068190; maximum remains 30.85%. With DMA handoff
held to one clock (`dma_write_issue=2`), memory reaches 107/120 (89.17%),
log RMS 0.06378 and maximum 16.07%, with no cases over 20%. Pipeline remains
329/360; SPI transfer becomes 125/128 and contention worsens versus the first
frontend. This mixed trial is not a promoted timing calibration. Full final
binary/default assessments remain in flight; use its continuation file for
authoritative handles and exact hashes.

A no-extra-DMA-DQB-hit-wait experiment changed memory RMS only 0.067579 ->
0.067576: posted-write pressure hides nearly all of that queue-hit latency.
Do not remove the cache-hit charge as a fix for these copy errors.

The worst NuttX case now has a focused Grifo-path profile:
`build/wr128/ubench-queue-review/assessment.json`. Identical bcpad4 bytes,
addresses and boot inputs take 63.55 clocks/pass with static lookahead 6 and
20.38 with 4, versus hardware 47.85. For 524,288 loop passes, IQB misses switch
from 527,278 to 946. Fetching the branch at 0x100fee1a with lookahead 6 claims
line 0x100fee20 and evicts the line holding its loop target at 0x100fee0a.
This isolates the model's speculative fill/replacement sensitivity; neither
static distance reproduces hardware. Timed branch resolution, partial fills
and shared CPU/DMA arbitration remain the structural target. Do not globally
retune lookahead from this one case or claim the silicon overlap identified.

The physical portq report remains absent. Exact current source/binary hashes,
live jobs and next actions are in
`build/wr128/write-width-review/continuation.json`.

## Deferred instruction-fill execution

The current experimental controller issues an IQB line as four real 32-bit
reads. `iq_deferred=0` keeps the qualified timing path. Mode 1 allows CPU data
and DMA between word reads; mode 2 finishes started fills before an external
CPU data request while DMA still wins at word boundaries. DQB hits bypass
that drain. Both retain the fitted memory/controller costs without per-PC
or workload-specific adjustments.

Unissued words have no completion timestamp. A queued lookahead request does
not replace a cache slot until its first read starts; redirects can cancel
unstarted requests. Code writes invalidate partial/completed slots while
preserving an issued word's bus occupancy. Completed instruction data survives
an unrelated row change. Background progress uses event times, with bounded
work across long idle intervals. Reset and controller reconfiguration cancel
the speculative state.

`make test-sdramc-deferred` passes 67 isolated ordering/lifecycle checks;
forcing the old queue path fails 44. Full default functional/SD DMA checks
and mode-2 122-case SD DMA/GUI/49-watchdog-reset checks pass on a55fa564.
The additional 264 bus cases and reader native pixel/I/O checks also pass.
This does not complete a shared global event arbiter: existing fitted DMA
engine holds remain, and an entire blocking CPU fetch/data wait does not yet
poll asynchronous devices at each internal word boundary.

Mode 1's completed 608-case assessment is
`build/wr128/deferred-iqb/policy-one-assessment.json`: pipeline 330/360 within
10%, SPI transfer 124/128, SPI solo 120/128, memory 95/120. Contention RMS
improves 6.28213 -> 4.54297, but memory log RMS worsens 0.06378 -> 0.10289.
The focused bcpad4 loop moves from 63.55 to 44.31 clocks/pass versus hardware
47.85 with lookahead still 6. Mode 2 gives 37.62 on that loop and recovers
memory log RMS to 0.06932, 101/120 within 10%, maximum 17.63%.
Neither policy is promoted; their CPU/fetch arbitration remains a hypothesis.

Mode 2's reader screens are +2.01%, +4.01% and +2.04% versus hardware, with
forward steps +5.88--6.48% (previous width-model +8.70--9.45%). Short return
steps remain about -15.5%. This is a diagnostic improvement: unmatched
physical allocation and missing full input/boot/coasting/suspend/wake
measurements still prevent workflow qualification. The complete mode-2 and
unchanged-default five-suite comparisons are now complete. Mode 2 has
124/128 SPI transfer, 120/128 solo, 101/120 memory, 72/90 NuttX, 30/40 RISC-V
and 328/360 pipeline cases within 10%. All 738 native cases pass, but the
broader timing regressions prevent promotion. The complete default scores
and input/hardware identities exactly match the preceding width model.
A physical-bank-only variant did not fix mode 1's bulk-copy regression.

The current source/binary snapshot, completed checks, live handles and next
actions are in `build/wr128/deferred-iqb/continuation.json`. Collect portq
normally when its card returns and compare the unchanged frozen predictions.

## Instruction sampling at the fetch response

The CPU previously advanced through fetch latency and decoded bytes before
settling an SPI/DMA completion which became due during that latency. Existing
data-access settlement could hide it for loads, but a prefix or register-only
instruction executed the stale word. The core now settles asynchronous events
after any waited fetch, including fixed internal latency, then refreshes mapped
regions before sampling instruction bytes. Zero-wait fetches add no callback;
there is no extra data request, wait charge or mid-instruction IRQ acceptance.

The new real SPI/HSDMA instruction-patch, cached/remapped fetch and IVRAM tests
fail 28 checks on the previous core and pass now. All 497 CPU/DMA boundary
checks, 24 fetch-accounting combinations, 3,850 architectural comparisons and
67 deferred-controller checks pass. Full default functional/122-case SD DMA
and experimental GUI/49-reset/122-case SD DMA runs pass. Reader pixel/I/O and
measured phase results exactly match the previous width-model replay.

Both five-suite runs completed, with all 738 native cases passing under each
setting. Default scores and input identities exactly reproduce the width
baseline. The reference frontend retains within-10% counts of 125/128 SPI
transfer, 120/128 solo, 107/120 memory, 75/90 NuttX, 39/40 RISC-V and 329/360
pipeline. Evidence is in `build/wr128/fetch-response-review/verification.json`;
the isolated core diff is `change.patch`. This is a correctness fix, not a
calibration retune.

## SPI/DMA arrivals during blocked instruction fills

The current experimental word scheduler polls SPI/SD and DMA before issuing
each word of a blocking IQB demand or started-fill drain. It then rechecks bus
ownership, cache identity and controller configuration. Code invalidation can
restart the pending demand; reset/reconfiguration can resume it through the
new interface. Half-clock boundaries admit events through the preceding whole
MCLK edge, preventing a later DMA arrival from preempting an already issued
word. Event wiring survives controller reset. Default `iq_deferred=0` does not
use the hook. Timer/UART/watchdog polling and IRQ acceptance are unchanged;
this is not yet a complete machine event timeline.

Timing callbacks now return total delay from the supplied request clock and
may advance the shared clock while handling events. C33 advances to the
response once, including frontend load/store/RMW data waits. All 545 CPU/DMA
checks, 48 fetch-accounting combinations, 3,853 architectural comparisons,
86 deferred-controller checks and 262,144 fetch-path comparisons pass.
Removing only in-wait polling fails 40 checks, old additive clock accounting
fails 18, and rounding event boundaries upward fails two. Full default checks
and both experimental policies' 122-case SD DMA/GUI/49-reset runs pass.

Both full five-suite assessments pass all 738 native cases. Default scores,
input hashes and device hashes exactly match the preceding fetch-response
baseline. Mode 2 has within-10% counts of 124/128 SPI transfer, 120/128 solo,
101/120 memory, 71/90 NuttX, 30/40 RISC-V and 328/360 pipeline. Contention RMS
worsens from the earlier mode 2's 4.54038 to 13.51482 clocks/pass. Removing
only the fitted CPU hold worsens it to 18.27306; removing CPU, bus and register
holds gives 6.67657 and still misses hardware. Those two 608-case diagnostics
are separate three-suite trials, not complete qualification or promoted fits.

The manual's two-beat SDRAM READ is not evidence that DMA may arbitrate at
every 32-bit word within a CPU AHB transaction. The real transaction boundary,
AHB ownership versus SDRAM reservations, and timing of actual data/cache
commit remain unresolved. Native ordering checks prove the implemented
policy executes consistently; they do not identify the silicon's policy.
Keep both deferred modes optional. Source/binary snapshots, individual
negative controls, all timing results and remaining validation are in
`build/wr128/word-event-review/continuation.json`. Collect portq through normal
mounted files when its card returns; keep both prospective predictions frozen.

The final reader replay verifies native pixels and I/O work. All three screen
medians are +2.02--4.01% versus hardware; 13/27 phase medians are within 5%
and 21/27 within 10%. The largest phase-median change versus the previous
mode-two replay is 193.4 microseconds. Physical allocation remains unmatched
and full input/boot/coasting/suspend/wake qualification remains outstanding.
The additional 264 bus cases all pass native validation, with 240/264 within
10%. No validation jobs remain live. The complete evidence and source
snapshot are in `build/wr128/word-event-review/verification.json`; the overall
accuracy goal remains active.

## Cache coherence at write acceptance

The previous controller invalidated IQB/DQB entries while reserving a write's
future bus occupancy. A real SPI/HSDMA or IDMA destination still pending for
37 clocks thus evicted usable instruction/data entries before RAM changed.
The new 18-case matrix across three sizes, two DMA engines and all three IQB
policies fails 36 checks on the old implementation. The accepted RAM store
now notifies the controller; mere timing reservation does not flush caches.
This also covers writes without a prior timing request, and invalidates all
cached aliases of the same physical bytes. SDRAMC manual II.4.2.4 specifies
flushing a related buffer when its buffered address is written.

All 572 destination-deadline checks, 545 CPU/DMA checks, 87 deferred-controller
checks, 45 accepted-alias checks and 262,144 fetch-path comparisons pass.
The controller-only fixtures now explicitly deliver store acceptance; they
do not allocate RAM. Integration checks use real RAM and SPI/DMA devices.
Default functional/122-case SD DMA and both experimental policies' SD DMA,
GUI and 49-watchdog-reset runs pass. The matching reader replay also passes
native pixel and I/O verification. No timing calibration defaults changed.

All three full five-suite assessments are complete: default, reference local
frontend with DMA handoff=2, and deferred IQB mode 2. Each passes all 738 native
cases; scores and all input/device identities exactly match its preceding
setting. The reader replay is byte-identical to the reference. Evidence is in
`build/wr128/write-commit-review/verification.json`. This fixes premature
coherence effects, not the unresolved AHB arbitration granularity or all
asynchronous descriptor/source phase execution. Keep the IQB policies optional
and the prospective portq predictions frozen.

## Reset qualification correction and staged IDMA review

The historical 49-reset checks above used `--bare-elf`. They prove only the
isolated reset path and do not satisfy this repository's Grifo requirement.
That obsolete assembly guest and Make recipe have been replaced with a real
launcher app calling `grifo_reboot()`. The new check records four watchdog
resets and four completed FLASH/Grifo/init/app boots, including three complete
reboots, under default and both IQB policies. A global instruction budget cuts
off the final partial boot. All three new runs pass; the raw evidence and
explicit exclusion of the old reset results are in the write-commit review.
The old guest is archived there only for provenance. The new test supports a
frozen `--wremu` and retained `--out` evidence; SD DMA and GUI tests now also
accept `--wremu`.

An isolated IDMA prototype now executes descriptor reads, source read,
payload write and descriptor writeback on shared-clock events. The working
core performs 28 descriptor writes before payload acceptance across seven
native device cases. An expanded eight-case test, including a clock-valued
MMIO source, fails 57 phase checks on the working core and passes on the
prototype. The initial prototype serialized a fitted HSDMA delay, producing
up to 52% SPI transfer error. Do not promote that FIFO policy: an outstanding
completion is not evidence of continuous engine/AHB occupancy. The following
event queue selects pending effects by due time, distinguishes actual AHB
phases from fitted idle gaps, and preserves serial execution of IDMA units.
Native firmware and timing validation are tracked at
`build/wr128/idma-phase-review/continuation.json`; core sources and the
98221937 binary remain unchanged while these candidates are evaluated.

The first event-queue candidate passed 608 microbenchmark cases but failed
real revision-3 boot after 174 bytes: completing an older IDMA cleared an RX
cause that arrived during descriptor writeback. Candidate v2 preserves that
new cause using an event generation counter. It passes all 122 SD DMA driver
cases, GUI boot, three complete Grifo reboots and 112 isolated phase checks
across eight cases. Default and reference-frontend full five-suite runs remain
live. Do not merge the first candidate's timing data with v2's native checks.
Before integrating, exercise queued transactions, batched event timing,
reset cancellation and pending triggers, and verify no hardware request can
start before its actual trigger timestamp.

## Integrated SPI/DMA event timeline

The v2 prototype's full default and reference comparisons completed with all
738 native cases passing each. It nevertheless loses RX data when several
events are processed late. This is a separate, confirmed failure; those
microbenchmark results did not qualify the complete event scheduler.

The integrated revision merges pending SPI edges and DMA phases by due time.
Before SPI changes RXD or starts another buffered character, earlier DMA work
is settled. A shared-edge SPI event is visible to the DMA response at that
edge. Selecting pending effects by time preserves overlap between fitted idle
delays and actual engine work; IDMA units themselves execute serially. Both
IDMA and HSDMA source data is now sampled at response time, rather than when
future bus occupancy is reserved. CPU write holds affect a pending HSDMA
destination separately from its already reserved source response.

The new `test-dma-timeline` replaces the obsolete IDMA-only test and passes
250 checks. It covers descriptor order, clock-valued MMIO reads, multiple
queued units, reset cancellation/wiring, distinct RX bytes under late polling,
new causes during writeback, pending requests under queue pressure, and a DMA
write that gates SPI before a wire edge. Seven isolated fault variants fail
3/5/13/3/1/10/3 checks respectively; the device-only address/undefined-behavior
sanitizer run also passes all 250 checks. All 545 CPU/DMA and 572 destination-
deadline checks pass. Source, logs and controls are preserved in
`build/wr128/dma-timeline-review/`.

The integrated binary passes the full default functional suite, both IQB
policies' 122-case SD DMA/GUI/real Grifo reboot checks, and reader pixel/I/O
verification. Reader screens are +3.19--5.35%, forward steps +8.70--9.45% and
short returns about -14.5--14.8%; these remain diagnostic phase measurements.
Default/reference full timing comparisons are complete, with all 738 native
cases passing under each setting. Default within-10% counts are transfer
124/128, solo 118/128, memory 106/120, NuttX 75/90, RISC-V 39/40 and pipeline
169/360. The reference frontend gives 123/128, 120/128, 107/120, 75/90, 39/40
and 329/360 respectively. SPI contention RMS improves while transfer RMS
worsens; preserve both in `baseline-comparison.json`. Memory and NuttX remain
below the representative threshold.
No fitted defaults changed and the IQB/frontend overrides remain optional.
Exact identities and completed checks are in
`build/wr128/dma-timeline-review/verification.json`; no validation jobs remain
live. The full accuracy milestone,
physical portq report, remaining arbitration identification and whole-reader
workflow qualification are still outstanding.

## Timed-fetch hypotheses and obsolete test cleanup

The working emulator remains `8dafbeab7a48923721a08de67efb8a6a5e74a4d8274a6a9346fc85816d06a235`.
`build/wr128/timed-fetch-review/assessment.json` rejects 27 immutable-category
fetch/FIFO/branch hypotheses over 15 real NuttX loop bodies. All 420 host
simulations complete, with chronological SDRAMC requests asserted. Fifteen
legacy controls reproduce executing-loop costs within 1%; the best candidate
has only 11/15 within 10%, versus 14/15 for the control. These are diagnostic
host simulations, not executing CPU measurements. No model or core changes
are promoted. In particular, the raw-category peek is valid only for these
immutable inputs; it does not implement asynchronous instruction sampling.
External fetch admission/cancellation and bcpad4 remain unresolved.

The old differential startup and linker scripts and Doom's bare linker script
are removed. `difftest/run.py` builds normal Grifo apps and verifies complete
boot, exact nonempty output and poweroff, retaining per-run input hashes and
logs. Its shell wrapper preserves DT_* callers. ABI, reduced compiler and
Doom math callers use that same path. Coverage reads only completed verified
logs and explicitly includes boot opcodes. Large arithmetic result streams
use the real filesystem, preserving exact comparisons. The prior serial
Doom attempt exhausted its bounded budget after 4,750 of 227,672 outputs;
it is retained as an incomplete attempt, not a native arithmetic failure.
The filesystem run completes all 227,672 exactly. Both compilers' generated
programs, all four ABI combinations and the reduction pass; see
`build/wr128/difftest-grifo-review/verification.json`.

No timing or validation jobs remain live. The card is still absent and the
existing physical portq request remains pending; do not request its path or
use raw-device reads. Next work is independent evidence for external fetch
admission/cancellation, the returned prospective port report, and full reader
workflow measurements. The overall accuracy goal remains active/incomplete.

## Prepared external-fetch discrimination probe

`tools/fetch_bench` now provides a 600-case controlled probe, exact machine-
code/layout validation, real-Grifo runner, collector, frozen comparison and
installer. It is prepared locally, not installed. Do not overwrite portq
before its physical report is collected; the installer enforces this and
preserves the cleaned six-entry menu and all five ordinary applications.

Five settings pass all 600 native cases using the same 8dafbeab core and
probe app. The final forecasts are `build/wr128/fetchq/prospective.json`, SHA
`d051baa471e507c0a1dc62cc770409f38fd50c1a036118d05f24e561ca0158ab`.
They include current defaults, the optional local-frontend reference, both
deferred IQB policies and demand-only IQB fetching. No physical timing exists
yet. Do not refit or overwrite these forecasts before collection.

The A0 measuring wrapper switches to an owned A0 stack so call/return does
not inject external stack traffic. It preserves/restores SP and disables
interrupts through the caller. Before timing, it configures IQBEN, rewrites
unchanged halfwords in eight copied-code lines and reads back the last one.
The manual guarantees matching-write invalidation but not emptying on an
IQBEN toggle. Initial toggle-only builds are archived and excluded from the
final forecasts. One- and eight-pass cases expose entry/exit costs; raw
interval ticks include those costs without a fitted subtraction.

Seven host tests pass, including failure archiving, clock/address rejection,
and a simulated card transition with verified backup/menu/app preservation.
All jobs are complete. No model defaults or executing core changes were
made. The pending local-port report, this new physical fetch report, hardware
arbitration evidence and full input-driven reader workflow remain required.
While the card is absent, advance reader input/suspend/wake measurement
preparation rather than rerunning unchanged suites or fitting these forecasts.

## Returned portq/fetchq evidence and SELDO correction

Both physical reports are collected and valid: portq SHA
`13425df38ae48578f875f2ffaff0db8108b63dd35d4c09a1f11135b72b39b72e`
(560 cases), and fetchq SHA
`ad2e63fb47d85635a84b71fa64ab909c2a0699a2ae3e7eb1cca53fa72c9d7847`
(600 cases). No hardware request remains pending. The same five ordinary
apps were verified after the fetch installation/remount and again after its
return; the card is ejected. Only fetchq remains as its diagnostic menu item.

The unchanged portq eight-halfword forecast reaches 496/560 within 10%, but
only 217/280 shared A0. Executing C33 gives 489/560 (shared 218/280, separate
271/280), against defaults 208/560. Do not conflate the declarative forecast
and actual CPU: their memory services differ. All 56 dependency contrasts
have median separation <=0.0003 MCLK/pair. Short shared-port loops retain
large errors; no generic interlock or model promotion is justified.

Fetchq's unchanged forecasts give 405/406/421/420/344 within10 out of600.
The header differs only in read-only REF.D25 SELDO, not configuration. The
manual II.4.1.5 and II.4.4 explicitly distinguish SELEN from actual SDCKE
status; the old MMIO code forced SELDO whenever SELEN was enabled. Analysis
now records the raw status discrepancy while requiring every writable
setting to match. The frozen JSON/model predictions were not changed.

The rejected readback fix advances autonomous self-refresh at the MMIO read,
after any elapsed deferred instruction words. It clears SELDO after a real
external access while SELEN can stay enabled. New controller tests fail 20
checks on the old object and pass now, including exact SELCO boundaries,
cache hits, write protection, disabled hardware and both deferred policies.
Full default functional/SD DMA/GUI/real Grifo reboot checks and all five
native timing suites passed, but deep suspend did not. Production Grifo
programs AURCO=0 before waiting for SELDO. The current refresh scheduler
treats zero as a refresh every SDCLK, preventing actual self-refresh entry.
The earlier forced status bit masks this controller problem. The manual's
ordinary SELCO<AURCO rule does not explain the shipping zero setting; its
special semantics remain unverified. Do not change the kernel or guess a
new zero-refresh timing policy to get a pass.

Binary `abd94a5b0fd167087f15a5add33e6813989206cf36a55d62a9da81cd3c677485`
and its source/tests are preserved as `rejected-*` under
`build/wr128/self-refresh-status-review/`. The before/after smoke runs use the
same guest and kernel: the baseline completes two suspends/resumes, while
the candidate exhausts its instruction budget polling at PC 0x81c. The
maintained test reproduces the rejection. The source/controller tests were
restored and rebuilding reproduced the exact baseline binary hash.

The inaccurate SELDO readback therefore remains a known issue in the working
core. Resolving zero-AURCO semantics, external-fetch admission/cancellation,
short shared-port loops and full input-driven physical reader workflows are
follow-up work. The two collected probes do not need rerunning. Read
`build/wr128/self-refresh-status-review/verification.json` and
`continuation.json` for the final disposition rather than treating the
candidate's passing calibration runs as qualification.
