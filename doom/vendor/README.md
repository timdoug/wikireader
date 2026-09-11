# PureDOOM

Source: https://github.com/Daivuk/PureDOOM
Revision: `355cfbd16fac119718879239336ee2ea408886bd`.

`PureDOOM.h` and `LICENSE` are vendored from that revision. Upstream's notices
are retained. No WAD assets are included in this directory or committed by
this port; the shareware WAD in the upstream repository was used for testing.

Local changes, marked in the header or guarded by `DOOM_NO_SOUND`:

- Fixed-point multiplication has an exact target hook; WikiReader uses the
  C33 signed 32-by-32-bit multiply and extracts product bits 16 through 47.
- Wall and masked-wall texture scales use an exact unsigned reciprocal
  helper in A0 RAM, with bounded normalization and restoring division.
- The main wall loop runs in A0 too, with indirect calls between it and
  SDRAM texture lookup/setup code.
- Fixed-point division uses an exact 48-by-32-bit restoring divide, with
  unsigned remainder arithmetic and signed overflow checks. Its inner loop
  runs in A0 RAM on C33, avoiding generic 64-bit software division.
- Sprite-name comparison reads possibly unaligned strings with `doom_memcpy`.
- Low-detail floor spans count pixel pairs, preventing writes beyond the span.
- Low-detail columns/spans preserve the caller's logical X coordinates.
- Low-detail columns/spans snapshot their source pointers and texture steps
  and write identical pixel pairs with aligned halfword stores. This avoids
  redundant SDRAM reads and writes without changing texture sampling.
- Fuzz and translated sprite columns respect low-detail pixel pairs.
- Low-detail draw bounds use the logical half-width and an indirect error call,
  allowing the drawing loops to run outside SDRAM's short-call range.
- Floor span generation and mapping also run in A0, reached through an
  indirect call. Their bounds error path uses the same call convention.
- Low-detail draw loops can cache the current immutable 256-byte light table.
  WikiReader keeps separate column/span entries in A0 and resets them when
  initializing the engine. Palette effects do not modify these WAD tables.
- Floor/ceiling rendering has an optional flat-texture cache hook. The WAD
  allocation is retained separately so zone tags apply to the original data.
- Retail WAD filename allocation includes all nine characters in `doomu.wad`,
  fixing a one-byte heap overwrite exposed by the sanitized replay check.
- Sprite initialization reads only the eight-byte patch headers through a
  bounded `W_ReadLumpPrefix` helper. Images are loaded by normal level
  precaching or on demand; negative sprite offsets use defined multiplication.
- `DOOM_NO_SOUND` skips sound precaching, playback, position updates, music
  and the unused volume mixing table's 32,768 software divisions.
- Optional boot and coarse BSP/plane/masked-render timing hooks feed the
  target's persistent performance log; host builds compile them out.
  A benchmark-only defaults hook fixes the view size and detail setting.

Platform-specific code and callbacks are in the parent directory. The drawing
functions and fixed-point divider are assigned to internal RAM by declarations
in `engine.c`; the reciprocal lives in `c33_math.c`. That file also supplies
the application's `__udivmodsi4` ABI bridge, allowing libgcc's existing signed
and unsigned division/remainder entry points to use an exact A0 divider.
