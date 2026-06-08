// dynamics_panel.h — Professional dynamics control panel with real-time sliders
#pragma once

#include "platform.h"
#include "dynamics_engine.h"
#include "ui_render.h"

class DynamicsPanel {
public:
  void Draw(HDC hdc, RECT waveformRect);
  void DrawPremium(HDC hdc, RECT waveformRect, double dpr);  // Blend2D panel (Phase 2)
  bool HitTest(int x, int y, RECT waveformRect) const;
  RECT GetRect(RECT waveformRect) const;

  bool OnMouseDown(int x, int y, RECT waveformRect);
  void OnMouseMove(int x, int y, RECT waveformRect);
  void OnMouseUp();
  bool OnMouseWheel(int x, int y, double steps, bool fine, RECT waveformRect);  // nudge knob under cursor (premium)
  bool OnHover(int x, int y, RECT waveformRect);              // update hovered knob; true if it changed (premium)
  bool IsOverResizeGrip(int x, int y, RECT waveformRect) const;  // for the diagonal resize cursor (premium)

  // Inline type-value editor (Inc 8, premium): double-click a knob or curve handle
  // to type an exact value; ESC cancels, Enter commits. Keys arrive via the SWS
  // accelerator (see SneakPeak::HandleDynamicsEditKey), so OnEditKey takes a VK/char.
  bool OnDoubleClick(int x, int y, RECT waveformRect);  // begin editing the knob/handle under the cursor; true if started
  bool IsEditingValue() const { return m_editIdx >= 0; }
  bool OnEditKey(int vk);  // feed one key to the editor; true = consumed (commit sets ParamsChanged)
  bool CommitValueEdit();  // parse the buffer + apply via SetSliderValue; true if the value changed (host commits on click)

  // Motion pass (premium): true while any animation is in flight (caret blink, Live
  // breathing pulse, tab-slide, knob value-ease). The host polls this each timer tick
  // and repaints only when true - zero idle cost.
  bool WantsAnimationFrame() const;

  bool IsVisible() const { return m_visible; }
  void Show(const DynamicsParams& params, double avgPeakDb);
  void Hide();

  const DynamicsParams& GetParams() const { return m_params; }
  bool ParamsChanged() const { return m_paramsChanged; }
  void ClearParamsChanged() { m_paramsChanged = false; }
  bool ApplyRequested() const { return m_applyRequested; }
  void ClearApplyRequested() { m_applyRequested = false; }
  bool PresetMenuRequested() const { return m_presetMenuRequested; }
  void ClearPresetMenuRequested() { m_presetMenuRequested = false; }
  void ApplyPreset(int presetIdx);
  void ApplyParams(const DynamicsParams& p);  // load arbitrary params (user preset); marks custom + changed
  RECT GetPresetButtonRect(RECT panelRect) const;
  bool IsDragging() const { return m_dragSlider >= 0 || m_panelDragging || m_resizing || m_dragHandle >= 0; }

  // For GR meter display
  void SetAvgGainReduction(double gr) { m_avgGR = gr; }

  // Overlay visibility toggles (read by SneakPeak in OnPaint)
  bool GetShowDyn() const { return m_showDyn; }
  bool GetShowEnv() const { return m_showEnv; }
  bool GetShowGR() const { return m_showGR; }
  bool GetBypassed() const { return m_bypassed; }
  void SetShowDyn(bool v) { m_showDyn = v; }
  void SetShowEnv(bool v) { m_showEnv = v; }
  void SetShowGR(bool v) { m_showGR = v; }
  // Meter-scale selector (View tab, premium): the plot/GR-meter dB floor.
  // 0 = -60 dB (default), 1 = -36, 2 = -24. Render-only; persisted as a global pref.
  int  GetMeterFloor() const { return m_meterFloorSel; }
  void SetMeterFloor(int idx) { m_meterFloorSel = idx < 0 ? 0 : (idx > 2 ? 2 : idx); }
  // Set when a Dyn/Env/GR overlay toggle is clicked, so the host can persist them
  // as global user prefs (restored on the next panel open). Live/A-B are NOT persisted.
  bool ViewPrefsChanged() const { return m_viewPrefsChanged; }
  void ClearViewPrefsChanged() { m_viewPrefsChanged = false; }

  // Live preview mode: write envelope points on every slider change
  bool IsLive() const { return m_liveMode; }
  void SetLiveMode(bool v) { m_liveMode = v; }   // host restores the persisted Live state after open
  bool LiveUndoOpen() const { return m_liveUndoOpen; }
  void SetLiveUndoOpen(bool v) { m_liveUndoOpen = v; }

private:
  enum class Tab { Compressor, Gate, View };   // premium panel active tab
  static constexpr int NUM_SLIDERS = 10;
  static constexpr int HANDLE_KNEE = 0;        // curve drag-handle ids
  static constexpr int HANDLE_GATE = 1;

  struct SliderDef {
    const char* label;
    double minVal, maxVal;
    const char* unit;
    int precision;
    double defaultVal;   // knob default-tick + Cmd-reset target (Inc 4); -100 = use avgPeak
  };
  static const SliderDef SLIDER_DEFS[NUM_SLIDERS];

  double GetSliderValue(int idx) const;
  void SetSliderValue(int idx, double val);

  static int SliderCol(int idx) { return (idx < 3 || idx == 6 || idx == 8) ? 0 : 1; }
  static int SliderRow(int idx) {
    if (idx < 3) return idx;       // 0-2: left rows 0-2
    if (idx < 6) return idx - 3;   // 3-5: right rows 0-2
    if (idx == 6) return 3;        // L.ahead: left row 3
    if (idx == 7) return 3;        // G.Thr: right row 3
    if (idx == 8) return 4;        // G.Range: left row 4
    return 4;                      // G.Hold: right row 4
  }

  RECT GetSliderTrackRect(RECT panelRect, int idx) const;
  RECT GetApplyButtonRect(RECT panelRect) const;
  RECT GetRmsToggleRect(RECT panelRect) const;
  RECT GetDynToggleRect(RECT panelRect) const;
  RECT GetEnvToggleRect(RECT panelRect) const;
  RECT GetLiveToggleRect(RECT panelRect) const;
  RECT GetGRToggleRect(RECT panelRect) const;
  RECT GetABToggleRect(RECT panelRect) const;
  RECT GetCloseButtonRect(RECT panelRect) const;
  int HitTestSlider(int x, int y, RECT panelRect) const;
  int HitTestKnob(int x, int y, RECT panelRect) const;    // knob under cursor in base coords, or -1 (premium)
  DynCurveParams BuildCurveParams() const;                // VM curve params from live state (render + handles)
  void DragCurveHandle(int x, int y, RECT panelRect);     // apply a curve-handle drag to params (premium)
  void BeginValueEdit(int idx);                           // open the inline editor on param idx (seed buffer)
  void CancelValueEdit();                                 // discard + close the inline editor
  bool OnMouseDownPremium(int x, int y, RECT panelRect);  // Blend2D panel hit-routing (Inc 3c)
  double PixelToValue(int px, RECT trackRect, int idx) const;
  int ValueToPixel(double val, RECT trackRect, int idx) const;
  static bool IsFineMode();

  bool m_visible = false;
  DynamicsParams m_params;
  double m_avgPeakDb = -18.0;
  double m_avgGR = 0.0;
  Tab m_tab = Tab::Compressor;   // active tab (premium panel)
  UiCanvas m_canvas;             // Blend2D renderer for the premium panel

  int m_dragSlider = -1;
  int m_hoverKnob = -1;       // premium: knob under the cursor (glow); -1 = none
  int m_hoverHandle = -1;     // premium: curve handle under the cursor (lights accent); -1 = none
  [[maybe_unused]] int m_dragHandle = -1;      // premium: curve handle being dragged (-1/0=knee/1=gate)
  [[maybe_unused]] int m_handleStartY = 0;     // grab Y for the relative secondary (ratio/range)
  [[maybe_unused]] double m_handleStartVal = 0.0;  // secondary param value at grab
  [[maybe_unused]] double m_handleGrabDx = 0.0;    // base-px offset cursor->handle center (no X jump on grab)
  int m_dragGrabOffset = 0;   // pixel offset: thumbX - clickX (prevents jump on grab)
  int m_dragStartX = 0;       // mouse X at drag start (for fine mode delta)
  [[maybe_unused]] int m_dragStartY = 0;  // mouse Y at knob-drag start (premium-only)
  [[maybe_unused]] int m_dragLastY = 0;   // mouse Y last move (premium velocity drag)
  double m_dragStartVal = 0.0; // slider value at drag start (for fine mode delta)

  // Inline type-value editor (Inc 8). [[maybe_unused]]: premium build only.
  [[maybe_unused]] int  m_editIdx = -1;       // param being typed (0..9), -1 = not editing
  [[maybe_unused]] bool m_editFresh = false;  // true until the first keystroke (then the seeded text is cleared)
  [[maybe_unused]] char m_editBuf[16] = {};   // current edit text (digits/./-)

  // Motion pass. All times are steady_clock seconds (NowSec()). [[maybe_unused]]: premium only.
  [[maybe_unused]] bool   m_motionInit = false;          // first paint seeds the ease arrays (no spurious ease on open)
  [[maybe_unused]] double m_caretBlinkRef = 0.0;         // caret blink phase ref (reset on each keystroke -> solid then blinks)
  [[maybe_unused]] double m_tabSlideStartSec = -1.0;     // tab-slide start (<0 = none); fill glides m_tabFrom -> m_tab
  [[maybe_unused]] int    m_tabFrom = 0;                 // tab the slide animates from
  [[maybe_unused]] double m_knobEaseFrom[NUM_SLIDERS]    = {}; // arc norm at the start of the current ease
  [[maybe_unused]] double m_knobTargetNorm[NUM_SLIDERS]  = {}; // last target norm (detect a non-drag change)
  [[maybe_unused]] double m_knobEaseStartSec[NUM_SLIDERS] = {}; // value-ease start (0 = settled)

  bool m_paramsChanged = false;
  bool m_applyRequested = false;
  bool m_presetMenuRequested = false;
  bool m_viewPrefsChanged = false;  // a Dyn/Env/GR overlay toggle changed -> host persists
  int m_presetIdx = -1;  // -1 = custom, 0..PRESET_COUNT-1 = built-in
  bool m_showDyn = true;  // show dynamics curves (orange + purple + threshold)
  bool m_showEnv = true;  // show volume envelope (cyan)
  bool m_showGR = true;   // show gain reduction shading between curves
  int  m_meterFloorSel = 0;  // meter-scale dB floor: 0=-60 (default), 1=-36, 2=-24; render-only, persisted
  bool m_bypassed = false;  // A/B: envelope bypass for comparison
  bool m_liveMode = false;  // live preview: write envelope on slider change
  bool m_liveUndoOpen = false; // undo block is open for live session

  // Panel dragging
  bool m_panelDragging = false;
  int m_dragOffsetX = 0, m_dragOffsetY = 0;
  int m_offsetX = 0, m_offsetY = 0;

  // Premium panel free-resize (aspect-locked uniform scale; bottom-right grip).
  // [[maybe_unused]]: only the premium build references these (OFF build = GDI).
  [[maybe_unused]] double m_uiScale = 1.0;       // 1.0 = default 480x300; clamped [0.8, 2.0]
  [[maybe_unused]] bool m_resizing = false;
  [[maybe_unused]] int m_resizeAnchorL = 0;      // fixed top-left during a resize drag
  [[maybe_unused]] int m_resizeAnchorT = 0;
  [[maybe_unused]] int m_resizeStartX = 0;       // cursor X at grab (relative-delta scaling)
  [[maybe_unused]] double m_resizeStartScale = 1.0;  // m_uiScale at grab -> first move = no-op

  static const int PANEL_W = 380;
  static const int PANEL_H = 148;
  static const int TITLE_H = 22;
  static const int ROW_H = 18;
  static const int THUMB_R = 4;
  static const int TRACK_H = 2;
};
