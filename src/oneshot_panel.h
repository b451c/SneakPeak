// oneshot_panel.h — premium ONE-SHOT PREP overlay (v2.4 INC-B1)
//
// The "10-30 variations per SFX" workflow, first increment: one panel that
// trims silence, applies the industry edge micro-fades (5 ms in / 20 ms out)
// and normalizes to a target, then writes the prepared WAV next to the
// source. Slicing to variations + naming patterns arrive in INC-B2.
// Premium-only (LimiterPanel pattern); host contract identical: OnMouseDown
// returns true when consumed, host polls the one-shot flags.
#pragma once

#include "platform.h"
#include "ui_render.h"

struct OneShotParams {
  bool trimEnable = true;
  double trimThreshDb = -60.0;  // silence threshold
  double trimPadMs = 10.0;      // keep-padding around the kept region
  double fadeInMs = 5.0;        // industry click-killers
  double fadeOutMs = 20.0;
  int normMode = 3;             // 0 Off, 1 Peak dBFS, 2 LUFS-I, 3 True-peak safe
  double normTarget = -1.0;     // meaning depends on normMode
};

class OneShotPanel {
public:
  bool IsVisible() const { return m_visible; }
  void Show();   // premium-only: no-op in the OFF build
  void Hide();

  RECT GetRect(RECT waveformRect) const;
  bool HitTest(int x, int y, RECT waveformRect) const;
  void DrawPremium(HDC hdc, RECT waveformRect, double dpr);

  bool OnMouseDown(int x, int y, RECT waveformRect);
  void OnMouseMove(int x, int y, RECT waveformRect);
  void OnMouseUp();
  bool OnHover(int x, int y, RECT waveformRect);
  bool OnMouseWheel(int x, int y, double steps, bool fine, RECT waveformRect);
  bool IsDragging() const { return m_dragKnob >= 0 || m_panelDragging; }

  bool RunRequested()      { bool v = m_runRequested; m_runRequested = false; return v; }
  bool ParamsChanged() const { return m_paramsChanged; }
  void ClearParamsChanged()  { m_paramsChanged = false; }
  bool GeomChanged() const   { return m_geomChanged; }
  void ClearGeomChanged()    { m_geomChanged = false; }

  const OneShotParams& GetParams() const { return m_params; }
  void SetParams(const OneShotParams& p);   // clamps to knob ranges
  void SetPanelOffset(int ox, int oy) { m_offsetX = ox; m_offsetY = oy; }
  int  GetPanelOffsetX() const { return m_offsetX; }
  int  GetPanelOffsetY() const { return m_offsetY; }

private:
  double EffScale(RECT waveformRect) const;
  int    HitId(double lx, double ly) const;
  double GetValue(int idx) const;
  void   SetValue(int idx, double v);
  void   SetNormMode(int mode);   // switches the TARGET range + default

  bool m_visible = false;
  OneShotParams m_params;
  int  m_offsetX = 0, m_offsetY = 0;
  int  m_dragKnob = -1, m_dragLastY = 0;
  bool m_panelDragging = false;
  int  m_dragOffX = 0, m_dragOffY = 0;
  bool m_geomChanged = false;
  int  m_hover = OS_HIT_NONE;
  bool m_runRequested = false;
  bool m_paramsChanged = false;
  UiCanvas m_canvas;
};
