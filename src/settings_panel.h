// settings_panel.h — premium Settings overlay (v2.2.0)
//
// Home for UI preferences (the global UI scale now; migrated view prefs next), so
// the context menu stays a WORK menu. Blend2D-rendered in OnPaintOverlay on the
// same infrastructure as the Dynamics panel (UiCanvas, base-coord layout shared
// with hit-testing). The panel base size scales with g_uiScale (premium chrome);
// while the slider is being dragged the panel's OWN geometry is frozen at the
// grab-time scale so the thumb mapping stays stable under the cursor (the rest
// of the UI live-previews); it snaps to the new scale on release.
//
// Host contract (mirrors DynamicsPanel): OnMouseDown returns true when the click
// landed on the panel; the host then polls the one-shot flags. OnMouseMove during
// a drag returns true when g_uiScale changed (host relayouts); a panel drag (any
// non-control spot, s20) moves the offsets instead and the host repaints while
// IsPanelDragging. OnMouseUp returns true when a slider drag ended (host persists
// + queues the font rebuild); GeomChanged after a panel drag (host persists).
#pragma once

#include "platform.h"
#include "ui_render.h"

class SettingsPanel {
public:
  bool IsVisible() const { return m_visible; }
  void Show() { m_visible = true; }
  void Hide() { m_visible = false; m_dragging = false; m_panelDragging = false; m_hover = SET_HIT_NONE; }

  RECT GetRect(RECT waveformRect) const;
  bool HitTest(int x, int y, RECT waveformRect) const;
  // prefs = the host-owned preference values shown in the RULER/METERS/VIEW sections.
  void DrawPremium(HDC hdc, RECT waveformRect, double dpr, const SettingsPrefs& prefs);

  bool OnMouseDown(int x, int y, RECT waveformRect);  // true = consumed (click inside the panel)
  bool OnMouseMove(int x, int y, RECT waveformRect);  // true = g_uiScale changed (slider drag)
  bool OnMouseUp();                                   // true = a slider drag just ended
  bool OnHover(int x, int y, RECT waveformRect);      // true = hover target changed (repaint)
  bool IsDragging() const { return m_dragging || m_panelDragging; }
  bool IsPanelDragging() const { return m_panelDragging; }
  bool GeomChanged() const   { return m_geomChanged; }
  void ClearGeomChanged()    { m_geomChanged = false; }
  void SetPanelOffset(int ox, int oy) { m_offsetX = ox; m_offsetY = oy; }
  int  GetPanelOffsetX() const { return m_offsetX; }
  int  GetPanelOffsetY() const { return m_offsetY; }

  // One-shot flags, polled by the host right after a consumed OnMouseDown.
  bool ScaleChangedByClick() { bool v = m_scaleClicked; m_scaleClicked = false; return v; }
  bool FitRequested()        { bool v = m_fitRequested; m_fitRequested = false; return v; }
  // Preference control clicked (SET_HIT_RULER0..SET_HIT_VIEW_FFT3), or
  // SET_HIT_NONE. The host maps the id onto its existing CM_* command handler.
  int  PrefClicked()         { int v = m_prefClicked; m_prefClicked = SET_HIT_NONE; return v; }

private:
  double EffScale(RECT waveformRect) const;  // g_uiScale (drag-frozen), fit-clamped to the rect
  int    HitId(double lx, double ly) const;  // base-coord point -> SettingsHit id

  bool   m_visible = false;
  bool   m_dragging = false;     // slider thumb drag in flight
  bool   m_panelDragging = false;  // panel drag in flight (s20; the limiter pattern)
  int    m_dragOffX = 0, m_dragOffY = 0;
  int    m_offsetX = 0, m_offsetY = 0;   // panel-drag offsets (persisted by the host)
  bool   m_geomChanged = false;
  double m_dragScale = 1.0;      // g_uiScale at grab (freezes the panel's own geometry)
  double m_grabDx = 0.0;         // base-px cursor->thumb offset (no jump on thumb grab)
  int    m_hover = SET_HIT_NONE;
  bool   m_scaleClicked = false; // track jump / density preset clicked -> host applies+persists
  bool   m_fitRequested = false; // "Fit to window" clicked -> host computes+applies
  int    m_prefClicked = SET_HIT_NONE; // preference control clicked -> host runs its CM_* handler
  UiCanvas m_canvas;
};
