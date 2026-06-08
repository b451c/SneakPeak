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
#include <memory>

// Inputs for the transfer-curve hero. Plain values (no Blend2D in the header);
// the compression math mirrors dynamics_engine.cpp ComputeCompression().
struct DynCurveParams {
  double thresholdDb  = -24.0;
  double ratio        = 3.5;
  double kneeDb       = 6.0;
  double gateThreshDb = -55.0;
  double gateRangeDb  = 24.0;   // POSITIVE magnitude (engine value is negated at the wiring point)
  double makeupDb     = 0.0;    // post-comp makeup; used for the GATE onset (engine gates post-makeup)
  double avgPeakDb    = -18.0;  // operating point (signal level); < inMinDb hides it
  double avgGrDb      = 0.0;    // engine GetAvgGainReduction(), NEGATIVE dB - drives the GR meter value
  double inMinDb      = -60.0;  // plot range (square: out uses the same range)
  double inMaxDb      = 0.0;
  bool   showGate     = true;
};

// Cached Blend2D objects (offscreen image + per-role fonts), owned by UiCanvas.
// Defined in ui_render.cpp so no Blend2D types leak into this header (keeps the
// strict warning flags off the library headers - see the header note above).
struct Gfx;

// One knob's render state (pure data). The panel builds these from its params;
// the renderer draws the arc/fill/indicator/tick from norm, and the text from
// value/unit/precision. Geometry (the cell rect) comes from ComputeDynLayout.
struct KnobVM {
  double value       = 0.0;   // display value (engine units)
  double norm        = 0.0;   // 0..1 within [min,max] -> fill arc + indicator
  double defaultNorm = 0.0;   // 0..1 default position -> ring tick + Cmd-reset target
  const char* label  = nullptr;
  const char* unit   = nullptr;
  int    precision   = 1;
  bool   isGate      = false; // violet fill (gate params) vs amber (compressor)
  bool   showAuto    = false; // Makeup in auto mode -> "<n> auto" readout
};

// View-model handed to UiCanvas::RenderPanel - pure data (no Blend2D, no engine
// types beyond DynCurveParams). DynamicsPanel builds it each paint from its state;
// the renderer is a pure function of it. Geometry comes from ComputeDynLayout
// (shared with hit-testing), not carried here.
struct DynPanelVM {
  DynCurveParams curve;              // transfer plot + GR meter
  int   activeTab   = 0;            // 0=Compressor, 1=Gate, 2=View
  const char* presetName = nullptr; // null -> "Preset"
  KnobVM knobs[10];                  // all 10 params; only on-tab ones get a rect
  // Toggle/state (Inc 5): the View-tab pills + header A/B render from these; the
  // Peak/RMS segmented reads rmsMode. Mirror DynamicsPanel's live members.
  bool  showDyn  = true;
  bool  showEnv  = true;
  bool  showGR   = true;
  bool  liveMode = false;
  bool  bypassed = false;
  bool  rmsMode  = false;
};

// Plain rectangle (logical px, panel-relative); no Blend2D so it can be shared by
// the renderer and the panel's hit-testing.
struct URect {
  double x = 0, y = 0, w = 0, h = 0;
  bool contains(double px, double py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// Computed panel geometry - the SINGLE source of truth for BOTH RenderPanel and
// DynamicsPanel hit-testing (so draw and click can never drift). Panel-relative px.
struct DynLayout {
  URect header, footer, plotWell, grMeter;
  URect preset, abBtn, closeBtn, apply;
  URect tabSeg[3];   // Compressor / Gate / View pill segments
  URect knob[10];    // per-param knob cells; empty (w==0) when not on the active tab
  URect rms[2];      // Peak / RMS segmented halves (Compressor tab; empty otherwise)
  URect viewToggle[5]; // Dyn / Env / GR / Live / A-B pills (View tab; empty otherwise)
  URect resizeGrip;  // bottom-right corner drag handle (free resize)
};
DynLayout ComputeDynLayout(double w, double h, int activeTab = 0);

class UiCanvas {
public:
  UiCanvas();                 // defined in .cpp (Gfx must be complete - pimpl)
  ~UiCanvas();
  UiCanvas(const UiCanvas&) = delete;
  UiCanvas& operator=(const UiCanvas&) = delete;

  // Render the premium transfer-curve hero into an offscreen Blend2D buffer,
  // then BitBlt to hdc at (x, y), sized w x h logical px. The render runs at
  // device pixels (w*dpr x h*dpr) with a dpr scale so it is crisp on HiDPI.
  void RenderTransferCurve(HDC hdc, int x, int y, int w, int h, double dpr,
                           const DynCurveParams& p);

  // Render the full premium Dynamics Panel into the offscreen buffer, then
  // scaled-blit to hdc at (x,y), sized w x h logical px (crisp on HiDPI).
  void RenderPanel(HDC hdc, int x, int y, int w, int h, double dpr,
                   const DynPanelVM& vm);

private:
  bool ensure(HDC hdc, int devW, int devH);   // (re)create offscreen surface
  bool prepareSurface(HDC hdc, int devW, int devH);              // surface + cached image/fonts
  void presentSurface(HDC hdc, int x, int y, int w, int h, int devW, int devH); // copy + scaled blit

  HDC m_memDC = nullptr;
#ifdef _WIN32
  HBITMAP m_memBmp = nullptr;
#endif
  int m_w = 0;
  int m_h = 0;
  std::unique_ptr<Gfx> m_gfx;   // cached BLImage + fonts (opaque; see ui_render.cpp)
};
