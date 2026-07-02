// limiter_panel.h — premium HARD LIMITER overlay (v2.4.0 INC-L1)
//
// Standalone-mode front end for limiter_engine (true-peak hard limiter):
// 5 knobs (Gain/Ceiling/Attack/Hold/Release), TRUE PEAK + LINK pills, preview
// readouts (in/out peak, max GR), GR strip, factory presets, Apply. Premium
// (Blend2D) only - in the OFF build Show() is a no-op and the menu entry is
// grayed, mirroring the Settings panel's premium-only reality.
//
// Host contract (mirrors DynamicsPanel/SettingsPanel): OnMouseDown returns
// true when the click landed on the panel; the host then polls the one-shot
// flags (ApplyRequested / PresetMenuRequested / ParamsChanged / GeomChanged).
// The knob drag / wheel nudge / double-click type-in mechanics are copied from
// DynamicsPanel so the two panels feel identical.
#pragma once

#include "platform.h"
#include "ui_render.h"
#include "limiter_engine.h"

// Factory presets (plan C7); user presets live in ExtState (host-managed).
struct LimiterPreset {
  const char* name;
  LimiterParams params;
};
constexpr int kLimPresetCount = 4;
extern const LimiterPreset g_limiterPresets[kLimPresetCount];

// Locale-safe LimiterParams serialization for the user-preset blob: values
// stored x1000 as integers (never %f/atof - a comma locale breaks those),
// bools as 0/1. Absent keys keep the field defaults (forward-compatible).
void LimiterParamsToString(const LimiterParams& p, char* buf, int bufSize);
bool LimiterParamsFromString(const char* str, LimiterParams& out);

class LimiterPanel {
public:
  bool IsVisible() const { return m_visible; }
  void Show();   // premium-only: no-op in the OFF build
  void Hide();

  RECT GetRect(RECT waveformRect) const;
  bool HitTest(int x, int y, RECT waveformRect) const;
  void DrawPremium(HDC hdc, RECT waveformRect, double dpr);

  bool OnMouseDown(int x, int y, RECT waveformRect);  // true = consumed
  void OnMouseMove(int x, int y, RECT waveformRect);
  void OnMouseUp();
  bool OnHover(int x, int y, RECT waveformRect);      // true = hover changed (repaint)
  bool OnMouseWheel(int x, int y, double steps, bool fine, RECT waveformRect);
  bool OnDoubleClick(int x, int y, RECT waveformRect);
  bool IsDragging() const { return m_dragKnob >= 0 || m_panelDragging; }

  // Inline type-value editor (DynamicsPanel Inc-8 mechanics; caret non-blinking).
  bool IsEditingValue() const { return m_editIdx >= 0; }
  bool OnEditKey(int vk);       // true = consumed (always, while editing)
  bool CommitValueEdit();       // true = the value actually changed

  // One-shot flags, polled by the host after a consumed interaction.
  bool ApplyRequested()      { bool v = m_applyRequested; m_applyRequested = false; return v; }
  bool PresetMenuRequested() { bool v = m_presetMenuReq; m_presetMenuReq = false; return v; }
  bool ParamsChanged() const { return m_paramsChanged; }
  void ClearParamsChanged()  { m_paramsChanged = false; }
  bool GeomChanged() const   { return m_geomChanged; }
  void ClearGeomChanged()    { m_geomChanged = false; }

  const LimiterParams& GetParams() const { return m_params; }
  void SetParams(const LimiterParams& p);   // clamps each field to its knob range
  void ApplyPreset(int idx);
  int  GetPresetIdx() const { return m_presetIdx; }
  // A loaded USER preset shows its name in the preset box (any knob change
  // clears it back to "Preset", exactly like the factory-name behavior).
  void SetUserPresetName(const char* name);
  RECT GetPresetButtonRect(RECT waveformRect) const;  // screen coords (menu anchor)

  void SetMono(bool mono) { m_mono = mono; }
  void SetPanelOffset(int ox, int oy) { m_offsetX = ox; m_offsetY = oy; }
  int  GetPanelOffsetX() const { return m_offsetX; }
  int  GetPanelOffsetY() const { return m_offsetY; }

  // Background Apply progress: >= 0 turns the Apply button into a progress
  // bar (the user watches the panel, not the window title); -1 = idle.
  void SetApplyProgress(int pct) { m_applyPct = pct; }
  bool IsApplyBusy() const { return m_applyPct >= 0; }

  // Preview stats from the host worker -> readouts; pending renders "..." (or
  // "N%" while the full pass reports progress - podcast-length files).
  // outPending: draft result (no output measure yet) - OUT alone shows "...".
  void SetPreviewStats(double inDb, double outDb, double grDb, bool outPending);
  void SetStatsPending(bool pending, int pct = -1)
  {
    m_statsPending = pending;
    m_statsPct = pending ? pct : -1;
  }

private:
  double EffScale(RECT waveformRect) const;  // g_uiScale, fit-clamped to the rect
  int    HitId(double lx, double ly) const;  // base-coord point -> LimiterHit id
  double GetValue(int idx) const;
  void   SetValue(int idx, double v);        // clamps to the knob range
  void   BeginValueEdit(int idx);
  void   CancelValueEdit() { m_editIdx = -1; }

  bool m_visible = false;
  LimiterParams m_params;
  int  m_presetIdx = 0;          // 0..3 factory; -2 user preset (m_userName); -1 custom
  char m_userName[64] = { 0 };   // loaded user-preset name (valid when idx == -2)
  bool m_mono = false;

  int  m_offsetX = 0, m_offsetY = 0;   // panel-drag offsets (persisted by host)
  int  m_dragKnob = -1, m_dragLastY = 0;
  bool m_panelDragging = false;
  int  m_dragOffX = 0, m_dragOffY = 0;
  bool m_geomChanged = false;
  int  m_hover = LIM_HIT_NONE;

  bool m_applyRequested = false;
  bool m_presetMenuReq = false;
  bool m_paramsChanged = false;

  int  m_editIdx = -1;
  char m_editBuf[24] = { 0 };
  bool m_editFresh = false;

  double m_inDb = -999.0, m_outDb = -999.0, m_grDb = 0.0;
  bool m_statsValid = false, m_statsPending = false, m_outPending = false;
  int m_statsPct = -1;   // full-pass progress while pending (-1 = plain "...")
  int m_applyPct = -1;   // background-apply progress (-1 = idle)
#ifdef SNEAKPEAK_BLEND2D_PANEL
  char m_inText[24] = { 0 }, m_outText[24] = { 0 }, m_grText[24] = { 0 };
#endif

  UiCanvas m_canvas;
};
