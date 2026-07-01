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

// Shared param count for the Dynamics panel (SLIDER_DEFS rows, knob slots, VM
// arrays). v2.3.0: 10 -> 14 (gate extension) -> 15 (M.Boost, upward mode).
constexpr int kDynNumParams = 15;
constexpr int kDynParamMaxBoost = 14;  // knob index; laid out only in Up mode

// Inputs for the transfer-curve hero. Plain values (no Blend2D in the header);
// the compression math mirrors dynamics_engine.cpp ComputeCompression().
struct DynCurveParams {
  double thresholdDb  = -24.0;
  double ratio        = 3.5;
  double kneeDb       = 6.0;
  double gateThreshDb = -55.0;
  double gateRangeDb  = 24.0;   // POSITIVE magnitude (engine value is negated at the wiring point)
  double gateRatio    = 2.0;    // downward-expander ratio Rg (closed-state slope Rg-1; 2.0 = legacy)
  double gateHystDb   = 0.0;    // close threshold relative to gate thresh (<= 0; band shading)
  bool   upward       = false;  // Up mode: boost below threshold (mirrored gain computer)
  double maxBoostDb   = 8.0;    // Up-mode boost cap; boost also floored at gateThreshDb (raw x)
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
  bool   showOff     = false; // sentinel value -> "Off" readout (G.Thr detent)
  bool   showInf     = false; // Ratio at the Inf:1 sentinel (0.0) -> "Inf" readout
  bool   hover       = false; // cursor over (or dragging) this knob -> glow + cap tint
  bool   editing     = false; // inline type-value editor open on this knob (Inc 8)
  const char* editText = nullptr; // live edit buffer (valid only during the paint)
  bool   caretOn     = true;  // editor caret visible this frame (blink; motion pass)
};

// View-model handed to UiCanvas::RenderPanel - pure data (no Blend2D, no engine
// types beyond DynCurveParams). DynamicsPanel builds it each paint from its state;
// the renderer is a pure function of it. Geometry comes from ComputeDynLayout
// (shared with hit-testing), not carried here.
struct DynPanelVM {
  DynCurveParams curve;              // transfer plot + GR meter
  int   activeTab   = 0;            // 0=Compressor, 1=Gate, 2=View
  const char* presetName = nullptr; // null -> "Preset"
  KnobVM knobs[kDynNumParams];       // all params; only on-tab ones get a rect
  // Toggle/state (Inc 5): the View-tab pills + header A/B render from these; the
  // Peak/RMS segmented reads rmsMode. Mirror DynamicsPanel's live members.
  bool  showDyn  = true;
  bool  showEnv  = true;
  bool  showGR   = true;
  bool  liveMode = false;
  bool  bypassed = false;
  bool  rmsMode  = false;
  bool  upward   = false;           // Down/Up header pill state; hides M.Boost when false
  int   meterFloorSel = 0;          // active meter-scale segment index (see kMeterFloorOptDb)
  bool  compact  = false;           // Compact mode: hero plot hidden, knobs in a 4-col grid
  int   dragHandle = -1;            // curve handle being dragged (-1 none, 0 knee, 1 gate) -> glow
  int   hoverHandle = -1;           // curve handle under the cursor -> lights its accent colour
  // Motion pass: the panel computes these from its animation clock; the renderer is a
  // pure function of them (no time access in ui_render.cpp).
  int    tabFrom    = 0;            // tab the active-pill fill slides from
  double tabSlideT  = 1.0;          // 0..1 eased slide progress (1 = settled at activeTab)
  double livePulse  = 0.0;          // Live-pill glow intensity 0..1 (0 = off; breathes when armed)
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
  URect modeBtn;     // DOWN/UP processor-mode state button (A/B-style; click flips)
  URect tabSeg[3];   // Compressor / Gate / View pill segments
  URect knob[kDynNumParams]; // per-param knob cells; empty (w==0) when not on the active tab
  URect rms[2];      // Peak / RMS segmented halves (Compressor tab; empty otherwise)
  URect viewToggle[5]; // Dyn / Env / GR / Live / A-B pills (View tab; empty otherwise)
  URect meterScale[4]; // plot/GR-meter dB-floor selector segments (View tab; empty otherwise)
  URect compactToggle; // Compact-mode pill (View tab; empty otherwise)
  URect resizeGrip;  // bottom-right corner drag handle (free resize)
};
DynLayout ComputeDynLayout(double w, double h, int activeTab = 0, bool compact = false);

// Curve drag-handle hit boxes (base panel coords, ~13px radius), shared by the
// renderer and DynamicsPanel hit-testing so the glyph and the grab box always
// coincide. Computed from the plot well + curve params. [0] = knee handle
// (Threshold/Ratio, Compressor tab), [1] = gate node (Gate Thr/Range, Gate tab);
// the off-tab entry is empty (w==0). Positions clamp into the plot well.
void ComputeCurveHandles(const URect& plotWell, const DynCurveParams& p, int activeTab,
                         URect out[2]);

// --- Settings panel (premium Settings overlay, v2.2.0) -----------------------

// Hit ids shared by the panel's hit-testing and the renderer's hover styling, so
// the highlighted control is always the one a click would land on.
enum SettingsHit {
  SET_HIT_NONE     = -1,
  SET_HIT_CLOSE    = 0,
  SET_HIT_SLIDER   = 1,   // the UI-scale slider row (thumb or track)
  SET_HIT_DENSITY0 = 2,   // Compact / Comfortable / Spacious (DENSITY0 + i)
  SET_HIT_DENSITY1 = 3,
  SET_HIT_DENSITY2 = 4,
  SET_HIT_FIT      = 5,
  // Migrated preferences (A2b). The host maps these 1:1 onto the existing CM_*
  // commands, so the panel and the (OFF-build) menu share one behavior path.
  SET_HIT_RULER0       = 6,   // Relative / Absolute / Bars-Beats (RULER0 + i)
  SET_HIT_RULER1       = 7,
  SET_HIT_RULER2       = 8,
  SET_HIT_MASTER       = 9,   // meter source: master output toggle
  SET_HIT_METER0       = 10,  // Peak / RMS / VU (METER0 + i)
  SET_HIT_METER1       = 11,
  SET_HIT_METER2       = 12,
  SET_HIT_VIEW_METERS  = 13,  // show meters (bottom panel)
  SET_HIT_VIEW_RMS     = 14,  // show RMS overlay
  SET_HIT_VIEW_SNAP    = 15,  // snap to zero-crossing
  SET_HIT_VIEW_MINIMAP = 16,  // minimap
  SET_HIT_VIEW_ZOOM0   = 17,  // wheel-zoom center: mouse (#83)
  SET_HIT_VIEW_ZOOM1   = 18,  // wheel-zoom center: edit cursor
};

// Current preference values, filled by the host each paint (it owns this state).
struct SettingsPrefs {
  int  rulerMode = 0;          // 0=Relative 1=Absolute 2=Bars&Beats
  int  meterMode = 0;          // 0=Peak 1=RMS 2=VU
  bool meterFromMaster = false;
  bool showMeters = true;
  bool showRMS = true;
  bool snapZero = false;
  bool minimap = false;
  bool zoomOnCursor = false;   // wheel zoom centers on the edit cursor (default: mouse)
};

// View-model for RenderSettingsPanel - pure data, built by SettingsPanel each paint.
struct SettingsVM {
  double uiScale  = 1.0;   // current global scale [0.8,2.0] -> thumb position + % readout
  int    hover    = SET_HIT_NONE;
  bool   dragging = false; // thumb drag in flight -> active styling on thumb + readout
  SettingsPrefs prefs;     // migrated preference values (host-owned state)
};

// --- Gain panel (premium port, v2.2.0 Inc D) ---------------------------------
// View-model for RenderGainPanel. The premium panel lays out in the SAME base
// 110x32 coords as the GDI panel's literals (knob zone x<32, close x>96), so
// GainPanel's existing SP()-scaled hit-test serves both builds unchanged.
struct GainVM {
  double valNorm  = 0.0;     // current dB position 0..1 along the 270deg arc
  double zeroNorm = 0.0;     // 0 dB position 0..1 (active arc runs zero->value)
  const char* text = nullptr; // formatted readout ("+3.2 dB", "-inf", "... rel")
  bool boost = false;        // db > 0 -> red arc/readout (over-unity warning)
  bool gold  = false;        // batch-without-selection (relative mode) readout tint
};

// --- Toast (premium port, v2.2.0 Inc F) ---------------------------------------
// View-model for RenderToast: short status pill, faded via alpha. The blit rect
// is opaque (presentSurface forces alpha), so the renderer composites the pill
// over the host-provided background color - the rect then blends seamlessly with
// the waveform background it covers (the GDI toast was an opaque box too).
struct ToastVM {
  const char* text = nullptr;
  double alpha   = 1.0;          // 1 = solid, ->0 over the last 500ms
  double uiScale = 1.0;          // g_uiScale (pill content scales with the UI)
  uint32_t bgColor = 0xFF000000; // theme waveform bg (ARGB) under the pill
};

// --- Bottom-panel meters (premium port, v2.2.0 Inc E) -------------------------
// View-model for RenderMeters: per-channel bar + peak-hold as 0..1 norms over the
// -60..0 dB range (the same DbToX mapping as the GDI meter). The renderer lays out
// in rect/uiScale base coords so the meter content scales with the global UI scale.
struct MetersVM {
  double barNorm[2]  = { 0.0, 0.0 };
  double peakNorm[2] = { 0.0, 0.0 };
  int    numCh = 2;                    // 1 (M) or 2 (L/R)
  int    mode  = 0;                    // 0=Peak 1=RMS 2=VU -> per-mode gradient shades
  const char* modeLabel = nullptr;     // "PPM"/"RMS"/"VU" scale caption
  double uiScale = 1.0;                // g_uiScale (content scales with the UI)
};

// Computed geometry in base kSettingsW x kSettingsH coords - the single source for
// BOTH the renderer and SettingsPanel hit-testing (draw == hit by construction).
struct SettingsLayout {
  URect header, closeBtn;
  URect scaleLabel, scaleValue;  // "UI SCALE" caption + right-aligned % readout
  URect sliderRow, sliderTrack;  // row = interactive zone; track = the visual rail
  URect density[3];              // Compact / Comfortable / Spacious segments
  URect fitBtn;                  // "Fit to window"
  URect rulerCaption, rulerSeg[3];               // RULER section
  URect metersCaption, masterToggle, meterSeg[3]; // METERS section (+ master pill)
  URect viewCaption, viewToggle[4];              // VIEW: Meters / RMS / Snap / Minimap
  URect zoomSeg[2];                              // VIEW: wheel-zoom center Mouse / Cursor
};
SettingsLayout ComputeSettingsLayout(double w, double h);

class UiCanvas {
public:
  UiCanvas();                 // defined in .cpp (Gfx must be complete - pimpl)
  ~UiCanvas();
  UiCanvas(const UiCanvas&) = delete;
  UiCanvas& operator=(const UiCanvas&) = delete;

  // Render the full premium Dynamics Panel into the offscreen buffer, then
  // scaled-blit to hdc at (x,y), sized w x h logical px (crisp on HiDPI).
  void RenderPanel(HDC hdc, int x, int y, int w, int h, double dpr,
                   const DynPanelVM& vm);

  // Render the premium Settings panel (v2.2.0): header + UI-scale slider +
  // density presets + fit-to-window. Same surface/blit flow as RenderPanel.
  void RenderSettingsPanel(HDC hdc, int x, int y, int w, int h, double dpr,
                           const SettingsVM& vm);

  // Render the premium gain knob overlay (v2.2.0 Inc D): 110x32 base, knob +
  // active arc + dB readout + close. Crisp on HiDPI like the other panels.
  void RenderGainPanel(HDC hdc, int x, int y, int w, int h, double dpr,
                       const GainVM& vm);

  // Render the premium L/R meters into the bottom-panel meters rect (v2.2.0
  // Inc E): per-mode gradient bars + peak-hold + scaled dB ticks/labels.
  void RenderMeters(HDC hdc, int x, int y, int w, int h, double dpr,
                    const MetersVM& vm);

  // Render the premium toast pill (v2.2.0 Inc F), alpha-faded; drawn LAST in
  // OnPaintOverlay so it sits above every other premium surface.
  void RenderToast(HDC hdc, int x, int y, int w, int h, double dpr,
                   const ToastVM& vm);

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
  int m_strideW = 0;   // real surface row stride (alignment-padded; see ensure())
  std::unique_ptr<Gfx> m_gfx;   // cached BLImage + fonts (opaque; see ui_render.cpp)
};
