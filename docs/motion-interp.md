# 120 Hz+ motion interpolation (render-side)

**Status: Phase 1 (vanilla, 256-wide) implemented, awaiting build/test with
the owner's ROM.** Origin: agent brief `smw-recomp-agent-brief.md`,
Phase 0/1. See the brief for the phase gates.

What this adds: high-refresh-display motion smoothness for the vanilla
game. The simulation stays at 60 Hz; presentation gains in-between
frames re-rasterized from captured PPU state.

## Where the code is

| Piece | File |
|---|---|
| Interpolator (capture + subframe render) | `src/smw_interp.c`, `src/smw_interp.h` |
| Capture hooks (live raster) | `src/smw_rtl.c` (`SmwInterp_CaptureLine`, `SmwInterp_EndFrame`) |
| Time-based presenter + gates | `src/main.c` (`SmwInterpDrawFrame`, `SmwInterpUsable`, `SmwInterpPresent`) |
| Config | `config.ini` `[Graphics] MotionInterp`, env `SMW_MOTION_INTERP` |

## Provenance

* **`src/smw_interp.c` capture/render core**: reimplementation of
  SuperRyuWorld1-Recomp's `overlay/src/srw1_interp.c` (MIT), retargeted
  onto this tree's `Ppu` (which evolved past SRW1's pin):
  * `mode7Hd` no longer exists; the scratch copy NULLs this tree's
    equivalents (`hdFrame`, `hdCapture`) instead, plus `extraObjects`/
    `extraObjectCount` and the overlay render buffers.
  * Side-bar art (`Srw1Interp_RenderSideArt`) is **not ported** (per the
    brief: obsolete with real widescreen; Phase 2/3 owns margins).
  * The register-prefix capture (`kRegBytes = offsetof(Ppu, vramPointer)`)
    is unchanged: this tree's `ppu_register_fields.inc` declares the same
    `inidisp..cgwsel` prefix, and `ppu.h` static-asserts the 0x40 size.
    *(Verified against `snesrecomp/runner/src/snes/ppu.h` /
    `ppu_register_fields.inc`.)*
* **Presenter is new, per the brief's design requirement**: SRW1 presents
  `n = round(refresh/60)` subframes per tick paced by a timer; at 170 Hz
  that is n=3 (180 presents/s against a 170 Hz vsync: beating and dropped
  frames). This tree instead computes, for every display refresh,
  `alpha = (now - tick_start) / tick_period` and renders at the matching
  fraction. Interpolate maps that to `f = alpha - 1` (sweep prev->cur),
  Extrapolate to `f = alpha` (project past cur). *(Design as briefed;
  smoothness at 170 Hz not yet measured.)*

## How the presenter paces (verified reasoning, unmeasured)

* Both output backends block on vsync: `SdlRenderer_EndDraw` →
  `SDL_RenderPresent` on a renderer created with the vsync flag
  (`snesrecomp_sdl_create_renderer(..., g_benchmark_frames == 0)`), and
  `OpenGLRenderer_EndDraw` → `SDL_GL_SwapWindow` after
  `SDL_GL_SetSwapInterval(1)`. So one `EndDraw` = one display refresh,
  and a loop of presents emits exactly one frame per refresh at the
  elapsed fraction of the 60 Hz tick. *(Verified in `src/main.c` /
  `src/opengl.c`.)*
* The loop stops when the next present could complete after the 60 Hz
  deadline (would slip the tick and drift SPC/MSU-1 audio): estimate =
  measured vsync-to-vsync interval + measured subframe raster cost, both
  EMAs. Conservative by construction.
* A present that did not block (compositor vsync off behind our back,
  software renderer) makes the measured interval collapse below 4 ms
  (240 Hz = 4.17 ms passes); the loop then degrades to one present per
  tick — no busy spin. `SmwInterpUsable()` also refuses benchmarks
  (`g_benchmark_frames`, created with vsync off) and netplay.
* 60 Hz display: after the live present the remaining tick is less than
  one refresh, so no in-between frames fit; Interpolate falls back to
  presenting the live frame (zero added latency there), Extrapolate shows
  live then nothing extra. No interpolation claims at 60 Hz.
* Interpolate deliberately does **not** present the live frame before the
  sweep (only as fallback): that would jump one frame ahead and back
  every tick. Extrapolate presents live first (it *is* f=0).

## Modes

| Mode | Latency | Behavior |
|---|---|---|
| `off` (default) | stock | Untouched path: capture hooks no-op; call site takes `DrawPpuFrameWithPerf`. |
| `interpolate` | + up to 1 tick | Sweeps prev->cur across the tick (per-line scroll, per-slot OAM). |
| `extrapolate` | none | Live frame first, then projected forward. |

Config: `[Graphics] MotionInterp = off|interpolate|extrapolate` (also
`0|1|2`). Env override for QA: `SMW_INTERP_DEBUG=1` prints a pacing line
(interval, raster cost, presents/tick) once per 60 ticks.

## Known limits (by design, matching SRW1)

* Mode 7 lines are rendered from captured `m7matrix` per line but the
  matrix itself is not interpolated (map screen / Bowser fight gain no
  smoothness there; no artifacts either).
* Sprite matching is by slot, then nearest identical tile within 16 px;
  spawns/teleports stay put (no false motion).
* Blanked screens (`inidisp & 0x80`), BG mode changes and non-consecutive
  captures fall back to the live frame.
* VRAM/OAM are captured whole-frame (at line 0), not per line — matches
  SRW1; mid-frame OAM rewrites are not interpolated.

## Phase 1 acceptance checklist (pending ROM + owner visual test)

- [ ] `Off`: bit-identical frames vs baseline (`FrameDump_*` +
      `SNESRECOMP_FRAMEDUMP_PIXELS=1`), unchanged simulation.
- [ ] Interpolate + Extrapolate visibly smooth at 120 and 170 Hz.
- [ ] Scene cuts, level transitions, blanked screens fall back live.
- [ ] Scrolling levels, parallax, HDMA waves, status-bar split, Yoshi,
      sprite spawns, pause, overworld, title screen.
- [ ] Audio stays in sync (pacing never slips a tick).

## Phase 2+ hooks (for the wide-snes spike)

* The interpolator is written against `g_snes_width == 256`
  (`SmwInterpUsable` gate) with hardcoded 1024-byte pitch and buffers;
  Phase 3 parametrizes by extra width: `PpuSetExtraSpace` on the scratch,
  `(256 + 2*extra) * 4` pitch/buffers, drop the gate, adjust
  `SmwInterpPresent`'s copies, and re-check OAM wrap (`wrap_delta`
  assumes 512/256) and the right-margin 9-bit X decode
  (`wsOamRightHintStrict` in this tree).
* `src/smw_interp.c` already NULLs the host-only hooks on the scratch
  copy (HD capture/frame, extra objects, overlays, line enhancer), which
  is what a widened scratch render also needs.