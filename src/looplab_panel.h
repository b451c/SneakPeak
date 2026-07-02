// looplab_panel.h — premium LOOP LAB overlay (v2.4 INC-A5)
//
// UI re-home of the Loop Lab workflow (user critique: the context menu is an
// ENTRY POINT, panels are where work happens). One panel with the loop
// readouts, the finder's candidate list (click = set + audition), PLAY LOOP /
// PLAY SEAM transport, FIND / WELD+ms / SET FROM SELECTION / CLEAR and the
// WRITE SMPL ON SAVE toggle. ALL loop logic stays host-side; the panel is a
// view + one-shot action flags (OneShotPanel/LimiterPanel host contract:
// OnMouseDown returns true when consumed, the host polls the flags).
// Premium-only: Show() is a no-op in the OFF build (the full Loop menu stays).
#pragma once

#include "platform.h"
#include "ui_render.h"

// Live host state, built by the host each paint (it owns ALL loop state).
struct LoopLabRowState {
  double startSec = 0.0, endSec = 0.0, score = 0.0;
  bool texture = false;   // texture-tier candidate (weld after picking)
  bool active = false;    // candidate == the current loop region
};
struct LoopLabState {
  bool hasLoop = false;
  double startSec = 0.0, endSec = 0.0;
  bool hasSelection = false;                // gates SET FROM SELECTION
  bool playLoopOn = false, playSeamOn = false;
  bool findBusy = false;
  bool writeSmpl = true;
  int numRows = 0;
  LoopLabRowState rows[kLoopLabMaxRows];
};

class LoopLabPanel {
public:
  bool IsVisible() const { return m_visible; }
  void Show();   // premium-only: no-op in the OFF build
  void Hide();

  RECT GetRect(RECT waveformRect) const;
  bool HitTest(int x, int y, RECT waveformRect) const;
  void DrawPremium(HDC hdc, RECT waveformRect, double dpr, const LoopLabState& st);

  bool OnMouseDown(int x, int y, RECT waveformRect);
  void OnMouseMove(int x, int y, RECT waveformRect);
  void OnMouseUp();
  bool OnHover(int x, int y, RECT waveformRect);
  bool OnMouseWheel(int x, int y, double steps, bool fine, RECT waveformRect);
  bool IsDragging() const { return m_weldDrag || m_panelDragging; }

  // One-shot flags, polled by the host after a consumed OnMouseDown. The
  // action id maps 1:1 onto the existing CM_* handlers (Settings pattern -
  // one behavior path, no drift); WELD carries GetWeldMs().
  int  ActionClicked()    { int v = m_action; m_action = LL_HIT_NONE; return v; }
  int  CandidateClicked() { int v = m_candClicked; m_candClicked = -1; return v; }
  bool ParamsChanged() const { return m_paramsChanged; }
  void ClearParamsChanged()  { m_paramsChanged = false; }
  bool GeomChanged() const   { return m_geomChanged; }
  void ClearGeomChanged()    { m_geomChanged = false; }

  int  GetWeldMs() const { return m_weldMs; }
  void SetWeldMs(int ms);   // clamps to 5..500
  void SetPanelOffset(int ox, int oy) { m_offsetX = ox; m_offsetY = oy; }
  int  GetPanelOffsetX() const { return m_offsetX; }
  int  GetPanelOffsetY() const { return m_offsetY; }

private:
  double EffScale(RECT waveformRect) const;
  int    HitId(double lx, double ly) const;

  bool m_visible = false;
  int  m_weldMs = 50;                 // Weld crossfade ms (5..500, persisted)
  int  m_offsetX = 0, m_offsetY = 0;
  bool m_weldDrag = false;
  int  m_dragLastY = 0;
  bool m_panelDragging = false;
  int  m_dragOffX = 0, m_dragOffY = 0;
  bool m_geomChanged = false, m_paramsChanged = false;
  int  m_hover = LL_HIT_NONE;
  int  m_action = LL_HIT_NONE, m_candClicked = -1;
  // Gating snapshot from the last paint: clicks on disabled controls are
  // suppressed here so the host never sees actions that cannot apply.
  bool m_stHasLoop = false, m_stHasSel = false, m_stFindBusy = false;
  int  m_stNumRows = 0;
  UiCanvas m_canvas;
};
