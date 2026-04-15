// dynamics_panel.h — Inline dynamics control panel with real-time sliders
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

private:
  static constexpr int NUM_SLIDERS = 5;

  struct SliderDef {
    const char* label;
    double minVal, maxVal;
    const char* unit;
    int precision; // decimal places
  };
  static const SliderDef SLIDER_DEFS[NUM_SLIDERS];

  double GetSliderValue(int idx) const;
  void SetSliderValue(int idx, double val);

  // Layout: slider index -> column (0=left, 1=right) and row (0-2)
  static int SliderCol(int idx) { return (idx < 3) ? 0 : 1; }
  static int SliderRow(int idx) { return (idx < 3) ? idx : idx - 3; }

  RECT GetSliderTrackRect(RECT panelRect, int idx) const;
  RECT GetApplyButtonRect(RECT panelRect) const;
  RECT GetCloseButtonRect(RECT panelRect) const;
  int HitTestSlider(int x, int y, RECT panelRect) const;
  double PixelToValue(int px, RECT trackRect, int idx) const;
  int ValueToPixel(double val, RECT trackRect, int idx) const;
  static bool IsFineMode();

  bool m_visible = false;
  DynamicsParams m_params;
  double m_avgPeakDb = -18.0;

  int m_dragSlider = -1;
  bool m_paramsChanged = false;
  bool m_applyRequested = false;

  // Panel dragging
  bool m_panelDragging = false;
  int m_dragOffsetX = 0, m_dragOffsetY = 0;
  int m_offsetX = 0, m_offsetY = 0;

  static const int PANEL_W = 320;
  static const int PANEL_H = 76;
  static const int TITLE_H = 14;
  static const int ROW_H = 18;
  static const int THUMB_R = 4;
  static const int TRACK_H = 2;
};
