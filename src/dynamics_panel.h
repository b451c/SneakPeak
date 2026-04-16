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
  bool IsDragging() const { return m_dragSlider >= 0 || m_panelDragging; }

  // For GR meter display
  void SetAvgGainReduction(double gr) { m_avgGR = gr; }

  // Overlay visibility toggles (read by SneakPeak in OnPaint)
  bool GetShowDyn() const { return m_showDyn; }
  bool GetShowEnv() const { return m_showEnv; }
  void SetShowDyn(bool v) { m_showDyn = v; }
  void SetShowEnv(bool v) { m_showEnv = v; }

private:
  static constexpr int NUM_SLIDERS = 6;

  struct SliderDef {
    const char* label;
    double minVal, maxVal;
    const char* unit;
    int precision;
  };
  static const SliderDef SLIDER_DEFS[NUM_SLIDERS];

  double GetSliderValue(int idx) const;
  void SetSliderValue(int idx, double val);

  static int SliderCol(int idx) { return (idx < 3) ? 0 : 1; }
  static int SliderRow(int idx) { return (idx < 3) ? idx : idx - 3; }

  RECT GetSliderTrackRect(RECT panelRect, int idx) const;
  RECT GetApplyButtonRect(RECT panelRect) const;
  RECT GetRmsToggleRect(RECT panelRect) const;
  RECT GetDynToggleRect(RECT panelRect) const;
  RECT GetEnvToggleRect(RECT panelRect) const;
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
  bool m_showDyn = true;  // show dynamics curves (orange + purple + threshold)
  bool m_showEnv = true;  // show volume envelope (cyan)

  // Panel dragging
  bool m_panelDragging = false;
  int m_dragOffsetX = 0, m_dragOffsetY = 0;
  int m_offsetX = 0, m_offsetY = 0;

  static const int PANEL_W = 380;
  static const int PANEL_H = 106;
  static const int TITLE_H = 22;
  static const int ROW_H = 18;
  static const int THUMB_R = 4;
  static const int TRACK_H = 2;
};
