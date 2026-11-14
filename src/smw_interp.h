/* Render-side motion interpolation for 120 Hz+ displays (see
 * docs/motion-interp.md). Game logic stays at 60 Hz; only presentation
 * gains in-between frames, built by re-rasterizing captured PPU state with
 * interpolated BG scroll and OAM positions.
 *
 * Reimplemented for this tree from SuperRyuWorld1-Recomp's srw1_interp.c
 * (MIT); the capture/render core is the same algorithm retargeted onto the
 * current Ppu layout. The presenter is new: time-based (alpha from the
 * wall clock per display refresh) instead of SRW1's fixed subframe count. */
#ifndef SMW_INTERP_H
#define SMW_INTERP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  kSmwMotion_Off = 0,          /* untouched stock path, no capture */
  kSmwMotion_Interpolate = 1,  /* in-betweens sweep prev->cur (up to 1 tick delay) */
  kSmwMotion_Extrapolate = 2,  /* low latency: cur first, then projected forward */
} SmwMotionMode;

/* Accepts "0"/"1"/"2" and the names below, case-insensitively. */
bool SmwMotionMode_Parse(const char *value, SmwMotionMode *out);
const char *SmwMotionMode_Name(SmwMotionMode mode);

SmwMotionMode SmwInterp_GetMode(void);
void SmwInterp_SetMode(SmwMotionMode mode);

/* Called by the live frame raster before each ppu_runLine(line). Cheap
 * no-op unless a mode is active. */
void SmwInterp_CaptureLine(struct Ppu *ppu, int line);
/* Called after the live raster finishes the frame. */
void SmwInterp_EndFrame(void);

/* f = signed fraction of the last tick's motion to apply relative to the
 * newest captured frame: -1..0 sweeps prev->cur (interpolate), 0..1
 * projects cur forward (extrapolate). Writes a 256x224 XRGB frame with a
 * 1024-byte pitch into `out`. False = not available (caller presents the
 * live frame instead). */
bool SmwInterp_RenderSubframe(float f, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif