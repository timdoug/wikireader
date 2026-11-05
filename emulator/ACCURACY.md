# Timing accuracy milestone

The active work continues the 2026-11-05 hardware review. The objective is
90% of representative timing cases within 10% of matching hardware, every
remaining error over 20% explained by reproducible evidence, and reliable
measurements of reader workflows. This is broader than passing the current
functional regressions or improving one aggregate score.

## Acceptance evidence

- Report median, 90th-percentile and maximum absolute relative error for
  each calibration suite, plus per-case signed errors and the existing RMS
  scores. Keep complete case coverage and matching workload/input hashes.
- Reach the 10% threshold in each representative suite: CPU/DMA memory
  copies, NuttX instruction/memory loops, RISC-V workloads and SPI transfer
  timing. Score pipeline stress cases separately; do not hide their misses
  by pooling them with many easier cases.
- Every case beyond 20% needs an isolated reproducer and evidence identifying
  the missing mechanism. A generic label such as "pipeline approximation"
  is not sufficient evidence of explanation.
- Validate a frontend model against prefix, branch, alignment, load/store
  and mixed-work probes, then against firmware workloads excluded from
  fitting. Preserve instruction semantics, exceptions, interrupt ordering,
  reset behavior and deterministic execution.
- Validate CPU/DMA arbitration with concurrent CPU and transfer observations,
  including queue hits/misses and SDRAM code. Keep card latency separate
  from bus occupancy and CPU execution time.
- Measure boot, search, article loading, scrolling and suspend/wake on the
  physical reader and emulator with repeatable inputs and matching firmware,
  card content and clock setup. Record raw repetitions and phase boundaries.
  Target workflow medians within 5% on the measured reader/card combination.
- Run the full functional regression suite and retain the hardware reports,
  input identities, model settings and emulator binaries for qualification.

## Work order

1. Model instruction fetch/decode and its interaction with local data access.
2. Resolve the largest CPU/DMA arbitration errors with hardware evidence.
3. Add and qualify the whole-reader workflow measurements.
4. Extend measurements to additional cards/clock configurations when available;
   report the validated hardware envelope rather than extrapolating silently.

Baseline evidence is `tools/pipeline_bench/assessment-2026-11-05.json` and
`build/wr128/pipeline-review/qualification.json`. The baseline has substantial
pipeline stress errors and does not meet this milestone. Record progress
and remaining requirements without marking the goal complete prematurely.

`tools/accuracy_report.py` reads completed calibration outputs, verifies
binary/device identities and reports plain per-case relative errors without
pooling suites. It accepts separate suite runs only with identical emulator
binaries, settings and environments. Run with `--require-milestone` for a
nonzero exit until all requirements have evidence; workflow qualification
remains unqualified. The standalone partial reader comparison is recorded
separately below; it is not included in that five-suite bundle. The detailed current baseline is
`build/wr128/idma-deadline-review/accuracy.json`. The CPU data-access event-order
fix improves SPI contention and NuttX scores; memory, RISC-V and pipeline
scores remain unchanged. The earlier SDRAM/reset baseline is retained in
`build/wr128/sdram-review/accuracy.json`. The later IDMA deadline correction
preserves all five scores from `build/wr128/event-order-review/accuracy.json`.

The 264-case matched dependency probe in `tools/bus_bench` now has a verified
hardware report. 64/66 matched contrasts are effectively zero. The two SDRAM
exceptions have different row-boundary placement; all corresponding A0
contrasts are zero. This supports structural fetch/data effects rather than
a generic load-result penalty. Current production accuracy on this additional
set is 176/264 within 10%, median error 7.17%, p90 40.86%, maximum 64.17%.
`build/wr128/buspipe/current-comparison.json` retains the complete evidence.

The reader probe in `tools/workflow_bench` now has verified hardware reports
for nine article loads and 36 active-gesture viewport steps. Native output,
clock setup, app/boot/content/resource identities and per-pass I/O work match.
`build/wr128/workflow-calibration/hardware-comparison.json` records 27 phase
medians: 21 within 10%, none within 5%, median absolute error 7.89%. Finished
screens take 6.08--7.89% longer in the model; downward steps 9.18--9.95%
longer; short return steps 14.28--14.73% less time (about 0.44 versus 0.52 ms).
App-to-keyboard takes 5.81% longer. The remaining retrieval difference lies
mainly outside card reads, supporting further CPU/fetch investigation.

This comparison remains diagnostic: physical filesystem allocation is
unmatched, total kernel boot performs different menu/file-read work, and
only one cold boot is measured. Full input-driven workflows, coasting,
repeated boots and suspend/wake qualification remain outstanding. This does
not meet the workflow or overall accuracy milestone.

The completed address-phase/fetch-accounting correction established the
preceding full baseline: binary `6d17536e23f87482869963ecc3d3b6d98e7109ea87515e29834b02a42ddde93d`,
with evidence in `build/wr128/address-phase-review/accuracy.json` and
`verification.json`. All 738 native cases pass, as do the additional bus and
reader output/work checks. Cases within 10% are SPI transfer 127/128,
SPI solo 118/128, memory 104/120, NuttX 74/90, RISC-V 39/40 and pipeline
169/360. The memory result includes one small regression across the threshold
(+9.904% to +10.004%), retained explicitly with cause unresolved.
The final reader screens are +6.03--7.77%, forward steps +8.86--9.57% and
short return steps about -14.4--14.8%, still outside the workflow target.
Historical reader statistics above belong to the previous binary.

The subsequent binary removes a host false stop on nine consecutive valid NOPs;
it passes full functional checks and all 560 new variable-unroll probe cases.
This new probe is installed but has no physical report yet. Offline shared-port
hypotheses still miss the acceptance threshold and have no executing CPU
qualification. Keep these separate from the completed baseline's scores.

The latest complete baseline is now binary
`bc75d10a0ce914374f845b0978736d2765e22787688a178a402f2056ec9124e2`,
with DMA destination-acceptance, active-engine, CPU post-wait settlement and
posted-write ownership corrections. `build/wr128/dma-write-review/accuracy.json`
and `verification.json` retain the full assessment. All 738 native cases,
311 destination-boundary assertions, 122 SD DMA cases, 264 additional bus
cases, 560 variable-unroll cases and reader pixel/I/O checks pass. Within-10%
case counts stay as above. SPI contention RMS regresses 6.141073 -> 6.367898,
transfer log RMS 0.026892 -> 0.028207 (transfer p90 4.17% -> 5.07%), and
NuttX log RMS 0.074854 -> 0.075158; memory, RISC-V and pipeline scores are
unchanged. This is correctness progress with mixed timing results. No model
defaults were refitted, and no calibration job remains live.

The physical variable-unroll probe is now installed and verified after
remount. At the user's request, obsolete probes were backed up and removed
from the card, preserving the five ordinary apps and the current probe.
The card is ejected for its hardware run. That report is still outstanding;
it is not included in any completed timing accuracy claim.

The new opt-in executing frontend (`local_frontend=1,itc_wait=4`) has completed
all five suites with binary
`9102183f84868c9c7b113d05ccbc386d195142c73ab16fda3054769bcbd1f8ab`.
`build/wr128/frontend-integration/area0-accuracy.json` and `verification.json`
retain exact results and validation. All native checks pass. Within-10% counts
are SPI transfer 127/128, SPI solo 120/128, memory 104/120, NuttX 74/90,
RISC-V 39/40 and pipeline 329/360 (91.39%, previously 46.94%). The additional
bus set improves to 243/264 (92.05%). These are real Grifo-path execution
results, superseding the earlier unintegrated prototype assessment.

Reader screens improve to +3.34--5.50%; 11/27 phase medians are within 5%,
21/27 within 10%. Physical-layout/input/boot/wake limitations still apply.
SPI solo absolute-cycle RMS and memory log RMS regress despite the pipeline
and reader gains; defaults remain unchanged. Seven five-suite cases exceed
20% without an established mechanism, and memory/NuttX still miss the suite
threshold. The prospective physical variable-unroll report, SDRAM arbitration,
posted-store analysis and full workflow qualification remain outstanding.
The overall goal remains active and incomplete; no calibration job is live.

A subsequent CPU/DMA handoff sensitivity review is recorded in
`build/wr128/conditional-restart/three-suite-assessment.json`. The isolated
CPU-only handoff trial reaches 345/360 pipeline cases and 128/128 SPI solo
cases within 10%, but memory falls to 101/120 and reader screens slow to
+4.40--6.43%. It is not promoted. The new optional `dma_write_issue` control
preserves the previous shared cost when zero, allowing those effects to be
separated without changing defaults. Full default and experimental SD DMA,
GUI and watchdog checks pass. The integrated five-suite comparison is now
complete, with all 738 native cases passing and scores identical to the causal
trial. `build/wr128/conditional-restart/integrated-accuracy.json` records the
full result; memory and workflow tradeoffs still prevent promotion.

The current write-width correction separates fixed overhead from the actual
16-bit SDRAM beats, preserving word timing at the calibration clock and
charging narrow writes one beat. With the reference frontend and a one-clock
DMA handoff trial, the completed 608-case comparison reaches memory 107/120
within 10%, log RMS 0.06378 and maximum 16.07%, with no memory cases over 20%.
Pipeline remains 329/360; SPI transfer falls to 125/128 and contention worsens
versus the first frontend. All native checks pass. These are partial trial
results, not qualification of the final binary. Default and experimental
functional/SD DMA/GUI/watchdog checks pass on the final clock-aware code; its
complete five-suite runs remain live. Exact identities and handles are in
`build/wr128/write-width-review/continuation.json`.

`build/wr128/ubench-queue-review/assessment.json` isolates the largest NuttX
error's sensitivity to speculative IQB replacement: the identical focused
loop takes 63.55 clocks/pass with lookahead 6 and 20.38 with 4, versus hardware
47.85. IQB misses move from approximately one per pass to nearly none. Neither
constant reproduces the device; timed fetch/branch overlap remains unresolved.
Do not count this diagnostic as a completed explanation of silicon behavior.

The write-width assessment has now completed on binary
`e087d459dcf544ce7bdce45569694c037948838809f5015385b7b268cb3191c6`.
Both default and experimental runs pass all 738 native cases. Experimental
within-10% counts are SPI transfer 125/128, SPI solo 120/128, memory 107/120,
NuttX 75/90, RISC-V 39/40 and pipeline 329/360. Memory maximum is 16.07%;
five other-suite cases still exceed 20%. Full evidence is in
`build/wr128/write-width-review/verification.json`. Suite and workflow targets
remain incomplete.

Deferred IQB word scheduling is now implemented, off by default. The first
policy improves SPI contention RMS to 4.54297 and the focused bcpad4 case to
44.31 clocks/pass, but memory drops to 95/120 within 10%. A second policy
preserves DMA word priority while draining started fills for CPU data misses;
memory recovers to 101/120. Its screens are within 4.01% and forward scroll
medians within 6.48%, with native pixels/I/O matching. Short returns remain
about 15.5% too fast and the physical/workflow limitations still apply.
Both default and experimental functional/122-case SD DMA checks pass, as do
67 scheduler tests and 264 bus native cases. The complete mode-two assessment
now has SPI transfer 124/128, solo 120/128, memory 101/120, NuttX 72/90,
RISC-V 30/40 and pipeline 328/360 within 10%, with all 738 native cases passing.
The default scores exactly match the width baseline. The broader regressions
prevent promotion; see `build/wr128/deferred-iqb/verification.json`.

The current core corrects stale instruction sampling after a waited fetch.
Events are settled before reading instruction bytes, without extra memory
requests or timing charges. Real SPI/DMA tests fail 28 checks on the old core
and pass now; all 497 CPU/DMA boundary checks and full functional/SD DMA/GUI/
watchdog checks pass. Reader phases and pixel/I/O results reproduce the width
baseline. Both full timing assessments completed with all 738 native cases
passing; default scores exactly reproduce the width baseline, and reference
frontend within-10% counts remain 125/128 transfer, 120/128 solo, 107/120 memory,
75/90 NuttX, 39/40 RISC-V and 329/360 pipeline. Exact evidence is in
`build/wr128/fetch-response-review/verification.json`.

The following word-boundary experiment also passes both full 738-case native
assessments, preserving default scores and input/hardware hashes exactly.
SPI/SD and DMA can now run inside experimental blocking IQB fills; C33 counts
callback-advanced time once. It passes 545 CPU/DMA checks, 48 fetch accounting
combinations, 3,853 architectural comparisons and 86 controller checks, with
40/18/2 failures respectively in isolated polling/clock/rounding negative
controls. Both experimental policies pass SD DMA/GUI/watchdog validation.

Mode 2 remains below target: 124/128 transfer, 120/128 solo, 101/120 memory,
71/90 NuttX, 30/40 RISC-V and 328/360 pipeline within 10%. SPI contention RMS
worsens to 13.51482 clocks/pass; separate trials removing fitted DMA holds do
not resolve it. No experimental settings are promoted. AHB arbitration
granularity is still a hypothesis, and the hook is not a complete machine
event timeline. Complete per-case timing and source identities are in
`build/wr128/word-event-review/continuation.json`. The remaining suite,
hardware prospective and full workflow requirements keep the goal active.

The current experiment's final reader replay matches native pixels and I/O,
with screen medians +2.02--4.01%, 13/27 phase medians within 5% and 21/27
within 10%. All 264 additional bus native cases pass; 240/264 are within 10%.
Neither removes the recorded workflow limitations or broader suite failures.
All validation jobs for this binary have completed. The final record is
`build/wr128/word-event-review/verification.json`.

The next default correctness change moves SDRAM cache invalidation from
future write reservation to accepted RAM effects and handles physical address
aliases. Eighteen real DMA/size/IQB-policy combinations fail 36 checks before
the fix; all 572 destination-deadline checks now pass, alongside 545 CPU/DMA,
87 deferred-controller, 45 accepted-alias and 262,144 fetch-path checks.
Full default and both experimental SD DMA/GUI/watchdog checks pass. The
reader replay also verifies native pixels and I/O. The three separate
default/reference/mode-2 five-suite assessments are complete, with all 738
native cases passing under each setting and identical timing scores/input
identities to their preceding baselines. Evidence is in
`build/wr128/write-commit-review/verification.json`. No calibration defaults
or experimental promotion decision changed. Historical bare-guest reset
results are explicitly excluded from Grifo qualification; new default and
two-policy runs verify three complete reboots each through FLASH/Grifo/init.

The integrated SPI/DMA timeline now orders pending bus phases and wire edges,
including late polling. Both engines sample sources at read completion; IDMA
descriptor writeback follows its payload. The preceding prototype passed all
738 microbenchmark cases under both settings but failed a late-poll receive
test, so it was not promoted. The integrated replacement passes 250 device
checks, seven fault-injection controls, 545 CPU/DMA checks, 572 destination
checks, the full functional suite, both IQB policies' native SD DMA/GUI/real
reboot checks, and reader pixel/I/O verification. Address and undefined-
behavior sanitizers pass the device suite. Both full timing comparisons are
complete, with all 738 native cases passing in each. Reference within-10%
counts are transfer 123/128, solo 120/128, memory 107/120, NuttX 75/90, RISC-V
39/40 and pipeline 329/360. Contention RMS improves, transfer RMS worsens and
memory/NuttX remain below target. The complete evidence is in
`build/wr128/dma-timeline-review/verification.json`. Fitted costs and
experimental promotion decisions are unchanged. These fixes strengthen event
semantics; the remaining timing and whole-workflow gates are still unmet.

An isolated timed-fetch review now covers 27 hypotheses across 15 unchanged
NuttX loop bodies, plus 15 legacy controls: 420 host timing simulations, no
guest execution. Controller requests are chronological and the controls
reproduce executing-loop timings within 1%. No proposed FIFO/branch/response
policy improves the hardware comparison consistently; the best reaches
11/15 within 10%, versus 14/15 for the control. None resolves bcpad4.
`build/wr128/timed-fetch-review/assessment.json` records all assumptions and
results. These hypotheses are rejected, not integrated or scored as CPU
qualification. The working emulator and complete suite metrics are unchanged.

Test cleanup removes the old direct-ELF differential startup/linker fixtures,
including their ABI, compiler-regression and Doom math callers. All now run
through FLASH/Grifo/init. The replacement passes 52 generated-program runs,
four ABI combinations, the compiler reduction and 227,672 exact Doom results;
the canonical DMA timeline target also passes. The earlier bare-program
qualification is retained as historical evidence only. Results and source
identities are in `build/wr128/difftest-grifo-review/verification.json`.

The controlled external-fetch probe in `tools/fetch_bench` is now prepared
for subsequent hardware work. Its 600 cases control code alignment, IQBEN,
ordinary/delayed branches, stream length and data placement. The A0 wrapper
also uses an A0 call stack and explicit code-line writes before timing;
it does not assume that toggling IQBEN empties the buffers. All five frozen
model settings pass the same 600 native cases through FLASH/Grifo/init.
`build/wr128/fetchq/prospective.json` retains their unmodified predictions.
It has no physical report and is not installed over the pending portq probe.
The current binary, timing scores and milestone status remain unchanged.

Both new physical probes have now returned. Portq passes all 560 native
checks. The unchanged eight-halfword declarative forecast reaches 496/560
within 10% (217/280 shared A0, 279/280 separate port); actual executing C33
reaches 489/560 (218/280 shared, 271/280 separate), versus default 208/560.
All 56 matched dependency contrasts are within 0.0003 MCLK/pair. Large
remaining errors concentrate in short shared-port loops.

Fetchq passes all 600 native cases. Its five unchanged forecasts reach
405/406/421/420/344 cases within 10%, respectively for defaults, reference
frontend, deferred word priority, fill drain and demand-only fetching. The
complete comparison keeps code placement, IQBEN and pass count separate.
These new holdouts do not qualify any replacement model or promote one.

The returned fetch header also identifies incorrect readback of read-only
SELDO: the working core reports self-refresh whenever SELEN is set. A proposed
correction passed controller, functional, full-suite and reader checks, but
failed a new deep-suspend test. Grifo programs AURCO=0 before polling SELDO;
the current scheduler interprets zero as continuous auto-refresh and cannot
enter self-refresh. Zero's actual controller semantics remain unverified.
The correction is rejected and preserved under
`build/wr128/self-refresh-status-review`; source and binary were restored to
the previous qualified `8dafbeab` core. This known readback issue remains open.
`make check` now includes `test-suspend`: actual Grifo deep sleep/wake,
register restoration and retained RAM. The baseline completes two suspends
and resumes; the rejected candidate gets stuck polling before sleep.
This is a functional regression check, not a physical timing qualification.

Both physical reports are archived, all five ordinary card apps are verified,
and the card is safely ejected. No hardware request remains pending. Neither
new probe qualifies a replacement timing model or changes the earlier
five-suite accuracy scores. The overall milestone remains incomplete.
