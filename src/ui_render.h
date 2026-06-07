// ============================================================================
// ui_render.h — Blend2D-backed offscreen vector rendering for premium panels
//
// Renders analytic-AA vector graphics (gradients, glow, soft depth, smooth
// curves) into a 32-bit offscreen buffer, then BitBlts into the target HDC -
// reusing the cross-platform path proven by spectral_view.cpp (Win32
// CreateDIBSection / SWELL SWELL_CreateMemContext + SWELL_GetCtxFrameBuffer).
//
// No Blend2D types leak into this header (kept in ui_render.cpp) so including it
// stays cheap and the strict project warning flags don't hit Blend2D headers.
// Design tokens + spec: ui_theme.h, .harness/design_dynamics_panel.md.
// ============================================================================
#pragma once

#include "platform.h"
#include <cstdint>

// Inputs for the transfer-curve hero. Plain values (no Blend2D in the header);
// the compression math mirrors dynamics_engine.cpp ComputeCompression().
struct DynCurveParams {
  double thresholdDb  = -24.0;
  double ratio        = 3.5;
  double kneeDb       = 6.0;
  double gateThreshDb = -55.0;
  double gateRangeDb  = 24.0;
  double avgPeakDb    = -18.0;  // operating point (signal level); < inMinDb hides it
  double inMinDb      = -60.0;  // plot range (square: out uses the same range)
  double inMaxDb      = 0.0;
  bool   showGate     = true;
};

class UiCanvas {
public:
  UiCanvas() = default;
  ~UiCanvas();
  UiCanvas(const UiCanvas&) = delete;
  UiCanvas& operator=(const UiCanvas&) = delete;

  // Render the premium transfer-curve hero into an offscreen Blend2D buffer,
  // then BitBlt to hdc at (x, y), sized w x h logical px. The render runs at
  // device pixels (w*dpr x h*dpr) with a dpr scale so it is crisp on HiDPI.
  void RenderTransferCurve(HDC hdc, int x, int y, int w, int h, double dpr,
                           const DynCurveParams& p);

private:
  bool ensure(HDC hdc, int devW, int devH);   // (re)create offscreen surface

  HDC m_memDC = nullptr;
#ifdef _WIN32
  HBITMAP m_memBmp = nullptr;
#endif
  int m_w = 0;
  int m_h = 0;
};
