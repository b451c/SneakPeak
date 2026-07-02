// oneshot_panel.cpp — premium ONE-SHOT PREP overlay (v2.4 INC-B1)
#include "oneshot_panel.h"
#include "globals.h"
#include "ui_theme.h"
#include <cmath>
#include <cstdio>

// Knob table. TARGET (idx 4) swaps range/unit/default with the normalize
// mode (Peak dBFS / LUFS-I / dBTP); the static row holds the Peak defaults.
struct OsKnobDef {
  const char* label;
  double minVal, maxVal;
  const char* unit;
  int precision;
  double defaultVal;
};
static const OsKnobDef OS_DEFS[kOsNumParams] = {
  { "Trim Thr", -90.0, -30.0, "dB", 0, -60.0 },
  { "Pad",      0.0,   200.0, "ms", 0, 10.0 },
  { "Fade In",  0.0,   100.0, "ms", 0, 5.0 },
  { "Fade Out", 0.0,   500.0, "ms", 0, 20.0 },
  { "Target",   -12.0, 0.0,   "dB", 1, -0.3 },
};

namespace {

double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// TARGET knob semantics per normalize mode.
void TargetRange(int mode, double* lo, double* hi, double* def, const char** unit)
{
  switch (mode) {
    case 2:  *lo = -30.0; *hi = -9.0; *def = -18.0; *unit = "LUFS"; break;  // LUFS-I
    case 3:  *lo = -12.0; *hi = 0.0;  *def = -1.0;  *unit = "dBTP"; break;  // TP safe
    default: *lo = -12.0; *hi = 0.0;  *def = -0.3;  *unit = "dBFS"; break;  // Peak/Off
  }
}

}  // namespace

double OneShotPanel::GetValue(int idx) const
{
  switch (idx) {
    case 0: return m_params.trimThreshDb;
    case 1: return m_params.trimPadMs;
    case 2: return m_params.fadeInMs;
    case 3: return m_params.fadeOutMs;
    case 4: return m_params.normTarget;
  }
  return 0.0;
}

void OneShotPanel::SetValue(int idx, double v)
{
  if (idx < 0 || idx >= kOsNumParams) return;
  double lo = OS_DEFS[idx].minVal, hi = OS_DEFS[idx].maxVal;
  if (idx == 4) {
    double def;
    const char* unit;
    TargetRange(m_params.normMode, &lo, &hi, &def, &unit);
  }
  v = ClampD(v, lo, hi);
  switch (idx) {
    case 0: m_params.trimThreshDb = v; break;
    case 1: m_params.trimPadMs = v; break;
    case 2: m_params.fadeInMs = v; break;
    case 3: m_params.fadeOutMs = v; break;
    case 4: m_params.normTarget = v; break;
  }
}

void OneShotPanel::SetNormMode(int mode)
{
  if (mode < 0 || mode > 3 || mode == m_params.normMode) return;
  m_params.normMode = mode;
  double lo, hi, def;
  const char* unit;
  TargetRange(mode, &lo, &hi, &def, &unit);
  m_params.normTarget = def;   // each mode has its own sane default
  m_paramsChanged = true;
}

void OneShotPanel::SetParams(const OneShotParams& p)
{
  m_params.trimEnable = p.trimEnable;
  m_params.normMode = (p.normMode >= 0 && p.normMode <= 3) ? p.normMode : 3;
  SetValue(0, p.trimThreshDb);
  SetValue(1, p.trimPadMs);
  SetValue(2, p.fadeInMs);
  SetValue(3, p.fadeOutMs);
  SetValue(4, p.normTarget);
}

void OneShotPanel::Show()
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  m_visible = true;
  m_hover = OS_HIT_NONE;
#endif
}

void OneShotPanel::Hide()
{
  m_visible = false;
  m_dragKnob = -1;
  m_panelDragging = false;
  m_hover = OS_HIT_NONE;
}

double OneShotPanel::EffScale(RECT wr) const
{
  double s = g_uiScale > 0.0 ? g_uiScale : 1.0;
  const double availW = (double)(wr.right - wr.left) - 8.0;
  const double availH = (double)(wr.bottom - wr.top) - 8.0;
  if (availW > 0.0 && availH > 0.0) {
    const double fitW = availW / (double)dynui::kOsPanelW;
    const double fitH = availH / (double)dynui::kOsPanelH;
    double cap = fitW < fitH ? fitW : fitH;
    if (cap < 0.5) cap = 0.5;
    if (s > cap) s = cap;
  }
  return s;
}

RECT OneShotPanel::GetRect(RECT wr) const
{
  const double S = EffScale(wr);
  const int pw = (int)std::lround((double)dynui::kOsPanelW * S);
  const int ph = (int)std::lround((double)dynui::kOsPanelH * S);
  int cx = (wr.left + wr.right) / 2 + m_offsetX;
  int cy = wr.bottom - ph - 10 + m_offsetY;
  int left = cx - pw / 2;
  if (left < wr.left) left = wr.left;
  if (left + pw > wr.right) left = wr.right - pw;
  if (cy < wr.top) cy = wr.top;
  if (cy + ph > wr.bottom) cy = wr.bottom - ph;
  return { left, cy, left + pw, cy + ph };
}

bool OneShotPanel::HitTest(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  return x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom;
}

int OneShotPanel::HitId(double lx, double ly) const
{
  const OneShotLayout L =
      ComputeOneShotLayout((double)dynui::kOsPanelW, (double)dynui::kOsPanelH);
  if (L.closeBtn.contains(lx, ly)) return OS_HIT_CLOSE;
  if (L.run.contains(lx, ly)) return OS_HIT_RUN;
  if (L.trimPill.contains(lx, ly)) return OS_HIT_TRIM;
  for (int i = 0; i < 4; i++)
    if (L.normSeg[i].contains(lx, ly)) return OS_HIT_SEG0 + i;
  for (int i = 0; i < kOsNumParams; i++)
    if (L.knob[i].contains(lx, ly)) return OS_HIT_KNOB0 + i;
  return OS_HIT_NONE;
}

bool OneShotPanel::OnMouseDown(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const int hit = HitId(lx, ly);

  if (hit == OS_HIT_CLOSE) { Hide(); return true; }
  if (hit == OS_HIT_RUN) { m_runRequested = true; return true; }
  if (hit == OS_HIT_TRIM) {
    m_params.trimEnable = !m_params.trimEnable;
    m_paramsChanged = true;
    return true;
  }
  if (hit >= OS_HIT_SEG0 && hit < OS_HIT_SEG0 + 4) {
    SetNormMode(hit - OS_HIT_SEG0);
    return true;
  }
  if (hit >= OS_HIT_KNOB0) {
    const int i = hit - OS_HIT_KNOB0;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {   // Cmd-click = default
      double lo, hi, def = OS_DEFS[i].defaultVal;
      const char* unit;
      if (i == 4) TargetRange(m_params.normMode, &lo, &hi, &def, &unit);
      SetValue(i, def);
      m_paramsChanged = true;
      return true;
    }
    m_dragKnob = i;
    m_dragLastY = y;
    return true;
  }
  m_panelDragging = true;
  m_dragOffX = x - pr.left;
  m_dragOffY = y - pr.top;
  return true;
}

void OneShotPanel::OnMouseMove(int x, int y, RECT wr)
{
  if (m_dragKnob >= 0) {
    double lo = OS_DEFS[m_dragKnob].minVal, hi = OS_DEFS[m_dragKnob].maxVal;
    if (m_dragKnob == 4) {
      double def;
      const char* unit;
      TargetRange(m_params.normMode, &lo, &hi, &def, &unit);
    }
    const double range = (hi - lo) != 0.0 ? (hi - lo) : 1.0;
    const int dy = m_dragLastY - y;
    m_dragLastY = y;
    const int ady = dy < 0 ? -dy : dy;
    const double speed = ady > 40 ? 40.0 : (double)ady;
    const double gainPerPx = (1.0 / 240.0) * (1.0 + speed * 0.05);
    double nrm = (GetValue(m_dragKnob) - lo) / range;
    nrm = ClampD(nrm + (double)dy * gainPerPx, 0.0, 1.0);
    SetValue(m_dragKnob, lo + nrm * range);
    m_paramsChanged = true;
    return;
  }
  if (m_panelDragging) {
    const RECT pr = GetRect(wr);
    const int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    const int defaultCX = (wr.left + wr.right) / 2;
    const int defaultCY = wr.bottom - ph - 10;
    m_offsetX = ((x - m_dragOffX) + pw / 2) - defaultCX;
    m_offsetY = (y - m_dragOffY) - defaultCY;
    m_geomChanged = true;
  }
}

void OneShotPanel::OnMouseUp()
{
  m_dragKnob = -1;
  m_panelDragging = false;
}

bool OneShotPanel::OnHover(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  int h = OS_HIT_NONE;
  const RECT pr = GetRect(wr);
  if (x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom) {
    const double S = EffScale(wr);
    h = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  }
  if (h == m_hover) return false;
  m_hover = h;
  return true;
}

bool OneShotPanel::OnMouseWheel(int x, int y, double steps, bool fine, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const int hit = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  if (hit < OS_HIT_KNOB0) return false;
  const int i = hit - OS_HIT_KNOB0;
  double lo = OS_DEFS[i].minVal, hi = OS_DEFS[i].maxVal;
  if (i == 4) {
    double def;
    const char* unit;
    TargetRange(m_params.normMode, &lo, &hi, &def, &unit);
  }
  const double range = (hi - lo) != 0.0 ? (hi - lo) : 1.0;
  double nrm = (GetValue(i) - lo) / range;
  nrm = ClampD(nrm + (fine ? 0.005 : 0.025) * steps, 0.0, 1.0);
  SetValue(i, lo + nrm * range);
  m_paramsChanged = true;
  return true;
}

void OneShotPanel::DrawPremium(HDC hdc, RECT wr, double dpr)
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  if (!m_visible) return;
  const RECT pr = GetRect(wr);

  OneShotVM vm;
  vm.trimEnable = m_params.trimEnable;
  vm.normMode = m_params.normMode;
  vm.hover = m_hover;
  for (int i = 0; i < kOsNumParams; i++) {
    const OsKnobDef& def = OS_DEFS[i];
    double lo = def.minVal, hi = def.maxVal, dflt = def.defaultVal;
    const char* unit = def.unit;
    if (i == 4) TargetRange(m_params.normMode, &lo, &hi, &dflt, &unit);
    const double range = (hi - lo) != 0.0 ? (hi - lo) : 1.0;
    KnobVM& k = vm.knobs[i];
    k.value = GetValue(i);
    k.norm = ClampD((k.value - lo) / range, 0.0, 1.0);
    k.defaultNorm = ClampD((dflt - lo) / range, 0.0, 1.0);
    k.label = def.label;
    k.unit = unit;
    k.precision = def.precision;
    k.hover = (m_hover == OS_HIT_KNOB0 + i) || m_dragKnob == i;
  }

  m_canvas.RenderOneShotPanel(hdc, pr.left, pr.top, pr.right - pr.left,
                              pr.bottom - pr.top, dpr, vm);
#else
  (void)hdc; (void)wr; (void)dpr;
#endif
}
