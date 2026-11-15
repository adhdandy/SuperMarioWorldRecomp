# 120 Hz+ motion interpolation (render-side)

**Status: Phase 1 (vanilla, 256-wide) implemented and machine-verified on
the target machine (2560x1440 @ 170 Hz).** Origin: agent brief
`smw-recomp-agent-brief.md`, Phase 0/1. Scene-by-scene visual acceptance
and listening are the owner's (see checklist). See the brief for the phase
gates.

Pacing/audio follow-up (2026-11-15): the first presenter spent a whole 60 Hz
period on presents *after* the guest frame had already run, so ticks took
17-18 ms (55-58 Hz) and music slowed and starved; and the refresh estimate
was an EMA over measured present gaps, which one hitch could push past the
tick period permanently, silently dropping interpolation to 60 fps until
restart (observed after level exits). Both are fixed below; the section
[Measured](#measured-this-machine) records the numbers.

What this adds: high-refresh-display motion smoothness for the vanilla
game. The simulation stays at 60 Hz; presentation gains in-between
frames re-rasterized from captured PPU state.

## Where the code is

| Piece | File |
|---|---|
| Interpolator (capture + subframe render) | `src/smw_interp.c`, `src/smw_interp.h` |
| Capture hooks (live raster) | `src/smw_rtl.c` (`SmwInterp_CaptureLine`, `SmwInterp_EndFrame`) |
| Time-based presenter + gates | `src/main.c` (`SmwInterpDrawFrame`, `SmwInterpRefreshInterval`, `SmwInterpPresentTracked`, `SmwInterpUsable`) |
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
  Extrapolate to `f = alpha` (project past cur). *(Engagement and pacing
  measured below; visual smoothness is the owner's check.)*

## How the presenter paces (verified in code; measured below)

* Both output backends block on vsync: `SdlRenderer_EndDraw` →
  `SDL_RenderPresent` on a renderer created with the vsync flag
  (`snesrecomp_sdl_create_renderer(..., g_benchmark_frames == 0)`), and
  `OpenGLRenderer_EndDraw` → `SDL_GL_SwapWindow` after
  `SDL_GL_SetSwapInterval(1)`. So one `EndDraw` = one display refresh,
  and a loop of presents emits one frame per refresh at the elapsed
  fraction of the 60 Hz tick. *(Verified in `src/main.c` /
  `src/opengl.c`.)*
* **The tick budget starts at the top of the host frame**
  (`SmwInterpDrawFrame(frame_start)` receives the timestamp taken before
  `RtlRunFrame`). The guest frame and the live raster have already spent
  part of the 16.67 ms by the time the presenter runs; budgeting a fresh
  period there made every tick 17-18 ms. The SPC produces exactly 534
  native samples per emulated frame, so a slow tick starves the device:
  that was the audible slow/stuttering music.
* **Present admission uses a remembered vblank grid, not a worst case.**
  A present submitted now rasterizes first, then shows at the first
  vblank at or after `now + raster_cost`. `g_smw_interp_vblank` is
  re-anchored only by presents that actually blocked on the compositor
  (waited ≥ half a refresh); a free present was queued for a later
  vblank and must not move the grid. The loop stops when the projected
  vblank would pass the 60 Hz deadline, so a present can never slip the
  tick. Cap: 4 presents/tick (covers 240 Hz and stops a compositor that
  stops blocking from turning this into a burst).
* **The refresh interval comes from the display mode**
  (`SDL_GetCurrentDisplayMode().refresh_rate`), re-queried every 120
  ticks so a mode change is followed. Measured present gaps are *not*
  used: they are sampled inside one tick, so a single compositor or
  scheduler hitch (a level exit loading assets, a page fault, a driver
  wait) could enlarge the estimate past the tick period. Every later
  tick then failed the fit test, and because a skipped present is never
  measured the estimate could never recover — interpolation stayed at
  60 fps until restart. That was the intermittent "interp stops after
  exiting a level" report.
* No separate "raster slower than a refresh" gate: the projected vblank
  already pushes `shown_at` past the deadline in that case, and skipping
  the render would freeze the raster-cost EMA (the same latch class).
* 60 Hz display: after the live present the remaining tick is less than
  one refresh, so no in-between frames fit; Interpolate falls back to
  presenting the live frame (zero added latency there), Extrapolate shows
  live then nothing extra. No interpolation claims at 60 Hz.
* Interpolate deliberately does **not** present the live frame before the
  sweep (only as fallback): that would jump one frame ahead and back
  every tick. Extrapolate presents live first (it *is* f=0).
* Fast-forward (`g_turbo`) bypasses the presenter: capping turbo at the
  display rate would defeat it.

### Measured (this machine)

`SMW_INTERP_DEBUG=1`, windowed 2560x1440 @ 170 Hz, MSVC/CMake Release,
75 s of attract mode (one line per 60 ticks; `ring` is the APU native
sample ring, `underflows`/`dropped` are device-side counters):

| Build | ticks/s | presents/tick | APU ring | underflows |
|---|---|---|---|---|
| stock path (`MotionInterp=off`) | 60.015 | 1 (present) | 1.7k-3.3k | 0 |
| first presenter (fixed) | 55.15 | 2.9 | dips to 25 | 12-17 |
| this presenter | 60.00 (59.95-60.03) | 2.5-2.6 | ~1.65k, stable | 0 |

Ideal at 170 Hz is 170/60 = 2.83 presents/tick. The remainder is
structural: the guest frame (~1.3 ms) and the live raster run before the
first present, so one vblank per tick can fall inside the simulation and
show the previous frame. Audio is unaffected because every present is
budgeted against the top-of-frame deadline.

## Modes

| Mode | Latency | Behavior |
|---|---|---|
| `off` (default) | stock | Untouched path: capture hooks no-op; call site takes `DrawPpuFrameWithPerf`. |
| `interpolate` | + up to 1 tick | Sweeps prev->cur across the tick (per-line scroll, per-slot OAM). |
| `extrapolate` | none | Live frame first, then projected forward. |

Config: `[Graphics] MotionInterp = off|interpolate|extrapolate` (also
`0|1|2`). Env override for QA: `SMW_MOTION_INTERP` forces the mode;
`SMW_INTERP_DEBUG=1` prints a pacing line once per 60 ticks: mode,
display interval, subframe raster cost, cumulative presents/tick,
presents in the last tick, measured tick rate (`ticks_hz`), APU ring
occupancy and cumulative `underflows`/`dropped`.

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

## Phase 1 acceptance checklist

Machine-verified (this machine, 2560x1440 @ 170 Hz, MSVC/CMake Release,
`--benchmark 600` framedumps: BMP + WRAM + VRAM + CGRAM + PPU JSON per
frame; `Temp/compare_baseline.py` compares all five artifacts):

- [x] `Off`: **3000/3000 files byte-identical** vs stock `main` baseline
      (600 frames x 5 artifacts). *(Verified 2026-11-15, feat/motion-interp
      vs main @ ff4d20f; re-verified after the pacing fix: 3600/3600 including
      the per-frame `.json`.)*
- [x] `Interpolate` with capture active: **3600/3600 byte-identical** vs
      baseline in headless mode — the per-line capture hooks do not perturb
      the simulation or the presented pixels. *(Re-verified 2026-11-15 with
      the final presenter.)*
- [x] Presenter engages in windowed mode at 170 Hz: interval 5.88 ms from
      the display mode, subframe raster 0.47-0.57 ms, presents/tick 2.5-2.6
      of the 2.83 ideal (the rest is the guest frame + live raster at the
      start of each tick). Both interpolate and extrapolate modes. No beat:
      each present blocks one refresh and alpha advances by the wall clock.
- [x] Tick pacing does not slip and audio does not starve while
      interpolating: 60.00 ticks/s (59.95-60.03), APU ring ~1.65k native
      samples and stable, 0 underflows, 0 dropped over a 75 s run.
      *(Machine-verified 2026-11-15.)*
- [ ] Interpolate + Extrapolate **visibly** smooth at 120 and 170 Hz
      (owner's eyes — attract mode + gameplay).
- [ ] Scene cuts, level transitions, blanked screens fall back live.
- [ ] Scrolling levels, parallax, HDMA waves, status-bar split, Yoshi,
      sprite spawns, pause, overworld, title screen.
- [ ] Interpolation survives level exits over a real play session (the
      display-mode interval removed the measured-gap latch; needs owner
      confirmation).
- [ ] Music sounds correct (tempo/pitch) over a real play session and
      listening session.

Baseline/run configs: `build\Release\mods\preloaded\state.toml` with the
widescreen feature `enabled = false` (vanilla 4:3; the interp gate excludes
the adaptive renderer by design). QA env: `SMW_MOTION_INTERP`,
`SMW_INTERP_DEBUG=1`.

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