# gcc.c-torture execute: what still fails

Baseline for the next pass. Regenerate with `tests/run-torture.sh execute`.

72 distinct tests, of which 17 fail at every level and 17 at exactly one.

**TIMEOUT means a wild jump, not a slow test.** Checked with the limit
raised 10x: `920501-2` ends at pc=0x00010011 and `20020201-1` at
pc=0x0000c012, both executing in low memory. Treat them as crashes.

**The "looks like" column is a grep, not a diagnosis** -- it reports what
the source mentions, so `float` on a test whose point is something else
is noise. It is there to suggest a grouping to try first, nothing more.

Statuses: `ABORT` is the test's own abort(), i.e. a wrong answer.
`EXITnz` is main returning non-zero, usually the same thing in a test
that does not call abort(). `FAULT` is the emulator trapping. `.` is a
pass at that level.

| test | -O0 | -O1 | -O2 | -Os | looks like |
|---|---|---|---|---|---|
| `20020201-1` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `20021127-1` | ABORT | ABORT | ABORT | ABORT | longlong |
| `920501-2` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `920501-6` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `920501-8` | ABORT | ABORT | ABORT | ABORT | varargs,float |
| `930513-1` | ABORT | ABORT | ABORT | ABORT | float |
| `arith-rand-ll` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `eeprof-1` | ABORT | ABORT | ABORT | ABORT |  |
| `pr50865` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr57131` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr69447` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr78378` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr78791` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr79327` | ABORT | ABORT | ABORT | ABORT |  |
| `pr81556` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr85582-1` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `pr99079` | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | longlong |
| `20030125-1` | . | ABORT | ABORT | ABORT | float |
| `920612-1` | . | ABORT | ABORT | ABORT |  |
| `920711-1` | . | ABORT | ABORT | ABORT |  |
| `pr22493-1` | . | ABORT | ABORT | ABORT |  |
| `pr23047` | . | ABORT | ABORT | ABORT |  |
| `20040409-1w` | . | . | ABORT | ABORT |  |
| `20040409-2w` | . | . | ABORT | ABORT |  |
| `20040409-3w` | . | . | ABORT | ABORT |  |
| `20040805-1` | . | . | ABORT | ABORT |  |
| `20090113-2` | . | . | TIMEOUT | TIMEOUT | struct |
| `20101013-1` | . | . | ABORT | ABORT | longlong |
| `20120105-1` | . | . | TIMEOUT | TIMEOUT | struct |
| `20140425-1` | . | . | EXITnz | EXITnz |  |
| `20170111-1` | . | . | TIMEOUT | TIMEOUT | longlong,struct |
| `20190820-1` | . | . | TIMEOUT | TIMEOUT | longlong,struct |
| `920625-1` | . | . | TIMEOUT | ABORT | varargs,float |
| `930529-1` | . | . | TIMEOUT | TIMEOUT |  |
| `990208-1` | . | . | EXITnz | EXITnz |  |
| `nest-stdar-1` | . | . | FAULT | FAULT | varargs,float |
| `pr103052` | . | . | TIMEOUT | EXITnz |  |
| `pr107879` | . | . | EXITnz | EXITnz | float |
| `pr126405-3` | . | . | EXITnz | EXITnz |  |
| `pr42231` | . | . | TIMEOUT | ABORT |  |
| `pr42269-2` | . | . | EXITnz | EXITnz | longlong |
| `pr43784` | . | . | FAULT | FAULT | struct |
| `pr43835` | . | . | EXITnz | EXITnz | struct |
| `pr51447` | . | . | TIMEOUT | TIMEOUT |  |
| `pr57124` | . | . | ABORT | ABORT |  |
| `pr60072` | . | . | EXITnz | EXITnz |  |
| `pr64718` | . | . | EXITnz | EXITnz |  |
| `pr71494` | . | . | ABORT | ABORT |  |
| `pr83362` | . | . | EXITnz | EXITnz |  |
| `pr84169` | . | . | TIMEOUT | TIMEOUT | longlong |
| `pr85169` | . | . | TIMEOUT | TIMEOUT |  |
| `pr89369` | . | . | TIMEOUT | TIMEOUT | longlong,struct |
| `pr91632` | . | . | EXITnz | EXITnz |  |
| `pr94524-1` | . | . | EXITnz | EXITnz |  |
| `va-arg-11` | . | . | TIMEOUT | TIMEOUT | varargs |
| `20000402-1` | TIMEOUT | . | . | . | longlong |
| `20000523-1` | TIMEOUT | . | . | . | longlong |
| `20020402-2` | . | . | EXITnz | . | struct |
| `20040811-1` | TIMEOUT | . | . | . | alloca |
| `20170401-1` | . | . | . | TIMEOUT | struct |
| `20191023-1` | . | . | EXITnz | . |  |
| `920604-1` | TIMEOUT | . | . | . | longlong |
| `divconst-3` | TIMEOUT | . | . | . | longlong |
| `loop-13` | . | . | . | TIMEOUT |  |
| `pr30185` | TIMEOUT | . | . | . | longlong,struct |
| `pr33669` | TIMEOUT | . | . | . | longlong,struct |
| `pr37573` | . | . | . | ABORT | struct |
| `pr42721` | TIMEOUT | . | . | . | longlong |
| `pr43220` | TIMEOUT | . | . | . |  |
| `pr47337` | TIMEOUT | . | . | . | longlong |
| `pr49419` | . | . | . | TIMEOUT | struct |
| `vla-dealloc-1` | TIMEOUT | . | . | . | alloca |
