# Fetch/decode timing experiment

Latest physical variable-unroll evidence is now collected: the unchanged
eight-halfword forecast reaches 496/560 within 10%, but only 217/280 for
shared A0. The actual executing frontend reaches 489/560, versus default
208/560. These prospective results supersede expectations, while preserving
the original predictions. See `../port_bench/README.md` and
`build/wr128/portq/prospective-hardware-comparison.json` for all six frozen
alternatives and separate executing-C33 results. No configuration was
refitted after this return. Shared-port behavior remains incomplete,
especially for short loops; the depth change alone does not establish
the hardware's queue organization.

`frontend.py` is a timing-only event model of the fixed instruction shapes
in `pipeline_bench/pipeline.c`. It does not execute guest code, access a
device, or qualify the production emulator. The production CPU is unchanged
by this experiment.

```sh
python3 emulator/tools/experiments/frontend.py --out build/wr128/frontend-lab/queue.json
python3 emulator/tools/experiments/frontend.py --freeze-decode memory --out build/wr128/frontend-lab/freeze-memory.json
```

Both commands compare all 20 shapes at all eight A0 halfword offsets with
A0 data: 160 cases from the hash-verified physical pipeline report. Results
include every prediction/error, model configuration and source/report hash.
Quarter-MCLK events let fetch and data port handoff vary without rounding
each individual access up to a full clock. Loop timing is measured after
half the simulated iterations; physical ticks retain entry/timer overhead.
There is no fitted constant subtraction from the device results.

The hypothesis uses a shared 32-bit A0 port, four raw instruction halfwords,
one completed decode slot, a one-clock F stage and manual three-clock branch
execution followed by restart. F can combine a prefix with its following
base instruction if the prefix was ready at the beginning of the stage and
the base is ready by its end. Consecutive prefixes still consume decode
bandwidth. This produces queue-state-dependent compression without assigning
every prefix the same flat execution cost.

`--freeze-decode none` permits F to work while E handles memory;
`memory` blocks F throughout memory execution; `wait` allows F during the
last memory execution cycle. E-stage memory service defaults (1.5/2/1 MCLK
for halfword read/word read/store) come from the measured SDRAM-code/A0-data
slopes. Applying them with A0 code and the quarter-clock port handoff remain
hypotheses, not established properties of the reader.

Initial observations, 2026-11-05:

- Both variants reproduce the base, single/double prefix, move/prefix and
  prefixed-branch patterns across all eight alignments. Their largest raw
  relative error among these 48 cases is 0.23%, including timer overhead.
- The queue-only variant has 4.81% median, 25.06% p90 and 28.09% maximum
  error across all 160 cases. Its extended memory and load/consumer shapes
  remain too fast.
- Freezing decode during memory reduces those overall figures to 3.27%,
  17.25% and 22.19%, but worsens plain halfword loads from at most 4.22%
  error to 17.25%. Lower aggregate error does not establish the mechanism.
- Neither variant models a load-result dependency. The returned matched
  264-case bus probe now supports this choice: 64/66 contrasts are effectively
  zero, while the two SDRAM exceptions differ in row-boundary placement.
  See `bus_bench/README.md` for the measurements and remaining uncertainty. The core manual states that no interlock cycle is
  generated; do not add a generic load-use penalty from the old unmatched
  instruction mixes alone.

The experiment has no SDRAM timing, DMA arbitration, architectural semantics,
interrupts or self-modifying-code behavior. A replacement frontend must
integrate those with the actual CPU and pass the full firmware calibration
and functional checks before becoming the default. These 160 predictions
must not be substituted for those acceptance requirements.

The returned bus report can now be checked alongside the original probe:

```sh
python3 emulator/tools/experiments/frontend.py --bus-device emulator/tools/bus_bench/device.json --out build/wr128/frontend-lab/returned-controls-dispatch.json
python3 emulator/tools/experiments/test_frontend.py
```

This extracts all 44 instruction shapes from the hash-verified generated bus
source. It compares 88 A0-code cases per data placement, without introducing
register-dependency penalties. For SDRAM data, the local instruction port is
separate; two-cycle reads and four-cycle posted writes are explicit constant
service hypotheses. SDRAM queues, refresh and arbitration are still absent.
Results remain separated by data placement rather than pooled.

The review found an event-order bug in this timing experiment: a data request
created by E-stage dispatch could be serviced only on the next quarter tick,
even on an idle, separate port. Moving request service after dispatch removes
this artificial delay. A regression checks eight consecutive transfers against
the exact latency/loop-cost formula across three latencies and both alignments;
another checks convergence with doubled simulation length. This correction
does not alter the production CPU or indicate hardware has quarter-clock waits.

After correction, the separate-port bus cases have median error 0.10%, p90
1.41%, maximum 15.14%, with 86/88 within 10%. The shared-A0 cases still have
median error 16.28%, p90 27.08%, maximum 31.16%, with 38/88 within 10%.
The original 160 shared-A0 cases now have median error 5.86%, p90 27.08%,
maximum 31.16% (88/160 within 10%). The earlier lower errors partly hid the
dispatch bug; preserve those old artifacts but do not treat them as corrected
predictions. The remaining mismatch is concentrated in shared local fetch/data
behavior, supporting further arbitration/backpressure work rather than adding
a generic load interlock. No production timing change is qualified here.

A second optional hypothesis is available as `--trailing-ext`. If F is decoding
an unextended register operation and the next queued instruction is an EXT
already ready at F-stage start, the extension latch can predecode that EXT
for the following operation. A current extended instruction occupies the latch
and cannot do this. This is a proposed queue/latch mechanism beyond the
manual's general lookahead rule, not an established implementation detail.

With this rule the separate-port `mixed` cases become 46/47 cycles/pass,
against hardware 46.029/47.029, while the single-/double-prefix controls stay
14 and 21/22. All 88 separate-port controls are now within 3.43% (median
0.10%, p90 0.23%). The shared-A0 scores remain unchanged, including their
large misses. The instruction sequence/extension-count trace is identical
with and without lookahead for all original shapes, both alignments and both
port arrangements. Doubling simulation length preserves the tested steady
predictions. `trailing-prefix.json` and `trailing-prefix-traces.json` under
`build/wr128/frontend-lab` retain the predictions and dispatch traces. This
rule still needs shared-port behavior and whole-firmware validation before
any executing CPU integration.

The subsequent port review separates fetch-to-data/data-to-fetch turnaround
(`--port-turns` in quarter clocks), data response latency
(`--response-delay`) and CPU stage starts (`--stage-edge`). Current timings
do not independently identify response latency and data-to-fetch turnaround:
moving two quarter clocks between them can produce identical predictions.
Do not interpret a best-fitting split as a measured physical bus delay.

A second numerical scheduling bug let decoding free FIFO space but delayed
the next fetch offer by one simulation tick. Fetch now retries at that same
instant. For half-clock physical delays, quarter- and half-clock numerical
resolutions give identical execution and port-event traces. Some earlier
scores were accidentally better due to that artificial stall; those records
remain historical, not the current corrected hypothesis.

`--prefix-gate` optionally prevents EXT decoding while local data execution
occupies a proposed shared resource, without freezing ordinary decoding.
Together with trailing-prefix lookahead, `(2,2)` port turns and clock-edge
stage starts, this gives 140/160 original shared-A0 cases within 10% (median
2.88%, p90 10.99%, maximum 15.62%) and 78/88 additional shared-A0 cases
(median 4.38%, p90 11.42%, maximum 17.32%). All 88 separate-port controls
remain within 3.43%. Neither shared set meets the 90% target. These are still
unqualified structural hypotheses. Original reports and configuration sweeps
are under `build/wr128/frontend-lab/port-turnaround`.

```sh
python3 emulator/tools/experiments/frontend.py --trailing-ext --prefix-gate --port-turns 2,2 --stage-edge clock --bus-device emulator/tools/bus_bench/device.json --out build/wr128/frontend-lab/port-turnaround/my-prefix-resource.json
```

Eight invariant tests cover dispatch latency, convergence, instruction/prefix
sequence preservation, resolution independence, port exclusion/turnaround,
clock edges, unaffected separate-port controls and variable-unroll access
counts. `simulate(..., unroll=N)` now varies the body copies explicitly;
the default remains eight. The separate `tools/port_bench` physical application
uses seven unroll counts to distinguish per-copy throughput from loop restart.
It has passed all 560 cases through real FLASH/Grifo/init in the emulator;
hardware evidence is still pending. No executing CPU timing parameter is
changed by this laboratory work.

## Prospective variable-unroll comparison

`port_predictions.py` now reads each actual `PROBE_N` assembly template and
unroll count from the hash-verified generated application source. It verifies
the native Grifo-path replay and freezes predictions for all 560 cases, plus
the exact model/generator sources. It refuses to overwrite a prediction file.
Quarter- and half-clock numerical resolutions must agree for every case and
configuration. No physical report or fitted timer offset is used.

`build/wr128/frontend-lab/port-prospective.json` contains the initial five
configurations: shared-prefix resource, no prefix resource, no fetch-to-data
turn, no trailing-prefix lookahead and an alternative response/turnaround
split. The last is timing-equivalent in all 560 cases. This probe cannot
identify those two internal delays separately. The other alternatives differ
from the shared-prefix model in 43, 177 and 22 cases respectively; their
largest separations are 9, 9 and 15 cycles/pass. These are fixed predictions,
not observed silicon differences.

A raw-capacity review (`queue-capacity-review/summary.json`) compared 2, 3,
4, 6 and 8 halfword FIFO slots without changing the existing service/decoder
rules. Eight gives 152/160 original shared-A0 cases within 10% (median 3.11%,
p90 8.95%, max 12.37%) and 83/88 additional shared-A0 controls (max 17.32%).
All 88 separate-port controls remain within 3.43%. Six gives 144/160 and
80/88 respectively; two clearly regresses. The manual's four instruction
queues do not establish the raw storage width or allocation among stages.
Treat this as a capacity hypothesis, not evidence that the hardware has
four 32-bit queue registers. Both existing probe sets informed this review;
the new variable-unroll report will be the prospective check.

The six-configuration prospective snapshot, adding eight raw halfwords, is
`port-prospective-queues.json` (created before physical report collection).
It differs from four raw halfwords in 61 shared-A0 cases, by at most two
cycles/pass, and leaves separate-port cases unchanged. Those small changes
move many near-threshold cases; passing the offline percentage alone does
not establish the mechanism or qualify the executing CPU. The physical app
is installed and the card is ejected for the user's run. No production timing
default changes here.

```sh
python3 emulator/tools/experiments/port_predictions.py --out build/wr128/frontend-lab/my-prospective.json
python3 -m unittest discover -s emulator/tools/experiments -p 'test_*.py'
```

## Native streaming scheduler prototype

`src/frontend.[ch]` provides the scheduling state machine in C. It is now
linked into C33 behind the disabled-by-default `local_frontend` option.
The standalone checks below validate its scheduling separately. A side-effect-free
mapped-word callback supplies instruction bits and timing categories; actual
register, memory, exception and interrupt effects remain the C33 executor's
responsibility. Each EXT word is delivered in semantic order even when its
timing overlaps decoding. Non-EXT words dispatch into E, and the caller
resolves execution duration and control flow explicitly.

`make -C emulator test-frontend-native` compares 778 instruction-stream
configurations against the prospective Python model: exact E-issue traces,
prefix counts, addresses, instruction-word delivery and loop times. Coverage
includes capacities 4/8, alignments 0/2, both data ports, unroll 1/3/8/16,
optional decode rules, adjacent loads and directional handoff variants.
Lifecycle checks cover reset, redirect, explicit code/mapping invalidation,
rejected instruction regions and retaining in-flight port occupancy across
a speculative flush. Multiple data phases keep E active until explicit
completion. Long waits jump between event deadlines once queues fill.
All five test groups pass; evidence and source/binary snapshots are under
`build/wr128/frontend-lab/native-frontend/verification.json`.

The integration preserves separate retirement of prefix words, the existing
interrupt deferral through EXT sequences, third-prefix exceptions, reset,
fault PCs and code/mapping changes. Flush prefetched words when required;
do not replace mapped prefetch with speculative MMIO reads. Derive memory
phases from the real rd/wr waits, including multi-access instructions, and
avoid charging legacy local-data/prefix/branch costs on top of scheduled
ones. Quarter-clock port occupancy and whole-clock CPU events need explicit
accounting rather than rounding every phase independently. The standalone
driver executes no guest instructions and does not qualify firmware timing.

## Executing CPU experiment

`WREMU_MODEL=local_frontend=1,itc_wait=4` now enables the scheduler in C33 for
mapped area-0 code, including its IVRAM alias. Defaults remain unchanged.
The executor supplies real instruction bits, rd/wr waits and resolved control
flow; old prefix/local-data/branch timing is excluded while scheduling is
active. Unsupported code regions retain the legacy timing path. The ITC
override comes from a 13.01-clock hardware polling control (modeled as
9 + itc_wait after branch/F timing is separated), not a workload-specific cost.

`make test-frontend-cpu` checks 3,850 paired architectural states and explicit
prefix/IRQ/exception, self-modification, host patch, mapping/alias, reset and
CPU-copy cases. `make test-cpu-dma-order` includes real SPI/DMA completion
during an active frontend data wait. Full firmware qualification uses the
normal Grifo path. All 738 calibration cases, 264 extra bus cases, 560 emulator
variable-unroll cases, reader output/I/O checks and 122 SD DMA cases pass.

The complete experimental binary is `9102183f84868c9c7b113d05ccbc386d195142c73ab16fda3054769bcbd1f8ab`.
Pipeline stress improves from 169/360 to 329/360 within 10% of hardware;
additional bus cases improve from 176/264 to 243/264. Memory and SPI RMS
tradeoffs remain, so this is opt-in. Full per-suite results and exact source
snapshots are in `build/wr128/frontend-integration/verification.json` and
`area0-accuracy.json`; reader comparisons retain their workflow limitations.
The physical variable-unroll report is still outstanding, and the frozen
prospective predictions remain unchanged. These results establish executing
CPU progress, not a final identification of queue capacity or bus internals.

## Deferred SDRAM instruction reads

`iq_deferred=1` or `2` now supplies an additional executing controller
experiment. Its two IQB slots retain eight halfwords each, but only actual
32-bit READ reservations receive completion times. A separate speculative
request holds its address until a word can start, so merely predicting a
third line no longer evicts a live slot. Background reads catch up at bus
events; DMA can claim an exact word boundary. Invalidating code cancels
unissued reads and preserves real in-flight occupancy. Completed IQB contents
remain usable after an SDRAM row change.

The two policies test CPU-side arbitration separately: mode 1 yields between
words to CPU data as well as DMA; mode 2 finishes started fills for external
CPU data requests, allowing DQB hits through. This distinction is inferred,
not specified by the manual's priority between bus masters. Neither policy
adds register-dependency penalties or per-workload costs. `iq_deferred=0`
retains the qualified path; fitted DMA engine holds remain in both trials.

`make test-sdramc-deferred` checks 67 controller-level cases, including DMA
arrival before/at a word boundary, completion times after preemption, deferred
replacement, redirect cancellation, CPU/DQB policies, long idle intervals,
partial-fill invalidation and reset. Forcing the old path fails 44 checks.
Full native SD DMA/GUI/watchdog, bus and reader output/I/O checks pass.

Mode 1 improves the focused bcpad4 loop from 63.55 to 44.31 clocks/pass
(hardware 47.85) with lookahead unchanged at 6; SPI contention RMS improves
6.28213 -> 4.54297. Its CPU bulk-copy timing regresses. Mode 2 recovers most
of the copy regression and puts all three reader screen medians within
4.01%, but the focused loop becomes too fast. Its complete five-suite results
regress to 101/120 memory, 72/90 NuttX and 30/40 RISC-V cases within 10%, so
it is not promoted. See `build/wr128/deferred-iqb/verification.json` and the
per-case evidence; reader gains do not establish the final arbitration policy.

Next work must address timed CPU fetch/branch resolution and asynchronous
requests arriving during a blocking CPU wait. Deferred controller reads alone
do not create a shared event timeline for the entire machine. Keep prospective
port predictions unchanged until the physical report is collected.

The following core correction settles asynchronous events at a waited fetch
response before sampling instruction memory. This makes a completed DMA code
write visible even when the fetched instruction performs no data access.
Mapping epochs are refreshed after settlement. Actual SPI/DMA tests, mapped
and fallback reads, remapped code caches and fixed IVRAM latency cover the
ordering; zero-wait paths retain their callback behavior. It adds no data
transaction or wait charge. Current full timing evidence is tracked in
`build/wr128/fetch-response-review/continuation.json`. This response-boundary
fix is a prerequisite, not a replacement, for scheduling DMA arrivals inside
a multi-word blocking fetch.

## Experimental arrivals inside a blocking fill

The executing deferred IQB modes now poll SPI/SD and DMA before each unissued
word of a blocking demand or CPU fill-drain. A callback can advance the CPU
clock; the C33 request/response contract counts that progress exactly once.
After polling, the controller rechecks bus ownership, slot validity and
configuration. Polls at half-clock SDRAM boundaries stop at the preceding
whole MCLK edge. Reset retains host event wiring. There is no recursive
polling from DMA reservations, additional IRQ acceptance point or full
timer/UART/watchdog timeline.

Checks now cover 545 CPU/DMA boundaries, 48 fetch-accounting combinations,
3,853 paired architectural states and 86 controller cases. A real SPI/HSDMA
code patch during a blocked last-word fetch invalidates the partial fill and
changes the retired prefix. Removing in-wait polling fails 40 checks; old
additive clock accounting fails 18 and upward rounding fails two. Both full
five-suite runs pass all 738 native cases and default scores are unchanged.

This does not validate word-granular silicon arbitration. Mode 2 contention
RMS worsens to 13.51482 clocks/pass, and removing fitted DMA holds does not
resolve it. A two-beat SDRAM READ does not establish the boundary of a CPU AHB
transaction. Separate reservation, ownership and actual cache/data completion
before selecting that granularity. Both modes remain optional; complete
per-case evidence is in `build/wr128/word-event-review/continuation.json`.
# Preserved forecast evidence

The byte-for-byte pre-collection port/fetch forecasts, port model snapshots
and raw fetch model reports are versioned under
[`evidence/2026-11-05`](evidence/2026-11-05/README.md). Their original hashes
and historical classifications are unchanged; executable/card fixtures stay
in local build directories.
