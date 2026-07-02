// settings_panel.cpp — premium Settings overlay (v2.2.0)
#include "settings_panel.h"
#include "globals.h"
#include "ui_theme.h"
#include <cmath>

namespace {
constexpr double kScaleMin = 0.8;
constexpr double kScaleMax = 2.0;

// Slider X (base coords) -> UI scale, clamped to the range.
double ValueFromX(double lx, const SettingsLayout& L)
{
  double t = (lx - L.sliderTrack.x) / (L.sliderTrack.w > 0.0 ? L.sliderTrack.w : 1.0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return kScaleMin + t * (kScaleMax - kScaleMin);
}
}  // namespace

double SettingsPanel::EffScale(RECT wr) const
{
  double s = m_dragging ? m_dragScale : g_uiScale;
  if (s <= 0.0) s = 1.0;
  // Fit-clamp: never let the panel outgrow the waveform rect (high scales / small
  // windows). Floored so it can never become microscopic; the same clamp feeds the
  // hit-test divide, so draw == hit even when clamped.
  const double availW = (double)(wr.right - wr.left) - 8.0;
  const double availH = (double)(wr.bottom - wr.top) - 8.0;
  if (availW > 0.0 && availH > 0.0) {
    const double fitW = availW / (double)dynui::kSettingsW;
    const double fitH = availH / (double)dynui::kSettingsH;
    double cap = fitW < fitH ? fitW : fitH;
    if (cap < 0.5) cap = 0.5;
    if (s > cap) s = cap;
  }
  return s;
}

RECT SettingsPanel::GetRect(RECT wr) const
{
  const double S = EffScale(wr);
  const int pw = (int)std::lround((double)dynui::kSettingsW * S);
  const int ph = (int)std::lround((double)dynui::kSettingsH * S);
  // Centered horizontally, upper third vertically (clear of the bottom-docked
  // dynamics panel). Clamped into the waveform rect on small windows.
  int left = (wr.left + wr.right - pw) / 2;
  int top  = wr.top + ((wr.bottom - wr.top) - ph) / 3;
  if (left < wr.left) left = wr.left;
  if (top  < wr.top)  top  = wr.top;
  if (left + pw > wr.right)  left = wr.right - pw;
  if (top  + ph > wr.bottom) top  = wr.bottom - ph;
  return { left, top, left + pw, top + ph };
}

bool SettingsPanel::HitTest(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  return x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom;
}

int SettingsPanel::HitId(double lx, double ly) const
{
  const SettingsLayout L =
      ComputeSettingsLayout((double)dynui::kSettingsW, (double)dynui::kSettingsH);
  if (L.closeBtn.contains(lx, ly))  return SET_HIT_CLOSE;
  if (L.sliderRow.contains(lx, ly)) return SET_HIT_SLIDER;
  for (int i = 0; i < 3; ++i)
    if (L.density[i].contains(lx, ly)) return SET_HIT_DENSITY0 + i;
  if (L.fitBtn.contains(lx, ly))    return SET_HIT_FIT;
  for (int i = 0; i < 3; ++i)
    if (L.rulerSeg[i].contains(lx, ly)) return SET_HIT_RULER0 + i;
  if (L.masterToggle.contains(lx, ly)) return SET_HIT_MASTER;
  for (int i = 0; i < 3; ++i)
    if (L.meterSeg[i].contains(lx, ly)) return SET_HIT_METER0 + i;
  if (L.viewToggle[0].contains(lx, ly)) return SET_HIT_VIEW_METERS;
  if (L.viewToggle[1].contains(lx, ly)) return SET_HIT_VIEW_RULER;
  if (L.viewToggle[2].contains(lx, ly)) return SET_HIT_VIEW_SNAP;
  if (L.viewToggle[3].contains(lx, ly)) return SET_HIT_VIEW_MINIMAP;
  if (L.zoomSeg[0].contains(lx, ly))    return SET_HIT_VIEW_ZOOM0;
  if (L.zoomSeg[1].contains(lx, ly))    return SET_HIT_VIEW_ZOOM1;
  if (L.waveSeg[0].contains(lx, ly))    return SET_HIT_VIEW_WAVE0;
  if (L.waveSeg[1].contains(lx, ly))    return SET_HIT_VIEW_WAVE1;
  if (L.specSeg[0].contains(lx, ly))    return SET_HIT_VIEW_SPEC0;
  if (L.specSeg[1].contains(lx, ly))    return SET_HIT_VIEW_SPEC1;
  return SET_HIT_NONE;
}

void SettingsPanel::DrawPremium(HDC hdc, RECT wr, double dpr, const SettingsPrefs& prefs)
{
  if (!m_visible) return;
  const RECT pr = GetRect(wr);
  SettingsVM vm;
  vm.uiScale  = g_uiScale;   // live value even while the panel geometry is drag-frozen
  vm.hover    = m_hover;
  vm.dragging = m_dragging;
  vm.prefs    = prefs;
  m_canvas.RenderSettingsPanel(hdc, pr.left, pr.top, pr.right - pr.left,
                               pr.bottom - pr.top, dpr, vm);
}

bool SettingsPanel::OnMouseDown(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const SettingsLayout L =
      ComputeSettingsLayout((double)dynui::kSettingsW, (double)dynui::kSettingsH);

  if (L.closeBtn.contains(lx, ly)) { Hide(); return true; }

  if (L.sliderRow.contains(lx, ly)) {
    const double norm = (g_uiScale - kScaleMin) / (kScaleMax - kScaleMin);
    const double thumbX = L.sliderTrack.x + norm * L.sliderTrack.w;
    m_dragging  = true;
    m_dragScale = g_uiScale;          // freeze the panel's own geometry for the drag
    if (std::fabs(lx - thumbX) <= 10.0) {
      m_grabDx = lx - thumbX;         // thumb grab: keep the offset, no jump
    } else {
      m_grabDx = 0.0;                 // track click: jump to the absolute position
      g_uiScale = ValueFromX(lx, L);
      m_scaleClicked = true;
    }
    return true;                      // IsDragging() -> host SetCapture
  }

  for (int i = 0; i < 3; ++i)
    if (L.density[i].contains(lx, ly)) {
      g_uiScale = dynui::kDensityScale[i];
      m_scaleClicked = true;
      return true;
    }
  if (L.fitBtn.contains(lx, ly)) { m_fitRequested = true; return true; }

  // Migrated preferences: report the control id; the host runs its CM_* handler
  // (single behavior path for the panel and the OFF-build menu).
  const int hit = HitId(lx, ly);
  if (hit >= SET_HIT_RULER0 && hit <= SET_HIT_VIEW_SPEC1) {
    m_prefClicked = hit;
    return true;
  }

  return true;   // elsewhere on the panel body: consume (it is an overlay)
}

bool SettingsPanel::OnMouseMove(int x, int y, RECT wr)
{
  (void)y;
  if (!m_dragging) return false;
  const RECT pr = GetRect(wr);        // stable during the drag (EffScale frozen)
  const double lx = (double)(x - pr.left) / EffScale(wr) - m_grabDx;
  const SettingsLayout L =
      ComputeSettingsLayout((double)dynui::kSettingsW, (double)dynui::kSettingsH);
  const double v = ValueFromX(lx, L);
  if (v == g_uiScale) return false;
  g_uiScale = v;
  return true;
}

bool SettingsPanel::OnMouseUp()
{
  if (!m_dragging) return false;
  m_dragging = false;                 // panel geometry snaps to the new scale now
  return true;
}

bool SettingsPanel::OnHover(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  int h = SET_HIT_NONE;
  const RECT pr = GetRect(wr);
  if (x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom) {
    const double S = EffScale(wr);
    h = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  }
  if (h == m_hover) return false;
  m_hover = h;
  return true;
}
