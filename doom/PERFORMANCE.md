# Doom performance

## Loading and lighting-cache follow-up

The follow-up build keeps the existing image quality and improves modeled
startup and FPS. **It has not yet been measured on hardware.** The physical
14.73 fps result below belongs to the preceding build.

| Measurement | Committed baseline | Follow-up | Change |
| --- | ---: | ---: | ---: |
| Full boot to title frame | 6.266 s | 4.386 s | 30.0% less time |
| Full boot to E1M1 frame, ordinary `-warp` | 7.562 s | 6.874 s | 9.1% less time |
| App entry to E1M1 frame, `-wrbench` | 5.784 s | 5.083 s | 12.1% less time |
| Controlled E1M1 benchmark | 16.258 fps | 16.997 fps | 4.5% more FPS |
| Ordinary E1M1 opening, guest seconds 30-40 | 16.0 fps | 16.7 fps | 4.4% more FPS |
| Controlled engine time/frame | 54.405 ms | 51.730 ms | 4.9% less time |
| Controlled BSP/walls/frame | 26.335 ms | 25.674 ms | 2.5% less time |
| Controlled floors/ceilings/frame | 19.817 ms | 18.042 ms | 9.0% less time |
| Controlled LCD/frame | 7.028 ms | 7.028 ms | unchanged |

The full-boot and app-entry timings have different starting points. Title boot
also omits the level precache. Neither first-frame time includes waiting for
the level melt to finish. The stationary tests keep the same view/settings;
ordinary play polls input while the controlled benchmark locks input. The
controlled follow-up records 170 frames in 10.001862 s, with no SD reads, input,
menu frames or wipes during measurement.

Wall texture initialization now caches only patch headers and column offsets,
then frees that metadata after building texture lookups. It reads **252 sectors
instead of 1,610** during texture initialization. Level precaching still loads
the actual images it needs: total app-startup reads fall from **3,221 to 2,627
sectors**, a smaller saving than the texture stage alone. Startup diagnostics
are quiet by default (`-wrverbose` restores them), while fatal errors always
print. The emulator transmits serial immediately, so additional savings from
avoiding the physical serial wait remain unmeasured.

Lighting-cache misses now copy aligned WAD tables four words at a time, with
a byte fallback for unaligned sources. Both wall and floor draw loops benefit.
The code and tables use 4,908 bytes of A0, leaving 148 bytes. Texture sampling,
viewport, dithering and LCD conversion are unchanged. A trial of wider floor
writes produced essentially the same FPS and was discarded.

`-wrbench` buffers boot, warmup and benchmark records until measurement ends,
then writes/closes one batch and disables tracing. This moves the first-file
creation stall out of startup/warmup and removes periodic logging stalls during
subsequent play. The completion write can still stall briefly. An interrupted
benchmark has no persisted record. `-wrbench -wrtrace` retains five-second play
windows; ordinary `-wrtrace` or `doomlog.on` still traces boot and play. The
marker alone does not keep tracing enabled after an automatic benchmark.

DEMO1's movement/combat spot check reports 19.2 fps versus 17.8 in the preceding
build, with shots and only world frames in the window. Faster startup changes
which demo tics land in absolute guest seconds 30-40, so this is not an isolated
renderer speedup comparison. Use the controlled or stationary figures above.

### Follow-up validation and artifacts

The final source build ID is `0f7d0ef062d701d6`. A build manifest, saved sources,
app/map/disassembly, host test/build logs and a fresh playable emulator card are
in [`build/doom/next-after`](../build/doom/next-after). The final matching
persisted benchmark is in
[`trace-g8k_5mn9`](../build/doom/trace-g8k_5mn9); the preceding controlled baseline
is [`trace-a551fp4x`](../build/doom/trace-a551fp4x).

Address/undefined-behavior sanitizer checks cover adapters, renderer metadata,
random lighting/spans and logger persistence/failures. The 1,500-frame DEMO1
replay matches indexed pixels, palettes and player state exactly. Full-device
emulation passes title/menu startup, movement/firing and FAT save/load, with no
alignment faults or watchdog timeouts. A final comment correction changed the
source fingerprint; `.text` and `.fastcode` are byte-identical to the build used
for the behavioral tests and boot timings, as recorded in `validation.json`.
The final app has its own persisted benchmark and binary hashes.

| Check | Retained artifacts |
| --- | --- |
| Title boot, before / after | `boot-kb79r7ww` / `boot-exd8nwtu` |
| Direct level boot, before / after | `boot-h1o3mxz0` / `boot-lkyjznui` |
| Ordinary stationary FPS, before / after | `benchmark-ghohg21x` / `benchmark-2svns7tk` |
| Replay | `replay-5e_4zcs3` |
| Title, gameplay and save/load | `smoke-c7jpz8gx` |

All artifact directories are under `build/doom`. The original hardware logs and
reference remain unchanged. No kernel, clock, SDRAM timing or device FLASH
changes are part of this pass.

## First physical WikiReader measurements

Captured on 2026-09-10 (local time). The original card logs and a machine-readable
comparison are archived in
[`build/doom/hardware-run-20260910-210809`](../build/doom/hardware-run-20260910-210809).
The app, IWAD, kernel and launcher SHA256 hashes match the installed emulator
reference. Source build ID: `60dd31e23e3df5bc`.

This is one physical run, with a completed stationary benchmark followed by
about 75 seconds of logged play. It establishes a hardware baseline, not a
statistical confidence interval or a measurement of previous port versions.

### Controlled comparison

Both runs use shareware E1M1, skill 2, low detail, a 160x168 logical world view,
the same player position/angle/health, a five-second warmup and a ten-second
measurement. Neither benchmark contains input, menu frames, wipes, movement,
SD reads or logging writes. The hardware recorded 148 frames in 10.050192 s;
the emulator recorded 163 in 10.025805 s.

| Measurement | Physical device | Emulator |
| --- | ---: | ---: |
| Displayed frames per second | **14.726** | 16.258 |
| Mean frame interval | 67.907 ms | 61.508 ms |
| Engine time per frame | 59.199 ms | 54.405 ms |
| BSP/walls, within engine | 27.199 ms | 26.335 ms |
| Floors/ceilings, within engine | 21.930 ms | 19.817 ms |
| Masked rendering/sprites, within engine | 2.408 ms | 2.363 ms |
| Other engine work | 7.663 ms | 5.888 ms |
| LCD conversion and frame snapshot | 8.633 ms | 7.028 ms |
| Shortest-longest frame interval | 66.814-70.925 ms | 60.721-64.534 ms |
| Simulation ticks per second | 35.024 | 35.010 |
| App entry to first displayed frame | **4.377 s** | 5.784 s |

Hardware FPS is 9.4% below the model; expressed in the other direction, the model
overpredicts FPS by 10.4%. Wall and masked-render timings are close (3.3% and
1.9% longer on hardware), with larger gaps in floors (10.7%), LCD conversion
(22.8%) and the remaining engine work (30.1%). Those remaining engine costs
include simulation, HUD and work outside the three timed render phases.

Floors and walls together account for about 72% of a hardware frame; LCD
conversion accounts for about 13%. The game clock maintains 35 ticks/second
despite displaying fewer frames.

The later five-second play windows ranged from 13.10 to 20.77 fps. Across all
15 windows, including menu activity and trace stalls, the weighted average was
14.96 fps. These changing scenes are not comparisons with the stationary
emulator benchmark. There were no recorded SD read errors, DMA errors, DMA
timeouts or DMA fallback in the Doom boot/game windows. The last complete
window ends 96.290 seconds after app entry; there is no final EXIT record.

### Startup and tracing costs

The physical app reaches its first frame 24.3% sooner than the model. These
times exclude the factory FLASH/kernel load and time choosing the launcher
icon. Summed SD read time during app startup is 2.331 s on hardware versus
4.532 s in the emulator. For example, texture initialization reads exactly
1,610 sectors in each run, but its read time is 1.123 s versus 2.266 s. The
model substantially overestimates this card's read latency.

The existing card also has more directory entries than the synthetic fixture:
the early screens/defaults stages read 112/14 sectors versus 16/2. This is a
filesystem workload difference as well as a timing-model difference.

Tracing itself is visible. The first log creation/write/close takes **712.588
ms**, after the first displayed frame and before the warmup. Later flushes take
**40.076-43.700 ms**, approximately once every five seconds. Their mean-FPS
effect is small, but they contribute to the observed roughly 110-120 ms frame
intervals during otherwise steady play. The measured benchmark excludes these
writes. Reducing flush frequency or disabling ordinary play logging after
measurement would remove most of those periodic stalls.

Startup console output is another opportunity. The matching emulator run emits
1,188 console bytes between the startup message and readiness. Grifo's console
is 19,200 baud and waits for the transmit buffer, corresponding to about 0.62 s
of wire time at 10 bits per byte. The emulator completes TX immediately. Quiet
startup could therefore save additional time, but its actual benefit needs a
separate hardware measurement; wire time is not a measured saving.

### Limits of the current emulator reference

The clock and programmed SDRAM timing/refresh intervals match, but the complete
machine setup does not:

| Register | Physical device | Emulator |
| --- | --- | --- |
| Clock | `0a7b050e` | `0a7b050e` |
| Clock gates | `3b0c736e` | `3b0c736e` |
| SDRAM control | `00001353` | `00001352` |
| SDRAM refresh | `01ff0120` | `03ff0120` |
| SDRAM application unit | `8000000b` | `8000000b` |

The control-register difference selects a 32 MiB configuration on the device
and 16 MiB in the fixture. Both use the same 2/4/6 SDRAM timing fields and
`0x120` refresh interval. The refresh-register difference is the read-only
SELDO self-refresh status bit, not a different programmed refresh interval:
the current emulator returns that bit whenever self-refresh is enabled.
Do not infer a hardware timing-setting problem from these two raw hex values.

Before adjusting general emulator CPU costs from this result, match the
fixture's memory setup and account for serial/card timing. Then use the phase
breakdown to investigate LCD conversion and floor rendering, where the model
is less accurate. No firmware or timing-model changes were made as part of
collecting and analyzing this run.

### Reproduce the analysis

```sh
python3 doom/trace-report.py \
  build/doom/hardware-run-20260910-210809/doomperf.log \
  --compare build/doom/hardware-run-20260910-210809/doomemu.log
```

Input hashes and the original emulator reference are retained in `doomref.json`;
the verified hardware comparison is in `comparison.json` in that archive.
