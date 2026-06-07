// ============================================================================
// ui_render.h — Blend2D-backed offscreen vector rendering helper
//
// PHASE 0 (premium UI redesign, branch feat/premium-ui): de-risk the renderer.
// Blend2D renders analytic-AA vector graphics (gradients, rounded rects, glow,
// smooth curves) into a 32-bit offscreen buffer, which we then BitBlt into the
// target HDC. This reuses the exact "fill pixel buffer -> BitBlt" path already
// proven cross-platform by spectral_view.cpp (Win32 CreateDIBSection / SWELL
// SWELL_CreateMemContext + SWELL_GetCtxFrameBuffer).
//
// No Blend2D types leak into this header (kept in ui_render.cpp) so including it
// stays cheap and the strict project warning flags don't hit Blend2D headers.
// ============================================================================
#pragma once

#include "platform.h"

class UiCanvas {
public:
  UiCanvas() = default;
  ~UiCanvas();
  UiCanvas(const UiCanvas&) = delete;
  UiCanvas& operator=(const UiCanvas&) = delete;

  // PHASE 0 spike: render a FabFilter-flavoured demo tile (dark vertical
  // gradient, AA rounded-rect body, cyan glow border, smooth orange curve) into
  // the offscreen Blend2D buffer, then BitBlt it to hdc at (x, y) sized w x h.
  // 1:1 blit (logical pixels), mirroring spectral_view. Proves Blend2D compiles,
  // links, JITs and rasterises inside the REAPER process on every platform.
  void RenderSpikeDemo(HDC hdc, int x, int y, int w, int h);

private:
  bool ensure(HDC hdc, int w, int h);   // (re)create offscreen surface of w x h

  HDC m_memDC = nullptr;
#ifdef _WIN32
  HBITMAP m_memBmp = nullptr;
#endif
  int m_w = 0;
  int m_h = 0;
};
