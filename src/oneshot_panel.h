// oneshot_panel.h — premium ONE-SHOT PREP overlay (v2.4 INC-B1 + B2)
//
// The "10-30 variations per SFX" workflow: one panel that trims silence,
// applies the industry edge micro-fades (5 ms in / 20 ms out), normalizes to
// a target and writes the prepared WAV(s) next to the source. INC-B2 adds
// slicing (whole file / by regions-markers / by silence gaps) and the naming
// pattern ({name}_{nn}) - the whole variation batch in one Run.
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
  int sliceMode = 0;            // INC-B2: 0 Whole file, 1 By regions/markers,
                                //         2 By silence (gap > 150 ms, min 50 ms)
  char pattern[64] = "{name}_{nn}";  // INC-B2: output naming; tokens {name} {nn}
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
  // Pattern box clicked (INC-B2): the host opens the native input dialog
  // (free-text editing in the accelerator-driven panel is not worth the risk).
  bool PatternEditRequested() { bool v = m_patternEditReq; m_patternEditReq = false; return v; }
  // OPEN FOLDER clicked: the host reveals the export destination folder.
  bool OpenDirRequested() { bool v = m_openDirReq; m_openDirReq = false; return v; }
  bool ParamsChanged() const { return m_paramsChanged; }
  void ClearParamsChanged()  { m_paramsChanged = false; }
  bool GeomChanged() const   { return m_geomChanged; }
  void ClearGeomChanged()    { m_geomChanged = false; }

  const OneShotParams& GetParams() const { return m_params; }
  void SetParams(const OneShotParams& p);   // clamps to knob ranges
  void SetPattern(const char* pat);   // sanitized copy; empty -> "{name}_{nn}"
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
  bool m_patternEditReq = false;
  bool m_openDirReq = false;
  bool m_paramsChanged = false;
  UiCanvas m_canvas;
};
