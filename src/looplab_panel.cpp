// looplab_panel.cpp — premium LOOP LAB overlay (v2.4 INC-A5)
#include "looplab_panel.h"
#include "globals.h"
#include "ui_theme.h"
#include <cmath>
#include <cstdio>

namespace {

constexpr double kWeldMinMs = 5.0, kWeldMaxMs = 500.0, kWeldDefMs = 50.0;

double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

#ifdef SNEAKPEAK_BLEND2D_PANEL
// Compact timecode: m:ss.mmm (hours prepended only when needed) - the plan's
// candidate-row format; the full HH:MM:SS would not fit five list columns.
void FmtLoopTime(double sec, char* buf, size_t sz)
{
  if (sec < 0.0) sec = 0.0;
  const long long ms = (long long)std::llround(sec * 1000.0);
  const int h = (int)(ms / 3600000), m = (int)((ms / 60000) % 60);
  const int s = (int)((ms / 1000) % 60), mm = (int)(ms % 1000);
  if (h > 0) snprintf(buf, sz, "%d:%02d:%02d.%03d", h, m, s, mm);
  else       snprintf(buf, sz, "%d:%02d.%03d", m, s, mm);
}
#endif

}  // namespace

void LoopLabPanel::Show()
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  m_visible = true;
  m_hover = LL_HIT_NONE;
#endif
}

void LoopLabPanel::Hide()
{
  m_visible = false;
  m_weldDrag = false;
  m_panelDragging = false;
  m_hover = LL_HIT_NONE;
  m_action = LL_HIT_NONE;
  m_candClicked = -1;
}

void LoopLabPanel::SetWeldMs(int ms)
{
  m_weldMs = (int)ClampD((double)ms, kWeldMinMs, kWeldMaxMs);
}

double LoopLabPanel::EffScale(RECT wr) const
{
  double s = g_uiScale > 0.0 ? g_uiScale : 1.0;
  const double availW = (double)(wr.right - wr.left) - 8.0;
  const double availH = (double)(wr.bottom - wr.top) - 8.0;
  if (availW > 0.0 && availH > 0.0) {
    const double fitW = availW / (double)dynui::kLoopPanelW;
    const double fitH = availH / (double)dynui::kLoopPanelH;
    double cap = fitW < fitH ? fitW : fitH;
    if (cap < 0.5) cap = 0.5;
    if (s > cap) s = cap;
  }
  return s;
}

RECT LoopLabPanel::GetRect(RECT wr) const
{
  const double S = EffScale(wr);
  const int pw = (int)std::lround((double)dynui::kLoopPanelW * S);
  const int ph = (int)std::lround((double)dynui::kLoopPanelH * S);
  int cx = (wr.left + wr.right) / 2 + m_offsetX;
  int cy = wr.bottom - ph - 10 + m_offsetY;
  int left = cx - pw / 2;
  if (left < wr.left) left = wr.left;
  if (left + pw > wr.right) left = wr.right - pw;
  if (cy < wr.top) cy = wr.top;
  if (cy + ph > wr.bottom) cy = wr.bottom - ph;
  return { left, cy, left + pw, cy + ph };
}

bool LoopLabPanel::HitTest(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  return x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom;
}

int LoopLabPanel::HitId(double lx, double ly) const
{
  const LoopLabLayout L =
      ComputeLoopLabLayout((double)dynui::kLoopPanelW, (double)dynui::kLoopPanelH);
  if (L.closeBtn.contains(lx, ly)) return LL_HIT_CLOSE;
  if (L.playLoop.contains(lx, ly)) return LL_HIT_PLAY_LOOP;
  if (L.playSeam.contains(lx, ly)) return LL_HIT_PLAY_SEAM;
  if (L.findBtn.contains(lx, ly)) return LL_HIT_FIND;
  if (L.weldBtn.contains(lx, ly)) return LL_HIT_WELD;
  if (L.weldMs.contains(lx, ly)) return LL_HIT_WELD_MS;
  if (L.setSelBtn.contains(lx, ly)) return LL_HIT_SET_SEL;
  if (L.clearBtn.contains(lx, ly)) return LL_HIT_CLEAR;
  if (L.smplPill.contains(lx, ly)) return LL_HIT_SMPL;
  for (int i = 0; i < kLoopLabMaxRows; i++)
    if (L.row[i].contains(lx, ly)) return LL_HIT_ROW0 + i;
  return LL_HIT_NONE;
}

bool LoopLabPanel::OnMouseDown(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const int hit = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);

  switch (hit) {
    case LL_HIT_CLOSE:
      Hide();
      return true;
    case LL_HIT_FIND:
      if (!m_stFindBusy) m_action = hit;
      return true;
    case LL_HIT_WELD:
    case LL_HIT_PLAY_LOOP:
    case LL_HIT_PLAY_SEAM:
    case LL_HIT_CLEAR:
      if (m_stHasLoop) m_action = hit;
      return true;
    case LL_HIT_SET_SEL:
      if (m_stHasSel) m_action = hit;
      return true;
    case LL_HIT_SMPL:
      m_action = hit;
      return true;
    case LL_HIT_WELD_MS:
      if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {   // Cmd-click = default
        SetWeldMs((int)kWeldDefMs);
        m_paramsChanged = true;
        return true;
      }
      m_weldDrag = true;
      m_dragLastY = y;
      return true;
    default:
      break;
  }
  if (hit >= LL_HIT_ROW0) {
    const int row = hit - LL_HIT_ROW0;
    if (row < m_stNumRows) m_candClicked = row;
    return true;
  }
  m_panelDragging = true;
  m_dragOffX = x - pr.left;
  m_dragOffY = y - pr.top;
  return true;
}

void LoopLabPanel::OnMouseMove(int x, int y, RECT wr)
{
  if (m_weldDrag) {
    // Same accelerated drag mechanics as the panel knobs (OneShotPanel).
    const double range = kWeldMaxMs - kWeldMinMs;
    const int dy = m_dragLastY - y;
    m_dragLastY = y;
    const int ady = dy < 0 ? -dy : dy;
    const double speed = ady > 40 ? 40.0 : (double)ady;
    const double gainPerPx = (1.0 / 240.0) * (1.0 + speed * 0.05);
    double nrm = ((double)m_weldMs - kWeldMinMs) / range;
    nrm = ClampD(nrm + (double)dy * gainPerPx, 0.0, 1.0);
    SetWeldMs((int)std::lround(kWeldMinMs + nrm * range));
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

void LoopLabPanel::OnMouseUp()
{
  m_weldDrag = false;
  m_panelDragging = false;
}

bool LoopLabPanel::OnHover(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  int h = LL_HIT_NONE;
  const RECT pr = GetRect(wr);
  if (x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom) {
    const double S = EffScale(wr);
    h = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  }
  if (h == m_hover) return false;
  m_hover = h;
  return true;
}

bool LoopLabPanel::OnMouseWheel(int x, int y, double steps, bool fine, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const int hit = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  if (hit != LL_HIT_WELD_MS) return false;
  SetWeldMs(m_weldMs + (int)std::lround(steps * (fine ? 1.0 : 5.0)));
  m_paramsChanged = true;
  return true;
}

void LoopLabPanel::DrawPremium(HDC hdc, RECT wr, double dpr, const LoopLabState& st)
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  if (!m_visible) return;
  // Snapshot the gating state for hit-time checks (set every paint; a click
  // can only land after at least one paint of the visible panel).
  m_stHasLoop = st.hasLoop;
  m_stHasSel = st.hasSelection;
  m_stFindBusy = st.findBusy;
  m_stNumRows = st.numRows;

  const RECT pr = GetRect(wr);

  LoopLabVM vm;
  char sBuf[24], eBuf[24], lBuf[24], wBuf[16];
  char rowBuf[kLoopLabMaxRows][64];
  if (st.hasLoop) {
    FmtLoopTime(st.startSec, sBuf, sizeof(sBuf));
    FmtLoopTime(st.endSec, eBuf, sizeof(eBuf));
    FmtLoopTime(st.endSec - st.startSec, lBuf, sizeof(lBuf));
    vm.startText = sBuf;
    vm.endText = eBuf;
    vm.lenText = lBuf;
  }
  snprintf(wBuf, sizeof(wBuf), "%d ms", m_weldMs);
  vm.weldText = wBuf;
  vm.numRows = st.numRows < kLoopLabMaxRows ? st.numRows : kLoopLabMaxRows;
  for (int i = 0; i < vm.numRows; i++) {
    const LoopLabRowState& c = st.rows[i];
    char t0[24], t1[24];
    FmtLoopTime(c.startSec, t0, sizeof(t0));
    FmtLoopTime(c.endSec, t1, sizeof(t1));
    snprintf(rowBuf[i], sizeof(rowBuf[i]), "#%d  %s -> %s  (%.1f s)  %.2f",
             i + 1, t0, t1, c.endSec - c.startSec, c.score);
    vm.rowText[i] = rowBuf[i];
    vm.rowTexture[i] = c.texture;
    vm.rowActive[i] = c.active;
  }
  vm.hasLoop = st.hasLoop;
  vm.hasSelection = st.hasSelection;
  vm.playLoopOn = st.playLoopOn;
  vm.playSeamOn = st.playSeamOn;
  vm.findBusy = st.findBusy;
  vm.writeSmpl = st.writeSmpl;
  vm.hover = m_hover;
  vm.weldDragging = m_weldDrag;

  m_canvas.RenderLoopLabPanel(hdc, pr.left, pr.top, pr.right - pr.left,
                              pr.bottom - pr.top, dpr, vm);
#else
  (void)hdc; (void)wr; (void)dpr; (void)st;
#endif
}
