// dynamics_panel.cpp — Professional dynamics control panel
#include "dynamics_panel.h"
#include "theme.h"
#include "config.h"
#include "globals.h"
#include "ui_theme.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

// Slider/knob definitions: label, min, max, unit, precision, defaultVal.
// defaultVal feeds the knob default-tick + Cmd-reset (Inc 4); it mirrors the
// DynamicsParams defaults. -100 on Thresh = "use the analysed average peak".
const DynamicsPanel::SliderDef DynamicsPanel::SLIDER_DEFS[NUM_SLIDERS] = {
  { "Thresh",  -60.0,    0.0, "dB",  1, -100.0 },  // 0: operating-point default
  { "Ratio",     1.0,   20.0, ":1",  1,    4.0 },  // 1
  { "Knee",      0.0,   24.0, "dB",  0,    6.0 },  // 2
  { "Attack",    0.0,  500.0, "ms",  0,    5.0 },  // 3
  { "Release",   0.0, 1000.0, "ms",  0,  100.0 },  // 4
  { "Makeup",    0.0,   24.0, "dB",  1,    0.0 },  // 5
  { "L.ahead",   0.0,   20.0, "ms",  1,    0.0 },  // 6
  { "G.Thr",   -60.0,    0.0, "dB",  1, -100.0 },  // 7: gate threshold (-100 = off, engine default)
  { "G.Range", -40.0,    0.0, "dB",  0,  -20.0 },  // 8: gate max reduction
  { "G.Hold",    0.0,  200.0, "ms",  0,   50.0 },  // 9: gate hold time
};

// --- Layout constants ---
static constexpr int MARGIN = 4;
static constexpr int LABEL_W = 42;
static constexpr int LABEL_GAP = 4;
static constexpr int LEFT_TRACK_W = 74;
static constexpr int RIGHT_TRACK_W = 62;
static constexpr int VALUE_GAP = 4;

static constexpr int L_TRACK_X = MARGIN + LABEL_W + LABEL_GAP;
static constexpr int L_VALUE_X = L_TRACK_X + LEFT_TRACK_W + VALUE_GAP;

static constexpr int R_LABEL_X = 196;
static constexpr int R_TRACK_X = R_LABEL_X + LABEL_W + LABEL_GAP;
static constexpr int R_VALUE_X = R_TRACK_X + RIGHT_TRACK_W + VALUE_GAP;

static constexpr int APPLY_W = 50;
static constexpr int APPLY_H = 14;
static constexpr int TOGGLE_W = 28;
static constexpr int TOGGLE_GAP = 3;

// Premium panel free-resize bounds (aspect-locked uniform scale; bottom-right grip).
// [[maybe_unused]]: only the premium build references these (OFF build = GDI panel).
[[maybe_unused]] static constexpr double UI_SCALE_MIN = 0.8;   // ~384x240 floor
[[maybe_unused]] static constexpr double UI_SCALE_MAX = 2.0;   // ~960x600 cap (clamped to window)

// --- Motion pass (premium) durations (seconds) + clock ---
// [[maybe_unused]]: only the premium build references these.
[[maybe_unused]] static constexpr double kTabSlideSec  = 0.18;  // active tab fill glide
[[maybe_unused]] static constexpr double kValueEaseSec = 0.12;  // knob arc ease on wheel/type/reset
[[maybe_unused]] static constexpr double kLivePulseSec = 1.5;   // Live pill breathing cycle
[[maybe_unused]] static constexpr double kCaretBlinkSec = 1.0;  // editor caret on+off cycle

// Monotonic wall-clock seconds for animation timing (steady, cross-platform; not the
// REAPER API). Animation values are derived from elapsed = NowSec() - <startSec>.
[[maybe_unused]] static double NowSec()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// --- Value access ---

double DynamicsPanel::GetSliderValue(int idx) const
{
  switch (idx) {
    case 0: return m_params.threshold;
    case 1: return m_params.ratio;
    case 2: return m_params.kneeDb;
    case 3: return m_params.attackMs;
    case 4: return m_params.releaseMs;
    case 5: return m_params.autoMakeup ? 0.0 : m_params.makeupDb;
    case 6: return m_params.lookaheadMs;
    case 7: return m_params.gateThreshDb;
    case 8: return m_params.gateRangeDb;
    case 9: return m_params.gateHoldMs;
    default: return 0.0;
  }
}

void DynamicsPanel::SetSliderValue(int idx, double val)
{
  const auto& def = SLIDER_DEFS[idx];
  val = std::max(def.minVal, std::min(def.maxVal, val));
  switch (idx) {
    case 0: m_params.threshold = val; break;
    case 1: m_params.ratio = val; break;
    case 2: m_params.kneeDb = val; break;
    case 3: m_params.attackMs = val; break;
    case 4: m_params.releaseMs = val; break;
    case 5: m_params.makeupDb = val; m_params.autoMakeup = false; break;
    case 6: m_params.lookaheadMs = val; break;
    case 7: m_params.gateThreshDb = val; break;
    case 8: m_params.gateRangeDb = val; break;
    case 9: m_params.gateHoldMs = val; break;
  }
}

// --- Lifecycle ---

void DynamicsPanel::Show(const DynamicsParams& params, double avgPeakDb)
{
  m_params = params;
  m_avgPeakDb = avgPeakDb;
  if (m_params.threshold <= -99.0)
    m_params.threshold = avgPeakDb;
  m_visible = true;
  m_paramsChanged = false;
  m_applyRequested = false;
  m_dragSlider = -1;
  m_editIdx = -1;
  m_motionInit = false;        // re-seed value-ease on open (no glide from a stale value)
  m_tabSlideStartSec = -1.0;   // no tab-slide on open
  m_avgGR = 0.0;
  // Reset toggles to show everything on panel open
  m_showDyn = true;
  m_showEnv = true;
  m_showGR = true;
  m_meterFloorSel = 0;   // default -60 dB floor (RestoreDynamicsViewPrefs overrides from ExtState)
  m_compactMode = false; // default normal layout (RestoreDynamicsViewPrefs overrides from ExtState)
  m_bypassed = false;
  m_liveMode = false;
  m_liveUndoOpen = false;
}

void DynamicsPanel::Hide()
{
  m_visible = false;
  m_dragSlider = -1;
  m_editIdx = -1;
  m_hoverKnob = -1;
  m_hoverHandle = -1;
  m_dragHandle = -1;
  m_panelDragging = false;
  m_resizing = false;
  m_bypassed = false;
  m_liveMode = false;
  // Note: live undo block closure handled by SneakPeak (needs g_Undo_EndBlock2)
}

// --- Layout ---

// Premium panel base dimensions. Width is always kPanelW; the height shrinks in
// Compact mode (the hero plot is hidden). All premium geometry (GetRect, the resize
// fit-clamp, hit-testing) derives the height from here so the two modes stay in sync.
double DynamicsPanel::PanelBaseH() const
{
  return m_compactMode ? (double)dynui::kPanelHCompact : (double)dynui::kPanelH;
}

// Effective on-screen panel scale (v2.2.0 coupling): the global UI scale layered
// with the panel's own resize grip, soft-capped at EFF_SCALE_MAX so the grip stays
// useful at high global scales. Every premium geometry/hit-test site uses THIS
// (never m_uiScale directly), so draw and hit can never disagree.
double DynamicsPanel::EffScale() const
{
  const double g = g_uiScale > 0.0 ? g_uiScale : 1.0;
  const double m = m_uiScale > 0.0 ? m_uiScale : 1.0;
  return std::max(UI_SCALE_MIN, std::min(EFF_SCALE_MAX, g * m));
}

// The current panel layout (single source for render-prep + every hit-test): base
// coords, current tab, current compact state.
DynLayout DynamicsPanel::PanelLayout() const
{
  return ComputeDynLayout((double)dynui::kPanelW, PanelBaseH(), (int)m_tab, m_compactMode);
}

RECT DynamicsPanel::GetRect(RECT wr) const
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  const int pw = (int)std::lround((double)dynui::kPanelW * EffScale());
  const int ph = (int)std::lround(PanelBaseH() * EffScale());
#else
  const int pw = PANEL_W, ph = PANEL_H;
#endif
  int cx = (wr.left + wr.right) / 2 + m_offsetX;
  int cy = wr.bottom - ph - 10 + m_offsetY;
  int left = cx - pw / 2;
  if (left < wr.left) left = wr.left;
  if (left + pw > wr.right) left = wr.right - pw;
  if (cy < wr.top) cy = wr.top;
  if (cy + ph > wr.bottom) cy = wr.bottom - ph;
  return { left, cy, left + pw, cy + ph };
}

RECT DynamicsPanel::GetSliderTrackRect(RECT pr, int idx) const
{
  int col = SliderCol(idx);
  int row = SliderRow(idx);
  int trackX = pr.left + ((col == 0) ? L_TRACK_X : R_TRACK_X);
  int trackW = (col == 0) ? LEFT_TRACK_W : RIGHT_TRACK_W;
  int cy = pr.top + TITLE_H + row * ROW_H + ROW_H / 2;
  return { trackX, cy - TRACK_H / 2, trackX + trackW, cy + TRACK_H / 2 + 1 };
}

RECT DynamicsPanel::GetApplyButtonRect(RECT pr) const
{
  int x = pr.right - MARGIN - APPLY_W - 2;
  int y = pr.top + TITLE_H + 5 * ROW_H + 10;
  return { x, y, x + APPLY_W, y + APPLY_H };
}

RECT DynamicsPanel::GetRmsToggleRect(RECT pr) const
{
  int x = pr.left + R_LABEL_X;
  int y = pr.top + TITLE_H + 5 * ROW_H + 10;
  return { x, y, x + 42, y + APPLY_H };
}

RECT DynamicsPanel::GetDynToggleRect(RECT pr) const
{
  int x = pr.left + MARGIN;
  int y = pr.top + TITLE_H + 5 * ROW_H + 10;
  return { x, y, x + TOGGLE_W, y + APPLY_H };
}

RECT DynamicsPanel::GetEnvToggleRect(RECT pr) const
{
  RECT dyn = GetDynToggleRect(pr);
  int x = dyn.right + TOGGLE_GAP;
  int y = dyn.top;
  return { x, y, x + TOGGLE_W, y + APPLY_H };
}

RECT DynamicsPanel::GetLiveToggleRect(RECT pr) const
{
  RECT envR = GetEnvToggleRect(pr);
  int x = envR.right + TOGGLE_GAP;
  int y = envR.top;
  return { x, y, x + TOGGLE_W + 4, y + APPLY_H }; // slightly wider for "Live"
}

RECT DynamicsPanel::GetGRToggleRect(RECT pr) const
{
  RECT liveR = GetLiveToggleRect(pr);
  int x = liveR.right + TOGGLE_GAP;
  int y = liveR.top;
  return { x, y, x + TOGGLE_W - 4, y + APPLY_H }; // compact "GR"
}

RECT DynamicsPanel::GetABToggleRect(RECT pr) const
{
  RECT grR = GetGRToggleRect(pr);
  int x = grR.right + TOGGLE_GAP;
  int y = grR.top;
  return { x, y, x + TOGGLE_W - 4, y + APPLY_H }; // compact "A/B"
}

RECT DynamicsPanel::GetPresetButtonRect(RECT pr) const
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  // Layout is in base coords; scale onto the on-screen (scaled) panel rect.
  const double S = EffScale();
  DynLayout L = PanelLayout();
  return { pr.left + (int)(L.preset.x * S), pr.top + (int)(L.preset.y * S),
           pr.left + (int)((L.preset.x + L.preset.w) * S),
           pr.top + (int)((L.preset.y + L.preset.h) * S) };
#else
  return { pr.left + 92, pr.top + 4, pr.left + 198, pr.top + 18 };
#endif
}

RECT DynamicsPanel::GetCloseButtonRect(RECT pr) const
{
  return { pr.right - 16, pr.top + 4, pr.right - 2, pr.top + 18 };
}

void DynamicsPanel::ApplyPreset(int idx)
{
  if (idx < 0 || idx >= PRESET_COUNT) return;
  m_params = g_dynamicsPresets[idx].params;
  // Sentinel threshold: use average peak for initial value
  if (m_params.threshold <= -99.0)
    m_params.threshold = m_avgPeakDb;
  m_presetIdx = idx;
  m_paramsChanged = true;
}

void DynamicsPanel::ApplyParams(const DynamicsParams& p)
{
  m_params = p;
  if (m_params.threshold <= -99.0)   // sentinel -> operating point
    m_params.threshold = m_avgPeakDb;
  m_presetIdx = -1;                  // custom (user preset, not a built-in slot)
  m_paramsChanged = true;
}

// --- Coordinate conversion ---

int DynamicsPanel::ValueToPixel(double val, RECT tr, int idx) const
{
  const auto& def = SLIDER_DEFS[idx];
  double ratio = (val - def.minVal) / (def.maxVal - def.minVal);
  ratio = std::max(0.0, std::min(1.0, ratio));
  return tr.left + (int)(ratio * (double)(tr.right - tr.left));
}

double DynamicsPanel::PixelToValue(int px, RECT tr, int idx) const
{
  const auto& def = SLIDER_DEFS[idx];
  double ratio = (double)(px - tr.left) / (double)(tr.right - tr.left);
  ratio = std::max(0.0, std::min(1.0, ratio));
  return def.minVal + ratio * (def.maxVal - def.minVal);
}

bool DynamicsPanel::IsFineMode()
{
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

// --- Hit testing ---

bool DynamicsPanel::HitTest(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  RECT r = GetRect(wr);
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

int DynamicsPanel::HitTestSlider(int x, int y, RECT pr) const
{
  for (int i = 0; i < NUM_SLIDERS; i++) {
    RECT tr = GetSliderTrackRect(pr, i);
    if (x >= tr.left - THUMB_R && x <= tr.right + THUMB_R &&
        y >= tr.top - 8 && y <= tr.bottom + 8)
      return i;
  }
  return -1;
}

// --- Mouse interaction ---

#ifdef SNEAKPEAK_BLEND2D_PANEL
// Premium-panel hit routing via the shared ComputeDynLayout (panel-relative px),
// so clicks match exactly what RenderPanel drew. Sets the same poll flags the host
// already handles (preset/apply/bypass) + the internal tab; empty area drags the
// panel. (Knob + curve-handle interaction land in Inc 4/7.)
bool DynamicsPanel::OnMouseDownPremium(int x, int y, RECT pr)
{
  // Layout authority is in base coords (RenderPanel scales the whole panel by S);
  // convert the click into base coords so hit-tests match exactly what was drawn.
  const double S = EffScale();
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const DynLayout L = PanelLayout();

  // Bottom-right grip -> start an aspect-locked resize (top-left stays anchored).
  if (L.resizeGrip.contains(lx, ly)) {
    m_resizing = true;
    m_resizeAnchorL = pr.left;
    m_resizeAnchorT = pr.top;
    m_resizeStartX = x;                          // relative-delta: first move = no-op
    m_resizeStartScale = m_uiScale;              // (no snap wherever in the grip you grab)
    return true;                                 // IsDragging() -> host SetCapture
  }

  if (L.closeBtn.contains(lx, ly)) { Hide(); return true; }
  if (L.preset.contains(lx, ly))   { m_presetMenuRequested = true; return true; }
  if (L.abBtn.contains(lx, ly))    { m_bypassed = !m_bypassed; return true; }
  for (int i = 0; i < 3; ++i)
    if (L.tabSeg[i].contains(lx, ly)) {
      if ((Tab)i != m_tab) { m_tabFrom = (int)m_tab; m_tabSlideStartSec = NowSec(); m_tab = (Tab)i; }
      return true;                                // start the active-pill slide (motion pass)
    }
  if (!m_liveMode && L.apply.contains(lx, ly)) { m_applyRequested = true; return true; }

  // Curve drag-handles on the plot. Knee handle = Threshold only (HORIZONTAL; ratio
  // is the line's slope, set by the dedicated knob - the universal compressor
  // convention confirmed across FabFilter Pro-C/Ableton/Logic and the readable OSS
  // LSP/Calf/JUCE comps). Gate node = Gate Thr (X) + Gate Range (Y). Records the grab
  // Y + gate-range value for the gate's vertical axis; reuses m_dragHandle's
  // IsDragging() -> host SetCapture + re-analyse + Live-undo lifecycle (knob-identical).
  {
    URect handles[2];
    ComputeCurveHandles(L.plotWell, BuildCurveParams(), (int)m_tab, handles);
    for (int h = 0; h < 2; ++h) {
      if (!handles[h].contains(lx, ly)) continue;
      m_dragHandle = h;
      m_handleStartY = y;
      if (h == HANDLE_GATE) m_handleStartVal = GetSliderValue(8);    // gate range -> vertical axis
      m_handleGrabDx = lx - (handles[h].x + handles[h].w * 0.5);     // no X jump on grab (base px)
      return true;                                 // IsDragging() -> host SetCapture
    }
  }

  // Knob hit (only on-tab params have a non-empty rect). Cmd-click resets to the
  // param default; otherwise start a velocity-sensitive vertical drag. Reusing
  // m_dragSlider keeps IsDragging()/SetCapture/reanalyze/Live byte-for-byte with
  // the GDI slider path.
  for (int i = 0; i < NUM_SLIDERS; ++i) {
    if (!L.knob[i].contains(lx, ly)) continue;
    if (IsFineMode()) {                          // Cmd-click = reset to the param default
      if (i == 5) {                              // Makeup -> auto (DynamicsParams default)
        m_params.makeupDb = 0.0;
        m_params.autoMakeup = true;
      } else if (i == 7) {                       // Gate threshold -> off (engine sentinel -100)
        m_params.gateThreshDb = -100.0;
      } else {
        double dflt = SLIDER_DEFS[i].defaultVal;
        if (i == 0 && dflt <= -99.0) dflt = m_avgPeakDb;
        SetSliderValue(i, dflt);
      }
      m_paramsChanged = true;
      m_presetIdx = -1;
      return true;
    }
    m_dragSlider = i;
    m_dragStartY = y;
    m_dragLastY = y;
    m_dragStartVal = GetSliderValue(i);
    return true;                                 // IsDragging() -> host SetCapture
  }

  // Peak/RMS detection switch (Compressor tab): mutually-exclusive, sets rmsMode +
  // m_paramsChanged so the host re-analyses immediately - identical to the GDI toggle.
  if (L.rms[0].contains(lx, ly) || L.rms[1].contains(lx, ly)) {
    const bool wantRms = L.rms[1].contains(lx, ly);
    if (wantRms != m_params.rmsMode) { m_params.rmsMode = wantRms; m_paramsChanged = true; }
    return true;
  }

  // View-tab state toggles (independent). Each flips exactly the member its GDI
  // counterpart does, so the host's existing polls fire unchanged: m_showDyn/Env/GR
  // are read each paint (overlay + envelope sync); Live arms with an initial Apply
  // and the undo block is managed by the host; A/B drives the ACTIVE edge-compare.
  if (L.viewToggle[0].contains(lx, ly)) { m_showDyn = !m_showDyn; m_viewPrefsChanged = true; return true; }
  if (L.viewToggle[1].contains(lx, ly)) { m_showEnv = !m_showEnv; m_viewPrefsChanged = true; return true; }
  if (L.viewToggle[2].contains(lx, ly)) { m_showGR  = !m_showGR;  m_viewPrefsChanged = true; return true; }
  if (L.viewToggle[3].contains(lx, ly)) {
    m_liveMode = !m_liveMode;
    if (m_liveMode) m_applyRequested = true;     // initial apply when arming (GDI parity)
    m_viewPrefsChanged = true;                   // Live persists across sessions (user request)
    return true;
  }
  // (A/B is handled by the header abBtn above - no View-tab A/B pill.)
  // Meter-scale dB-floor selector: render-only (rescales plot + GR meter), so persist
  // the pref but do NOT mark params changed (no re-analysis).
  for (int i = 0; i < 3; ++i)
    if (L.meterScale[i].contains(lx, ly)) { m_meterFloorSel = i; m_viewPrefsChanged = true; return true; }
  // Compact toggle: relayout only (panel resizes on the next paint); persist the pref.
  if (L.compactToggle.contains(lx, ly)) { m_compactMode = !m_compactMode; m_viewPrefsChanged = true; return true; }

  m_panelDragging = true;          // empty area -> drag (OnMouseMove consumes offsets)
  m_dragOffsetX = x - pr.left;
  m_dragOffsetY = y - pr.top;
  return true;
}
#endif

bool DynamicsPanel::OnMouseDown(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  if (!HitTest(x, y, wr)) return false;

  RECT pr = GetRect(wr);

#ifdef SNEAKPEAK_BLEND2D_PANEL
  return OnMouseDownPremium(x, y, pr);
#endif

  // Close button
  RECT closeR = GetCloseButtonRect(pr);
  if (x >= closeR.left && x < closeR.right && y >= closeR.top && y < closeR.bottom) {
    Hide();
    return true;
  }

  // Preset button
  RECT presetR = GetPresetButtonRect(pr);
  if (x >= presetR.left && x < presetR.right && y >= presetR.top && y < presetR.bottom) {
    m_presetMenuRequested = true;
    return true;
  }

  // Apply button (disabled during Live mode - points already written in real-time)
  if (!m_liveMode) {
    RECT applyR = GetApplyButtonRect(pr);
    if (x >= applyR.left && x < applyR.right && y >= applyR.top && y < applyR.bottom) {
      m_applyRequested = true;
      return true;
    }
  }

  // Peak/RMS toggle
  RECT rmsR = GetRmsToggleRect(pr);
  if (x >= rmsR.left && x < rmsR.right && y >= rmsR.top && y < rmsR.bottom) {
    m_params.rmsMode = !m_params.rmsMode;
    m_paramsChanged = true;
    return true;
  }

  // Dyn visibility toggle
  RECT dynR = GetDynToggleRect(pr);
  if (x >= dynR.left && x < dynR.right && y >= dynR.top && y < dynR.bottom) {
    m_showDyn = !m_showDyn;
    m_viewPrefsChanged = true;
    return true;
  }

  // Env visibility toggle
  RECT envR = GetEnvToggleRect(pr);
  if (x >= envR.left && x < envR.right && y >= envR.top && y < envR.bottom) {
    m_showEnv = !m_showEnv;
    m_viewPrefsChanged = true;
    return true;
  }

  // Live toggle
  RECT liveR = GetLiveToggleRect(pr);
  if (x >= liveR.left && x < liveR.right && y >= liveR.top && y < liveR.bottom) {
    m_liveMode = !m_liveMode;
    if (m_liveMode) m_applyRequested = true; // initial apply when turning on
    m_viewPrefsChanged = true;               // Live persists across sessions
    return true;
  }

  // GR shading toggle
  RECT grR = GetGRToggleRect(pr);
  if (x >= grR.left && x < grR.right && y >= grR.top && y < grR.bottom) {
    m_showGR = !m_showGR;
    m_viewPrefsChanged = true;
    return true;
  }

  // A/B bypass toggle
  RECT abR = GetABToggleRect(pr);
  if (x >= abR.left && x < abR.right && y >= abR.top && y < abR.bottom) {
    m_bypassed = !m_bypassed;
    return true;
  }

  // Slider hit
  int slider = HitTestSlider(x, y, pr);
  if (slider >= 0) {
    m_dragSlider = slider;
    m_dragStartX = x;
    m_dragStartVal = GetSliderValue(slider);
    // Grab offset: distance from click to current thumb center.
    // This prevents the value from jumping when you grab near (not on) the thumb.
    RECT tr = GetSliderTrackRect(pr, slider);
    int thumbX = ValueToPixel(m_dragStartVal, tr, slider);
    m_dragGrabOffset = thumbX - x;
    // Don't set value on click - start dragging from current position
    return true;
  }

  // Panel drag
  m_panelDragging = true;
  m_dragOffsetX = x - pr.left;
  m_dragOffsetY = y - pr.top;
  return true;
}

void DynamicsPanel::OnMouseMove(int x, int y, RECT wr)
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  if (m_resizing) {
    // Aspect-locked: the cursor's horizontal travel from the grab point drives the
    // EFFECTIVE scale 1:1 in panel pixels (relative so the grabbed pixel stays put -
    // no first-move snap wherever in the grip you clicked); height follows. The grip
    // component is then solved back out under the global scale (eff = g * grip).
    const double g = g_uiScale > 0.0 ? g_uiScale : 1.0;
    const double effStart = std::max(UI_SCALE_MIN, std::min(EFF_SCALE_MAX, g * m_resizeStartScale));
    double eff = effStart + (double)(x - m_resizeStartX) / (double)dynui::kPanelW;
    // Fit is measured from the pinned top-left to the window's right/bottom edges,
    // so the panel never overflows (which would make GetRect re-clamp and break the
    // anchor). Since the anchor came from a fitting rect, both fits are >= current eff.
    const double fitW = (double)(wr.right - m_resizeAnchorL) / (double)dynui::kPanelW;
    const double fitH = (double)(wr.bottom - m_resizeAnchorT) / PanelBaseH();
    double effMax = std::min(EFF_SCALE_MAX, std::min(fitW, fitH));
    if (effMax < UI_SCALE_MIN) effMax = UI_SCALE_MIN;
    eff = std::max(UI_SCALE_MIN, std::min(effMax, eff));
    m_uiScale = std::max(UI_SCALE_MIN, std::min(UI_SCALE_MAX, eff / g));
    // Pin the top-left corner: solve GetRect's centring offsets for the new size.
    // EffScale() (not eff) so the pin agrees with GetRect even when the grip clamp
    // made the requested eff unreachable.
    const int W1 = (int)std::lround((double)dynui::kPanelW * EffScale());
    const int H1 = (int)std::lround(PanelBaseH() * EffScale());
    m_offsetX = (m_resizeAnchorL + W1 / 2) - (wr.left + wr.right) / 2;
    m_offsetY = m_resizeAnchorT - (wr.bottom - H1 - 10);
    m_geomChanged = true;            // persist size + position on mouse-up
    return;
  }
  if (m_dragHandle >= 0) { DragCurveHandle(x, y, GetRect(wr)); return; }
#endif
  if (m_dragSlider >= 0) {
#ifdef SNEAKPEAK_BLEND2D_PANEL
    // Velocity-sensitive vertical knob drag: moving up increases. Faster motion
    // covers more range (slow = fine). Accumulates on the live value - relative,
    // no wind-up because the clamped value is read back each move. (x unused.)
    (void)x;
    const auto& def = SLIDER_DEFS[m_dragSlider];
    const double range = (def.maxVal - def.minVal) != 0.0 ? (def.maxVal - def.minVal) : 1.0;
    const int dy = m_dragLastY - y;              // +ve = moved up
    m_dragLastY = y;
    const int ady = dy < 0 ? -dy : dy;
    const double speed = ady > 40 ? 40.0 : (double)ady;
    const double gainPerPx = (1.0 / 240.0) * (1.0 + speed * 0.05);
    // Seed from the value the user actually SEES: with auto-makeup on, the knob
    // displays the computed makeup (fabs(avgGR)), but GetSliderValue(5) reports 0
    // while auto is engaged - seeding from 0 would snap the knob down on first move.
    const double cur = (m_dragSlider == 5 && m_params.autoMakeup)
                         ? fabs(m_avgGR) : GetSliderValue(m_dragSlider);
    double nrm = (cur - def.minVal) / range;
    nrm = std::max(0.0, std::min(1.0, nrm + (double)dy * gainPerPx));
    SetSliderValue(m_dragSlider, def.minVal + nrm * range);
    m_paramsChanged = true;
    m_presetIdx = -1; // manual adjustment = custom
#else
    RECT pr = GetRect(wr);
    RECT tr = GetSliderTrackRect(pr, m_dragSlider);

    double val;
    if (IsFineMode()) {
      // Fine mode (Cmd/Ctrl): delta-based, 1/5th sensitivity from drag start value
      int dx = x - m_dragStartX;
      double pxRange = (double)(tr.right - tr.left);
      const auto& def = SLIDER_DEFS[m_dragSlider];
      double valRange = def.maxVal - def.minVal;
      double delta = ((double)dx / pxRange) * valRange * 0.2;
      val = m_dragStartVal + delta;
    } else {
      // Normal mode: absolute position with grab offset (no jump on click)
      val = PixelToValue(x + m_dragGrabOffset, tr, m_dragSlider);
    }
    SetSliderValue(m_dragSlider, val);
    m_paramsChanged = true;
    m_presetIdx = -1; // manual adjustment = custom
#endif
  }
  else if (m_panelDragging) {
#ifdef SNEAKPEAK_BLEND2D_PANEL
    const int pw = (int)std::lround((double)dynui::kPanelW * EffScale());
    const int ph = (int)std::lround(PanelBaseH() * EffScale());
#else
    const int pw = PANEL_W, ph = PANEL_H;
#endif
    int defaultCX = (wr.left + wr.right) / 2;
    int defaultCY = wr.bottom - ph - 10;
    int newLeft = x - m_dragOffsetX;
    int newTop = y - m_dragOffsetY;
    m_offsetX = (newLeft + pw / 2) - defaultCX;
    m_offsetY = newTop - defaultCY;
    m_geomChanged = true;            // persist size + position on mouse-up
  }
}

void DynamicsPanel::OnMouseUp()
{
  m_dragSlider = -1;
  m_panelDragging = false;
  m_resizing = false;
  m_dragHandle = -1;
}

// --- Premium wheel-nudge + hover (Inc 6) ------------------------------------

// Knob under the cursor in BASE coords (same convention as OnMouseDownPremium),
// or -1. Shared by hover + wheel. Premium layout; only called from premium paths.
int DynamicsPanel::HitTestKnob(int x, int y, RECT pr) const
{
  const double S = EffScale();
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const DynLayout L = PanelLayout();
  for (int i = 0; i < NUM_SLIDERS; ++i)
    if (L.knob[i].contains(lx, ly)) return i;
  return -1;
}

// Scroll over a knob nudges its value in normalized space (coarse 1/40 per notch,
// fine 1/200 with Cmd). Reuses SetSliderValue (clamp + autoMakeup side-effect) and
// sets the same poll flags a knob drag does, so the host re-analyses identically.
bool DynamicsPanel::OnMouseWheel(int x, int y, double steps, bool fine, RECT wr)
{
  if (!m_visible) return false;
  const int i = HitTestKnob(x, y, GetRect(wr));
  if (i < 0) return false;
  const auto& def = SLIDER_DEFS[i];
  const double range = (def.maxVal - def.minVal) != 0.0 ? (def.maxVal - def.minVal) : 1.0;
  // Seed Makeup from the displayed value while auto (GetSliderValue(5)==0 then).
  const double cur = (i == 5 && m_params.autoMakeup) ? fabs(m_avgGR) : GetSliderValue(i);
  double nrm = (cur - def.minVal) / range;
  nrm = std::max(0.0, std::min(1.0, nrm + (fine ? 0.005 : 0.025) * steps));
  SetSliderValue(i, def.minVal + nrm * range);
  m_paramsChanged = true;
  m_presetIdx = -1;
  return true;
}

// Update the hovered knob AND curve handle; returns true if either changed (the
// host repaints then - for the knob glow and the handle lighting its accent).
bool DynamicsPanel::OnHover(int x, int y, RECT wr)
{
  int hk = -1, hh = -1;
  if (m_visible) {
    const RECT pr = GetRect(wr);
    hk = HitTestKnob(x, y, pr);
    const double S = EffScale();
    const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
    const DynLayout L = PanelLayout();
    URect handles[2];
    ComputeCurveHandles(L.plotWell, BuildCurveParams(), (int)m_tab, handles);
    for (int h = 0; h < 2; ++h) if (handles[h].contains(lx, ly)) { hh = h; break; }
  }
  if (hk == m_hoverKnob && hh == m_hoverHandle) return false;
  m_hoverKnob = hk;
  m_hoverHandle = hh;
  return true;
}

// Cursor-feedback helper: is the cursor over the bottom-right resize grip?
bool DynamicsPanel::IsOverResizeGrip(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  RECT pr = GetRect(wr);
  const double S = EffScale();
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const DynLayout L = PanelLayout();
  return L.resizeGrip.contains(lx, ly);
}

// --- Premium curve handles (Inc 7) -----------------------------------------

// Live params -> DynCurveParams, shared by the render VM and the curve-handle
// hit-test/drag so the drawn curve and the grab boxes can never drift apart.
DynCurveParams DynamicsPanel::BuildCurveParams() const
{
  DynCurveParams c;
  c.thresholdDb  = (m_params.threshold <= -99.0) ? m_avgPeakDb : m_params.threshold;
  c.ratio        = m_params.ratio;
  c.kneeDb       = m_params.kneeDb;
  c.gateThreshDb = m_params.gateThreshDb;
  c.gateRangeDb  = -m_params.gateRangeDb;            // engine NEGATIVE -> render POSITIVE magnitude (#1)
  c.makeupDb     = m_params.autoMakeup ? -m_avgGR : m_params.makeupDb;
  c.avgPeakDb    = m_avgPeakDb;
  c.avgGrDb      = m_avgGR;                          // engine avg GR (negative) drives the meter (#2)
  c.showGate     = (m_params.gateThreshDb > -99.0);
  c.inMinDb      = dynui::kMeterFloorOptDb[m_meterFloorSel];  // View-tab meter-scale floor (default -60); inMaxDb stays 0
  return c;
}

// Apply a curve-handle drag to params. Primary axis = absolute cursor-X -> input dB:
// knee handle -> Threshold (idx 0), gate node -> Gate Thr (idx 7). The KNEE handle is
// HORIZONTAL-ONLY: ratio is the slope of the line above the knee and is set by the
// dedicated Ratio knob, matching the universal convention (no proven compressor -
// closed or open source - maps a knee point's vertical drag to ratio; threshold is a
// horizontal point, ratio is a knob/slope). The GATE node keeps a vertical axis ->
// Gate Range (a genuine depth, not a slope; drag-down = deeper). All via SetSliderValue
// so the host re-analyses exactly as for a knob drag (same m_paramsChanged poll, Live
// undo lifecycle via IsDragging).
void DynamicsPanel::DragCurveHandle(int x, int y, RECT pr)
{
  if (m_dragHandle < 0) return;
  const double S = EffScale();
  const DynCurveParams cp = BuildCurveParams();
  const DynLayout L = PanelLayout();
  const double span = (cp.inMaxDb - cp.inMinDb) != 0.0 ? (cp.inMaxDb - cp.inMinDb) : 1.0;
  const double lx = (double)(x - pr.left) / S - m_handleGrabDx;   // subtract grab offset (no jump)
  const double inDb = cp.inMinDb + (lx - L.plotWell.x) / L.plotWell.w * span;   // X -> input dB

  const int primary = (m_dragHandle == HANDLE_KNEE) ? 0 : 7;     // Threshold / Gate Thr
  SetSliderValue(primary, inDb);                                  // clamps to the slider range

  // Vertical axis applies ONLY to the gate node (Gate Range): relative drag from the
  // grab point, drag-down = deeper reduction. The knee handle has no vertical mapping.
  if (m_dragHandle == HANDLE_GATE) {
    const auto& sd = SLIDER_DEFS[8];                              // Gate Range [-40, 0]
    const double rng = (sd.maxVal - sd.minVal) != 0.0 ? (sd.maxVal - sd.minVal) : 1.0;
    const double startNorm = (m_handleStartVal - sd.minVal) / rng;
    const double norm = std::max(0.0, std::min(1.0,
        startNorm - (double)(y - m_handleStartY) / S / 160.0));   // drag down = deeper range
    SetSliderValue(8, sd.minVal + norm * rng);
  }
  m_paramsChanged = true;
  m_presetIdx = -1;
}

// --- Inline type-value editor (Inc 8) ---------------------------------------

#ifdef SNEAKPEAK_BLEND2D_PANEL

// VK/char -> the single character to append, or 0 if not an accepted edit key.
// macOS SWELL delivers punctuation as its ASCII code ('.', '-', ':'); Windows
// delivers VK_OEM_*/numpad codes. Handle both. The field is context-aware (it knows
// which param), so ':' and unit letters are optional - the user just types the number.
static char MapEditChar(int vk)
{
  if (vk >= '0' && vk <= '9') return (char)vk;                  // top-row digits (all platforms)
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return (char)('0' + (vk - VK_NUMPAD0));
  if (vk == '.' || vk == VK_DECIMAL  || vk == 0xBE) return '.'; // 0xBE = Win VK_OEM_PERIOD
  if (vk == '-' || vk == VK_SUBTRACT || vk == 0xBD) return '-'; // 0xBD = Win VK_OEM_MINUS
  if (vk == ':') return ':';                                    // mac ratio "4:1" (optional, parse ignores it)
  return 0;
}

// Locale-independent parse. MapEditChar normalises input to ASCII, so '.' is ALWAYS the
// decimal separator regardless of the C locale - atof/strtod would misread '.' under a
// comma locale (pl_PL/de_DE: "3.5" -> 3.0). Tolerant of a trailing unit (stops at the
// first non-numeric char): "12ms"->12, "4:1"->4, "-18.5"->-18.5, ".5"->0.5.
static double ParseEditValue(const char* s)
{
  while (*s == ' ') ++s;
  double sign = 1.0;
  if (*s == '-') { sign = -1.0; ++s; }
  else if (*s == '+') ++s;
  double val = 0.0;
  while (*s >= '0' && *s <= '9') { val = val * 10.0 + (double)(*s - '0'); ++s; }
  if (*s == '.') {
    ++s;
    double frac = 0.1;
    while (*s >= '0' && *s <= '9') { val += (double)(*s - '0') * frac; frac *= 0.1; ++s; }
  }
  return sign * val;
}

// Open the editor on param idx, seeding the buffer with the current displayed value so
// the user sees what they are replacing. m_editFresh makes the first accepted keystroke
// clear the seed (select-all-then-type feel); Backspace edits the seed in place instead.
void DynamicsPanel::BeginValueEdit(int idx)
{
  if (idx < 0 || idx >= NUM_SLIDERS) return;
  m_paramsChanged = false;   // editor is a clean transaction: only ITS own commit re-analyses
                             // (drops any stale pending flag so ESC/edit keys can't trigger one)
  m_editIdx = idx;
  m_editFresh = true;
  m_caretBlinkRef = NowSec();   // caret starts solid, then blinks
  const double v = (idx == 5 && m_params.autoMakeup) ? fabs(m_avgGR) : GetSliderValue(idx);
  std::snprintf(m_editBuf, sizeof(m_editBuf),
                SLIDER_DEFS[idx].precision == 1 ? "%.1f" : "%.0f", v);
  for (char* s = m_editBuf; *s; ++s) if (*s == ',') *s = '.';  // locale comma -> '.' (we only accept '.')
}

// Parse the buffer (ParseEditValue: locale-independent, unit-tolerant - "12ms"->12,
// "-18"->-18, "4:1"->4) and apply via SetSliderValue (clamps + clears autoMakeup for
// Makeup). Only commits when the text holds a digit, so a stray non-numeric entry cancels
// rather than snapping to 0. Returns true if the value actually changed. Always closes it.
bool DynamicsPanel::CommitValueEdit()
{
  if (m_editIdx < 0) return false;
  const int idx = m_editIdx;
  m_editIdx = -1;
  bool hasDigit = false;
  for (const char* s = m_editBuf; *s; ++s) if (*s >= '0' && *s <= '9') { hasDigit = true; break; }
  if (!hasDigit) return false;                                 // nothing usable -> treat as cancel
  const double oldVal = GetSliderValue(idx);
  const bool wasAuto = (idx == 5 && m_params.autoMakeup);
  SetSliderValue(idx, ParseEditValue(m_editBuf));              // clamps to the param range
  const bool changed = wasAuto || GetSliderValue(idx) != oldVal;
  if (changed) { m_paramsChanged = true; m_presetIdx = -1; }
  return changed;
}

void DynamicsPanel::CancelValueEdit()
{
  m_editIdx = -1;
}

// Double-click a knob (or a curve handle, which maps to its param: knee->Threshold,
// gate node->Gate Thr) to start typing an exact value. The editor renders on the param's
// knob cell (both the handle and its knob are on the active tab), so there is one edit
// surface. Returns true if editing started.
bool DynamicsPanel::OnDoubleClick(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  const int k = HitTestKnob(x, y, pr);
  if (k >= 0) { BeginValueEdit(k); return true; }
  const double S = EffScale();
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const DynLayout L = PanelLayout();
  URect handles[2];
  ComputeCurveHandles(L.plotWell, BuildCurveParams(), (int)m_tab, handles);
  if (handles[HANDLE_KNEE].contains(lx, ly)) { BeginValueEdit(0); return true; }   // knee -> Threshold
  if (handles[HANDLE_GATE].contains(lx, ly)) { BeginValueEdit(7); return true; }   // gate -> Gate Thr
  return false;
}

// Feed one key to the editor. Enter commits, ESC cancels, Backspace deletes a char,
// accepted chars append (first keystroke replaces the seeded value). Every key is
// consumed while editing so global shortcuts never fire mid-entry.
bool DynamicsPanel::OnEditKey(int vk)
{
  if (m_editIdx < 0) return false;
  if (vk == VK_RETURN) { CommitValueEdit(); return true; }
  if (vk == VK_ESCAPE) { CancelValueEdit(); return true; }
  if (vk == VK_BACK) {
    m_editFresh = false;
    m_caretBlinkRef = NowSec();   // keep the caret solid right after editing
    const size_t n = strlen(m_editBuf);
    if (n > 0) m_editBuf[n - 1] = '\0';
    return true;
  }
  const char c = MapEditChar(vk);
  if (c) {
    if (m_editFresh) { m_editBuf[0] = '\0'; m_editFresh = false; }
    const size_t n = strlen(m_editBuf);
    if (n < sizeof(m_editBuf) - 1) { m_editBuf[n] = c; m_editBuf[n + 1] = '\0'; }
    m_caretBlinkRef = NowSec();    // caret solid right after a keystroke, then blinks
  }
  return true;   // trap every key while editing
}

// True while any animation needs another frame: caret blink (editing) and the Live
// breathing pulse are continuous; tab-slide + value-ease are time-boxed. Polled by the
// host each timer tick - returns false when idle so there is no background repaint cost.
bool DynamicsPanel::WantsAnimationFrame() const
{
  if (!m_visible) return false;
  if (m_editIdx >= 0) return true;                 // caret blink
  if (m_liveMode)     return true;                 // Live breathing pulse
  // Transient animations: DrawPremium clears the start marker on the SAME frame it draws
  // the settled value, so gate on the marker being set (NOT on elapsed time). Gating on
  // duration stops the pump one tick BEFORE the final frame, freezing the animation just
  // short of its target (e.g. the tab-pill not fully reaching the active segment on return).
  if (m_tabSlideStartSec >= 0.0) return true;      // tab-slide in flight
  for (int i = 0; i < NUM_SLIDERS; ++i)
    if (m_knobEaseStartSec[i] > 0.0) return true;  // value-ease in flight
  return false;
}

#else   // GDI build: the inline type-value editor + motion are premium-only.

bool DynamicsPanel::OnDoubleClick(int, int, RECT) { return false; }
bool DynamicsPanel::OnEditKey(int) { return false; }
void DynamicsPanel::BeginValueEdit(int) {}
bool DynamicsPanel::CommitValueEdit() { return false; }
void DynamicsPanel::CancelValueEdit() {}
bool DynamicsPanel::WantsAnimationFrame() const { return false; }

#endif

// --- Drawing ---

void DynamicsPanel::Draw(HDC hdc, RECT wr)
{
  if (!m_visible) return;

  RECT pr = GetRect(wr);

  // Panel background
  {
    OwnedBrush bg(RGB(40, 40, 40));
    bg.Fill(hdc, &pr);
  }

  // Border
  {
    OwnedPen border(PS_SOLID, 1, RGB(80, 80, 80));
    DCPenScope scope(hdc, border);
    MoveToEx(hdc, pr.left, pr.top, nullptr);
    LineTo(hdc, pr.right - 1, pr.top);
    LineTo(hdc, pr.right - 1, pr.bottom - 1);
    LineTo(hdc, pr.left, pr.bottom - 1);
    LineTo(hdc, pr.left, pr.top);
  }

  SetBkMode(hdc, TRANSPARENT);

  // Title + GR meter
  {
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, RGB(255, 160, 40));
    RECT titleR = { pr.left + MARGIN + 2, pr.top + 4, pr.left + 90, pr.top + TITLE_H };
    DrawTextUTF8(hdc, "DYNAMICS", -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Preset button (right of title)
    {
      RECT presetR = GetPresetButtonRect(pr);
      OwnedPen presetPen(PS_SOLID, 1, RGB(100, 100, 100));
      DCPenScope presetScope(hdc, presetPen);
      MoveToEx(hdc, presetR.left, presetR.top, nullptr);
      LineTo(hdc, presetR.right - 1, presetR.top);
      LineTo(hdc, presetR.right - 1, presetR.bottom - 1);
      LineTo(hdc, presetR.left, presetR.bottom - 1);
      LineTo(hdc, presetR.left, presetR.top);
      SetTextColor(hdc, RGB(180, 180, 180));
      SelectObject(hdc, g_fonts.normal11);
      char presetLabel[32];
      if (m_presetIdx >= 0 && m_presetIdx < PRESET_COUNT)
        snprintf(presetLabel, sizeof(presetLabel), "%s \xE2\x96\xBE", g_dynamicsPresets[m_presetIdx].name);
      else
        snprintf(presetLabel, sizeof(presetLabel), "Preset \xE2\x96\xBE");
      DrawTextUTF8(hdc, presetLabel, -1, &presetR,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // GR meter bar (horizontal, right of preset button)
    if (m_avgGR < -0.1) {
      int meterX = pr.left + 204;
      int meterY = pr.top + 8;
      int meterMaxW = pr.right - 60 - meterX;
      double grNorm = std::min(1.0, fabs(m_avgGR) / 20.0); // 0..1 for 0..-20dB
      int meterW = (int)(grNorm * meterMaxW);

      OwnedBrush grBrush(RGB(220, 100, 40));
      RECT meterR = { meterX, meterY, meterX + meterW, meterY + 6 };
      grBrush.Fill(hdc, &meterR);

      // GR label
      SetTextColor(hdc, RGB(220, 100, 40));
      char grText[16];
      snprintf(grText, sizeof(grText), "%.1f dB", m_avgGR);
      RECT grLabelR = { meterX + meterW + 3, pr.top + 4, pr.right - 18, pr.top + TITLE_H };
      SelectObject(hdc, g_fonts.normal11);
      DrawTextUTF8(hdc, grText, -1, &grLabelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, oldFont);
  }

  // Close button [x]
  {
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, RGB(140, 140, 140));
    RECT closeR = GetCloseButtonRect(pr);
    DrawTextUTF8(hdc, "x", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }

  // Sliders
  for (int i = 0; i < NUM_SLIDERS; i++) {
    const auto& def = SLIDER_DEFS[i];
    int col = SliderCol(i);
    double val = GetSliderValue(i);
    RECT tr = GetSliderTrackRect(pr, i);
    int thumbX = ValueToPixel(val, tr, i);
    int cy = (tr.top + tr.bottom) / 2;

    // Label
    {
      HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);
      SetTextColor(hdc, RGB(160, 160, 160));
      int labelX = pr.left + ((col == 0) ? MARGIN : R_LABEL_X);
      RECT lr = { labelX, tr.top - 7, labelX + LABEL_W, tr.bottom + 7 };
      DrawTextUTF8(hdc, def.label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(hdc, oldFont);
    }

    // Track background
    {
      OwnedPen trackPen(PS_SOLID, TRACK_H, RGB(60, 60, 60));
      DCPenScope scope(hdc, trackPen);
      MoveToEx(hdc, tr.left, cy, nullptr);
      LineTo(hdc, tr.right, cy);
    }

    // Filled portion
    if (thumbX > tr.left) {
      OwnedPen fillPen(PS_SOLID, TRACK_H, RGB(255, 160, 40));
      DCPenScope scope(hdc, fillPen);
      MoveToEx(hdc, tr.left, cy, nullptr);
      LineTo(hdc, thumbX, cy);
    }

    // Thumb
    {
      bool active = (m_dragSlider == i);
      int r = active ? THUMB_R + 1 : THUMB_R;
      OwnedBrush thumbBr(RGB(220, 220, 220));
      OwnedPen thumbPen(PS_SOLID, 1, RGB(180, 180, 180));
      HBRUSH oldBr = (HBRUSH)SelectObject(hdc, (HBRUSH)thumbBr);
      HPEN oldPen = (HPEN)SelectObject(hdc, (HPEN)thumbPen);
      Ellipse(hdc, thumbX - r, cy - r, thumbX + r, cy + r);
      SelectObject(hdc, oldBr);
      SelectObject(hdc, oldPen);
    }

    // Value text
    {
      char text[32];
      if (i == 5 && m_params.autoMakeup) {
        // Makeup: show auto-computed value
        snprintf(text, sizeof(text), "%.1f auto", fabs(m_avgGR));
      } else if (def.precision == 1) {
        snprintf(text, sizeof(text), "%.1f %s", val, def.unit);
      } else {
        snprintf(text, sizeof(text), "%.0f %s", val, def.unit);
      }

      HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
      SetTextColor(hdc, RGB(220, 220, 220));
      int valX = pr.left + ((col == 0) ? L_VALUE_X : R_VALUE_X);
      RECT vr = { valX, tr.top - 7, pr.right - MARGIN, tr.bottom + 7 };
      DrawTextUTF8(hdc, text, -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(hdc, oldFont);
    }
  }

  // Bottom row: Peak/RMS toggle + Apply button
  {
    // Peak/RMS toggle
    RECT rmsR = GetRmsToggleRect(pr);
    {
      OwnedPen btnPen(PS_SOLID, 1, m_params.rmsMode ? RGB(255, 160, 40) : RGB(80, 80, 80));
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, rmsR.left, rmsR.top, nullptr);
      LineTo(hdc, rmsR.right - 1, rmsR.top);
      LineTo(hdc, rmsR.right - 1, rmsR.bottom - 1);
      LineTo(hdc, rmsR.left, rmsR.bottom - 1);
      LineTo(hdc, rmsR.left, rmsR.top);
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, m_params.rmsMode ? RGB(255, 160, 40) : RGB(160, 160, 160));
    DrawTextUTF8(hdc, m_params.rmsMode ? "RMS" : "Peak", -1, &rmsR,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Dyn toggle
    RECT dynR = GetDynToggleRect(pr);
    {
      COLORREF col = m_showDyn ? RGB(200, 130, 50) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, dynR.left, dynR.top, nullptr);
      LineTo(hdc, dynR.right - 1, dynR.top);
      LineTo(hdc, dynR.right - 1, dynR.bottom - 1);
      LineTo(hdc, dynR.left, dynR.bottom - 1);
      LineTo(hdc, dynR.left, dynR.top);
    }
    SetTextColor(hdc, m_showDyn ? RGB(200, 130, 50) : RGB(80, 80, 80));
    DrawTextUTF8(hdc, "Dyn", -1, &dynR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Env toggle
    RECT envR = GetEnvToggleRect(pr);
    {
      COLORREF col = m_showEnv ? RGB(0, 180, 220) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, envR.left, envR.top, nullptr);
      LineTo(hdc, envR.right - 1, envR.top);
      LineTo(hdc, envR.right - 1, envR.bottom - 1);
      LineTo(hdc, envR.left, envR.bottom - 1);
      LineTo(hdc, envR.left, envR.top);
    }
    SetTextColor(hdc, m_showEnv ? RGB(0, 180, 220) : RGB(80, 80, 80));
    DrawTextUTF8(hdc, "Env", -1, &envR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Live toggle
    RECT liveR = GetLiveToggleRect(pr);
    {
      COLORREF col = m_liveMode ? RGB(100, 220, 100) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, liveR.left, liveR.top, nullptr);
      LineTo(hdc, liveR.right - 1, liveR.top);
      LineTo(hdc, liveR.right - 1, liveR.bottom - 1);
      LineTo(hdc, liveR.left, liveR.bottom - 1);
      LineTo(hdc, liveR.left, liveR.top);
    }
    SetTextColor(hdc, m_liveMode ? RGB(100, 220, 100) : RGB(80, 80, 80));
    DrawTextUTF8(hdc, "Live", -1, &liveR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // GR shading toggle
    RECT grR = GetGRToggleRect(pr);
    {
      COLORREF col = m_showGR ? RGB(180, 60, 40) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, grR.left, grR.top, nullptr);
      LineTo(hdc, grR.right - 1, grR.top);
      LineTo(hdc, grR.right - 1, grR.bottom - 1);
      LineTo(hdc, grR.left, grR.bottom - 1);
      LineTo(hdc, grR.left, grR.top);
    }
    SetTextColor(hdc, m_showGR ? RGB(180, 60, 40) : RGB(80, 80, 80));
    DrawTextUTF8(hdc, "GR", -1, &grR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // A/B bypass toggle
    RECT abR = GetABToggleRect(pr);
    {
      COLORREF col = m_bypassed ? RGB(220, 180, 50) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, abR.left, abR.top, nullptr);
      LineTo(hdc, abR.right - 1, abR.top);
      LineTo(hdc, abR.right - 1, abR.bottom - 1);
      LineTo(hdc, abR.left, abR.bottom - 1);
      LineTo(hdc, abR.left, abR.top);
    }
    SetTextColor(hdc, m_bypassed ? RGB(220, 180, 50) : RGB(80, 80, 80));
    DrawTextUTF8(hdc, "A/B", -1, &abR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Apply button (grayed out during Live - points already written in real-time)
    RECT ar = GetApplyButtonRect(pr);
    {
      COLORREF applyCol = m_liveMode ? RGB(70, 70, 70) : RGB(255, 160, 40);
      OwnedPen btnPen(PS_SOLID, 1, applyCol);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, ar.left, ar.top, nullptr);
      LineTo(hdc, ar.right - 1, ar.top);
      LineTo(hdc, ar.right - 1, ar.bottom - 1);
      LineTo(hdc, ar.left, ar.bottom - 1);
      LineTo(hdc, ar.left, ar.top);
    }
    SetTextColor(hdc, m_liveMode ? RGB(70, 70, 70) : RGB(255, 160, 40));
    DrawTextUTF8(hdc, "Apply", -1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }
}

// --- Premium panel (Blend2D, Phase 2) --------------------------------------
// Builds a pure-data DynPanelVM from live params and hands it to the renderer.
// Drawn from OnPaintOverlay on the REAL window DC (HiDPI). Knobs/tabs/interaction
// land in Inc 4/5; this skeleton renders the slab + live transfer plot + GR meter.

void DynamicsPanel::DrawPremium(HDC hdc, RECT wr, double dpr)
{
  if (!m_visible) return;
  RECT pr = GetRect(wr);

  DynPanelVM vm;
  vm.curve      = BuildCurveParams();               // shared with curve-handle hit-testing
  vm.activeTab  = (int)m_tab;
  vm.presetName = (m_presetIdx >= 0 && m_presetIdx < PRESET_COUNT)
                    ? g_dynamicsPresets[m_presetIdx].name : nullptr;
  vm.showDyn  = m_showDyn;
  vm.showEnv  = m_showEnv;
  vm.showGR   = m_showGR;
  vm.liveMode = m_liveMode;
  vm.bypassed = m_bypassed;
  vm.rmsMode  = m_params.rmsMode;
  vm.meterFloorSel = m_meterFloorSel;
  vm.compact  = m_compactMode;
  vm.dragHandle = m_dragHandle;
  vm.hoverHandle = m_hoverHandle;

  const double now = NowSec();

  // Per-knob render state. norm/defaultNorm in [0,1]; Threshold's default tracks
  // the operating point (avgPeak). Makeup shows "auto" + the computed value when
  // autoMakeup is on. Gate params (>=7) tint violet.
  for (int i = 0; i < NUM_SLIDERS; ++i) {
    const auto& def = SLIDER_DEFS[i];
    const double range = (def.maxVal - def.minVal) != 0.0 ? (def.maxVal - def.minVal) : 1.0;
    auto norm = [&](double v) { return std::max(0.0, std::min(1.0, (v - def.minVal) / range)); };
    double dflt = def.defaultVal;
    if (i == 0 && dflt <= -99.0) dflt = m_avgPeakDb;   // threshold default = operating point
    KnobVM& k = vm.knobs[i];
    k.value       = GetSliderValue(i);
    double targetN = norm(k.value);
    k.defaultNorm = norm(dflt);
    k.label       = def.label;
    k.unit        = def.unit;
    k.precision   = def.precision;
    k.isGate      = (i >= 7);
    k.showAuto    = (i == 5 && m_params.autoMakeup);
    if (k.showAuto) { k.value = fabs(m_avgGR); targetN = norm(k.value); }

    // Value-ease (motion pass): the arc + indicator glide ~120ms to a value set by
    // wheel/type/reset; a drag (or active edit) on THIS knob is direct (no lag). Change
    // is auto-detected by watching targetN, so no value-change site needs hooking.
    double dispN;
    if (!m_motionInit || i == m_dragSlider || i == m_editIdx) {
      dispN = targetN; m_knobEaseFrom[i] = targetN; m_knobEaseStartSec[i] = 0.0;
    } else {
      if (targetN != m_knobTargetNorm[i]) {            // target jumped -> (re)start an ease
        double cur = m_knobTargetNorm[i];              // displayed value at the interruption
        if (m_knobEaseStartSec[i] > 0.0) {
          const double t = (now - m_knobEaseStartSec[i]) / kValueEaseSec;
          if (t < 1.0) { const double e = 1.0 - std::pow(1.0 - t, 3.0);
                         cur = m_knobEaseFrom[i] + (m_knobTargetNorm[i] - m_knobEaseFrom[i]) * e; }
        }
        m_knobEaseFrom[i] = cur; m_knobEaseStartSec[i] = now;
      }
      if (m_knobEaseStartSec[i] > 0.0) {
        const double t = (now - m_knobEaseStartSec[i]) / kValueEaseSec;
        if (t >= 1.0) { dispN = targetN; m_knobEaseStartSec[i] = 0.0; }
        else { const double e = 1.0 - std::pow(1.0 - t, 3.0);
               dispN = m_knobEaseFrom[i] + (targetN - m_knobEaseFrom[i]) * e; }
      } else dispN = targetN;
    }
    m_knobTargetNorm[i] = targetN;
    k.norm = dispN;
  }
  m_motionInit = true;

  // Glow the knob under the cursor (hover) and the one being dragged (active).
  if (m_hoverKnob >= 0 && m_hoverKnob < NUM_SLIDERS) vm.knobs[m_hoverKnob].hover = true;
  if (m_dragSlider >= 0 && m_dragSlider < NUM_SLIDERS) vm.knobs[m_dragSlider].hover = true;
  // Inline editor: the target knob renders the edit box + caret instead of its readout.
  // The caret blinks (~1s on/off); m_caretBlinkRef is reset on each keystroke (solid then blinks).
  if (m_editIdx >= 0 && m_editIdx < NUM_SLIDERS) {
    vm.knobs[m_editIdx].editing  = true;
    vm.knobs[m_editIdx].editText = m_editBuf;
    vm.knobs[m_editIdx].caretOn  = std::fmod(now - m_caretBlinkRef, kCaretBlinkSec) < kCaretBlinkSec * 0.5;
  }

  // Tab-slide: the active pill fill glides from m_tabFrom to the current tab over ~180ms.
  vm.tabFrom = m_tabFrom;
  if (m_tabSlideStartSec >= 0.0) {
    const double t = (now - m_tabSlideStartSec) / kTabSlideSec;
    if (t >= 1.0) { vm.tabSlideT = 1.0; m_tabSlideStartSec = -1.0; }  // draw the settled frame, then stop the pump
    else vm.tabSlideT = 1.0 - std::pow(1.0 - t, 3.0);
  } else vm.tabSlideT = 1.0;

  // Live-pill breathing pulse (~1.5s): glow intensity oscillates [0.45..1] while armed.
  if (m_liveMode) {
    const double ph = std::sin(2.0 * 3.14159265358979323846 * now / kLivePulseSec);
    vm.livePulse = 0.45 + 0.55 * (0.5 * (1.0 + ph));
  }

  m_canvas.RenderPanel(hdc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, dpr, vm);
}
