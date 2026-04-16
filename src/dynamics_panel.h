// dynamics_panel.h — Professional dynamics control panel with real-time sliders
#pragma once

#include "platform.h"
#include "dynamics_engine.h"

class DynamicsPanel {
public:
  void Draw(HDC hdc, RECT waveformRect);
  bool HitTest(int x, int y, RECT waveformRect) const;
  RECT GetRect(RECT waveformRect) const;

  bool OnMouseDown(int x, int y, RECT waveformRect);
  void OnMouseMove(int x, int y, RECT waveformRect);
  void OnMouseUp();

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
  RECT GetPresetButtonRect(RECT panelRect) const;
  bool IsDragging() const { return m_dragSlider >= 0 || m_panelDragging; }

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

  // Live preview mode: write envelope points on every slider change
  bool IsLive() const { return m_liveMode; }
  bool LiveUndoOpen() const { return m_liveUndoOpen; }
  void SetLiveUndoOpen(bool v) { m_liveUndoOpen = v; }

private:
  static constexpr int NUM_SLIDERS = 10;

  struct SliderDef {
    const char* label;
    double minVal, maxVal;
    const char* unit;
    int precision;
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
  double PixelToValue(int px, RECT trackRect, int idx) const;
  int ValueToPixel(double val, RECT trackRect, int idx) const;
  static bool IsFineMode();

  bool m_visible = false;
  DynamicsParams m_params;
  double m_avgPeakDb = -18.0;
  double m_avgGR = 0.0;

  int m_dragSlider = -1;
  int m_dragGrabOffset = 0;   // pixel offset: thumbX - clickX (prevents jump on grab)
  int m_dragStartX = 0;       // mouse X at drag start (for fine mode delta)
  double m_dragStartVal = 0.0; // slider value at drag start (for fine mode delta)
  bool m_paramsChanged = false;
  bool m_applyRequested = false;
  bool m_presetMenuRequested = false;
  int m_presetIdx = -1;  // -1 = custom, 0..PRESET_COUNT-1 = built-in
  bool m_showDyn = true;  // show dynamics curves (orange + purple + threshold)
  bool m_showEnv = true;  // show volume envelope (cyan)
  bool m_showGR = true;   // show gain reduction shading between curves
  bool m_bypassed = false;  // A/B: envelope bypass for comparison
  bool m_liveMode = false;  // live preview: write envelope on slider change
  bool m_liveUndoOpen = false; // undo block is open for live session

  // Panel dragging
  bool m_panelDragging = false;
  int m_dragOffsetX = 0, m_dragOffsetY = 0;
  int m_offsetX = 0, m_offsetY = 0;

  static const int PANEL_W = 380;
  static const int PANEL_H = 148;
  static const int TITLE_H = 22;
  static const int ROW_H = 18;
  static const int THUMB_R = 4;
  static const int TRACK_H = 2;
};
