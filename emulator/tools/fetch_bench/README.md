# Controlled external instruction fetching

This probe separates instruction-queue replacement from code alignment,
external data traffic and branch restart. It is prepared for the physical
reader after the local-port probe. Both reports have now returned. Fetchq's
600 native checks and installed identities pass; its raw physical report is
archived in `device.json`. The card is safely ejected with the five ordinary
apps preserved. The initial prospective predictions remain unchanged.

The 600 cases comprise 576 controlled SDRAM-code cases and 24 A0 controls:

- Register, fixed-address load/store and streaming byte-copy loops, with
  10-, 18- and 26-byte bodies. The stream cases include the problematic
  18-byte byte-copy shape at all eight halfword offsets.
- IQBEN enabled and disabled, preserving all other controller settings.
- Ordinary and delayed conditional branches. The ordinary ending is
  `sub; cmp; jrne`; the delayed ending is `sub; jrne.d; nop`. Both occupy
  three halfwords, and only a documented one-cycle NOP enters a delay slot.
- One, eight and 8,192 passes, three raw repetitions each. The A0 controls
  use eight and 8,192 passes.

All templates have an identical 16-byte register setup. They are copied to
owned storage, with the loop address explicitly recorded. SDRAM code never
crosses a 1-KiB row; external data occupies a separate 4-MiB bank. Source and
destination streams occupy different rows of the same bank. Whole-result
checksums observe loaded values and accumulated work. Every repetition checks
all used source/destination bytes, surrounding guards and the local stack.

The measuring wrapper, parameters and temporary call stack reside in owned
A0 RAM. This matters because a normal call would push its return address to
the external stack, introducing an unintended SDRAM transaction. Direct
high/low/high reads use Grifo's existing concatenated timer configuration.
Interrupts are disabled for the interval. There is no syscall or external
stack access between the timer reads.

Before timing, A0 code sets IQBEN, rewrites an unchanged halfword in each of
eight copied-code lines, and reads back the final location. The E07 manual,
II.4.2.4, specifies buffer invalidation on a matching data write. It does not
promise that toggling IQBEN empties the buffers, so the probe does not rely
on that assumption. The initial toggle-only prototype is archived under
`build/wr128/fetchq/initial-toggle-build` and excluded from the final forecasts.

The interval includes call/return, register setup and the timer boundary.
Raw whole-interval ticks are the accuracy metric. Short runs expose transient
costs; no fitted constant is subtracted to improve agreement.

```sh
python3 emulator/tools/fetch_bench/build.py
python3 emulator/tools/fetch_bench/run.py --out build/wr128/fetchq/my-run
python3 -m unittest discover -s emulator/tools/fetch_bench -p 'test_*.py'
```

The builder verifies every template halfword, branch displacement, legal
delay slot, size and alignment. It also checks the wrapper's stack switch,
restoration and lack of intervening external stack traffic against emitted
machine code. The runner snapshots the emulator and boots the real FLASH,
Grifo kernel and launcher. It verifies all 600 native results, addresses,
timer/controller context, complete coverage and poweroff. Seven host tests
exercise evidence validation, context matching, failed-return preservation,
and the complete installation transition using temporary files.

Five completed native runs use the identical application and core:

| Run under `build/wr128/fetchq` | Model |
| --- | --- |
| `coherent-default-v2` | Current defaults |
| `coherent-frontend-v2` | Optional local frontend reference |
| `coherent-word-priority` | Reference plus deferred IQB word priority |
| `coherent-fill-drain` | Reference plus started-fill drain before CPU data misses |
| `coherent-demand-only` | Reference with static IQB lookahead disabled |

All 3,000 native cases pass. These validate the measurement program and
freeze competing predictions; silicon comparisons are recorded below.
`build/wr128/fetchq/prospective.json` preserves all five settings, identities
and 9,000 raw samples before hardware collection. The freeze tool refuses
overwrites. Against the reference, the two deferred policies differ by over
10% in 34 and 36 cases, and disabling lookahead does so in 64 cases. All
models give identical paired IQBEN-on/off ticks for the A0 controls.

The installer requires the current portq physical report to have been
collected, archived and verified. It then backs up the replaced files and
six-entry menu, verifies the new copies and all five ordinary applications,
replaces only the final probe entry, removes backed-up obsolete probe files,
and ejects the card. This installation and return are complete, using only
ordinary mounted files. `build/wr128/fetchq/post-install-verification.json`
records the verified remount; `return-eject-verification.json` records the
archived return and final ejection. No further card operation is pending.

The collector discovers ordinary mounted files and archives raw failed
returns before validation, invalidating the latest pointer on failure.
The analyzer requires matching firmware, application, addresses and clocks,
and reports short/long, IQBEN and code-placement groups separately.

```sh
python3 emulator/tools/fetch_bench/collect.py
python3 emulator/tools/fetch_bench/analyze.py --out build/wr128/fetchq/hardware-comparison.json
```

## Returned hardware comparison

The physical report is `device-2026-11-05-ad2e63fb47d8.txt`. All 600 cases,
native results, guards, timers, addresses and installed inputs pass. The
frozen core reports `REF=03ff01c0`, versus hardware `01ff01c0`. The only
difference is read-only SELDO (D25), not a writable configuration bit. The
old core incorrectly asserted it whenever SELEN was enabled. The analyzer
retains both raw contexts and status values, while requiring every actual
clock/controller setting to match; changing a refresh interval still fails.

Of the five unchanged forecasts, defaults reach 405/600 within 10%, the
local-frontend reference 406/600, deferred word priority 421/600, fill drain
420/600, and demand-only fetch 344/600. Short and long runs remain separate
in `build/wr128/fetchq/hardware-comparison.json`; the numerous short cases
must not hide the larger long-loop misses. None qualifies a replacement
external fetch model. A proposed readback correction was rejected after it
broke real Grifo deep suspend: the existing auto-refresh scheduler cannot
enter self-refresh with the kernel's zero AURCO setting. The working core
was restored; the status discrepancy remains known and explicitly recorded.
The candidate and negative control are preserved separately in
`build/wr128/self-refresh-status-review`. `make -C emulator test-suspend`
now guards the real sleep/wake path; it does not measure physical wake timing.

External fetch admission/cancellation, full reader workflows and the overall
accuracy milestone remain open. Neither collected report needs to be rerun.
