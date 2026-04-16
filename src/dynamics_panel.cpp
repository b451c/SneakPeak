// dynamics_panel.cpp — Professional dynamics control panel
#include "dynamics_panel.h"
#include "theme.h"
#include "config.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

// Slider definitions: label, min, max, unit, precision
const DynamicsPanel::SliderDef DynamicsPanel::SLIDER_DEFS[NUM_SLIDERS] = {
  { "Thresh",  -60.0,    0.0, "dB",  1 },  // 0: left col row 0
  { "Ratio",     1.0,   20.0, ":1",  1 },  // 1: left col row 1
  { "Knee",      0.0,   24.0, "dB",  0 },  // 2: left col row 2
  { "Attack",    0.0,  500.0, "ms",  0 },  // 3: right col row 0
  { "Release",   0.0, 1000.0, "ms",  0 },  // 4: right col row 1
  { "Makeup",    0.0,   24.0, "dB",  1 },  // 5: right col row 2
};

// --- Layout constants ---
static constexpr int MARGIN = 4;
static constexpr int LABEL_W = 42;
static constexpr int LABEL_GAP = 4;
static constexpr int LEFT_TRACK_W = 74;
static constexpr int RIGHT_TRACK_W = 62;
static constexpr int VALUE_GAP = 4;

static constexpr int L_TRACK_X = MARGIN + LABEL_W + LABEL_GAP;
static constexpr int L_VALUE_X = L_TRACK_X + LEFT_TRACK_W + VALUE_GAP;

static constexpr int R_LABEL_X = 196;
static constexpr int R_TRACK_X = R_LABEL_X + LABEL_W + LABEL_GAP;
static constexpr int R_VALUE_X = R_TRACK_X + RIGHT_TRACK_W + VALUE_GAP;

static constexpr int APPLY_W = 50;
static constexpr int APPLY_H = 14;
static constexpr int TOGGLE_W = 28;
static constexpr int TOGGLE_GAP = 3;

// --- Value access ---

double DynamicsPanel::GetSliderValue(int idx) const
{
  switch (idx) {
    case 0: return m_params.threshold;
    case 1: return m_params.ratio;
    case 2: return m_params.kneeDb;
    case 3: return m_params.attackMs;
    case 4: return m_params.releaseMs;
    case 5: return m_params.autoMakeup ? 0.0 : m_params.makeupDb;
    default: return 0.0;
  }
}

void DynamicsPanel::SetSliderValue(int idx, double val)
{
  const auto& def = SLIDER_DEFS[idx];
  val = std::max(def.minVal, std::min(def.maxVal, val));
  switch (idx) {
    case 0: m_params.threshold = val; break;
    case 1: m_params.ratio = val; break;
    case 2: m_params.kneeDb = val; break;
    case 3: m_params.attackMs = val; break;
    case 4: m_params.releaseMs = val; break;
    case 5: m_params.makeupDb = val; m_params.autoMakeup = false; break;
  }
}

// --- Lifecycle ---

void DynamicsPanel::Show(const DynamicsParams& params, double avgPeakDb)
{
  m_params = params;
  m_avgPeakDb = avgPeakDb;
  if (m_params.threshold <= -99.0)
    m_params.threshold = avgPeakDb;
  m_visible = true;
  m_paramsChanged = false;
  m_applyRequested = false;
  m_dragSlider = -1;
  m_avgGR = 0.0;
  // Reset toggles to show everything on panel open
  m_showDyn = true;
  m_showEnv = true;
  m_liveMode = false;
  m_liveUndoOpen = false;
}

void DynamicsPanel::Hide()
{
  m_visible = false;
  m_dragSlider = -1;
  m_panelDragging = false;
  m_liveMode = false;
  // Note: live undo block closure handled by SneakPeak (needs g_Undo_EndBlock2)
}

// --- Layout ---

RECT DynamicsPanel::GetRect(RECT wr) const
{
  int cx = (wr.left + wr.right) / 2 + m_offsetX;
  int cy = wr.bottom - PANEL_H - 10 + m_offsetY;
  int left = cx - PANEL_W / 2;
  if (left < wr.left) left = wr.left;
  if (left + PANEL_W > wr.right) left = wr.right - PANEL_W;
  if (cy < wr.top) cy = wr.top;
  if (cy + PANEL_H > wr.bottom) cy = wr.bottom - PANEL_H;
  return { left, cy, left + PANEL_W, cy + PANEL_H };
}

RECT DynamicsPanel::GetSliderTrackRect(RECT pr, int idx) const
{
  int col = SliderCol(idx);
  int row = SliderRow(idx);
  int trackX = pr.left + ((col == 0) ? L_TRACK_X : R_TRACK_X);
  int trackW = (col == 0) ? LEFT_TRACK_W : RIGHT_TRACK_W;
  int cy = pr.top + TITLE_H + row * ROW_H + ROW_H / 2;
  return { trackX, cy - TRACK_H / 2, trackX + trackW, cy + TRACK_H / 2 + 1 };
}

RECT DynamicsPanel::GetApplyButtonRect(RECT pr) const
{
  int x = pr.right - MARGIN - APPLY_W - 2;
  int y = pr.top + TITLE_H + 3 * ROW_H + 10;
  return { x, y, x + APPLY_W, y + APPLY_H };
}

RECT DynamicsPanel::GetRmsToggleRect(RECT pr) const
{
  int x = pr.left + R_LABEL_X;
  int y = pr.top + TITLE_H + 3 * ROW_H + 10;
  return { x, y, x + 42, y + APPLY_H };
}

RECT DynamicsPanel::GetDynToggleRect(RECT pr) const
{
  int x = pr.left + MARGIN;
  int y = pr.top + TITLE_H + 3 * ROW_H + 10;
  return { x, y, x + TOGGLE_W, y + APPLY_H };
}

RECT DynamicsPanel::GetEnvToggleRect(RECT pr) const
{
  RECT dyn = GetDynToggleRect(pr);
  int x = dyn.right + TOGGLE_GAP;
  int y = dyn.top;
  return { x, y, x + TOGGLE_W, y + APPLY_H };
}

RECT DynamicsPanel::GetLiveToggleRect(RECT pr) const
{
  RECT envR = GetEnvToggleRect(pr);
  int x = envR.right + TOGGLE_GAP;
  int y = envR.top;
  return { x, y, x + TOGGLE_W + 4, y + APPLY_H }; // slightly wider for "Live"
}

RECT DynamicsPanel::GetCloseButtonRect(RECT pr) const
{
  return { pr.right - 16, pr.top + 4, pr.right - 2, pr.top + 18 };
}

// --- Coordinate conversion ---

int DynamicsPanel::ValueToPixel(double val, RECT tr, int idx) const
{
  const auto& def = SLIDER_DEFS[idx];
  double ratio = (val - def.minVal) / (def.maxVal - def.minVal);
  ratio = std::max(0.0, std::min(1.0, ratio));
  return tr.left + (int)(ratio * (double)(tr.right - tr.left));
}

double DynamicsPanel::PixelToValue(int px, RECT tr, int idx) const
{
  const auto& def = SLIDER_DEFS[idx];
  double ratio = (double)(px - tr.left) / (double)(tr.right - tr.left);
  ratio = std::max(0.0, std::min(1.0, ratio));
  return def.minVal + ratio * (def.maxVal - def.minVal);
}

bool DynamicsPanel::IsFineMode()
{
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

// --- Hit testing ---

bool DynamicsPanel::HitTest(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  RECT r = GetRect(wr);
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

int DynamicsPanel::HitTestSlider(int x, int y, RECT pr) const
{
  for (int i = 0; i < NUM_SLIDERS; i++) {
    RECT tr = GetSliderTrackRect(pr, i);
    if (x >= tr.left - THUMB_R && x <= tr.right + THUMB_R &&
        y >= tr.top - 8 && y <= tr.bottom + 8)
      return i;
  }
  return -1;
}

// --- Mouse interaction ---

bool DynamicsPanel::OnMouseDown(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  if (!HitTest(x, y, wr)) return false;

  RECT pr = GetRect(wr);

  // Close button
  RECT closeR = GetCloseButtonRect(pr);
  if (x >= closeR.left && x < closeR.right && y >= closeR.top && y < closeR.bottom) {
    Hide();
    return true;
  }

  // Apply button
  RECT applyR = GetApplyButtonRect(pr);
  if (x >= applyR.left && x < applyR.right && y >= applyR.top && y < applyR.bottom) {
    m_applyRequested = true;
    return true;
  }

  // Peak/RMS toggle
  RECT rmsR = GetRmsToggleRect(pr);
  if (x >= rmsR.left && x < rmsR.right && y >= rmsR.top && y < rmsR.bottom) {
    m_params.rmsMode = !m_params.rmsMode;
    m_paramsChanged = true;
    return true;
  }

  // Dyn visibility toggle
  RECT dynR = GetDynToggleRect(pr);
  if (x >= dynR.left && x < dynR.right && y >= dynR.top && y < dynR.bottom) {
    m_showDyn = !m_showDyn;
    return true;
  }

  // Env visibility toggle
  RECT envR = GetEnvToggleRect(pr);
  if (x >= envR.left && x < envR.right && y >= envR.top && y < envR.bottom) {
    m_showEnv = !m_showEnv;
    return true;
  }

  // Live toggle
  RECT liveR = GetLiveToggleRect(pr);
  if (x >= liveR.left && x < liveR.right && y >= liveR.top && y < liveR.bottom) {
    m_liveMode = !m_liveMode;
    if (m_liveMode) m_applyRequested = true; // initial apply when turning on
    return true;
  }

  // Slider hit
  int slider = HitTestSlider(x, y, pr);
  if (slider >= 0) {
    m_dragSlider = slider;
    m_dragStartX = x;
    m_dragStartVal = GetSliderValue(slider);
    // Grab offset: distance from click to current thumb center.
    // This prevents the value from jumping when you grab near (not on) the thumb.
    RECT tr = GetSliderTrackRect(pr, slider);
    int thumbX = ValueToPixel(m_dragStartVal, tr, slider);
    m_dragGrabOffset = thumbX - x;
    // Don't set value on click - start dragging from current position
    return true;
  }

  // Panel drag
  m_panelDragging = true;
  m_dragOffsetX = x - pr.left;
  m_dragOffsetY = y - pr.top;
  return true;
}

void DynamicsPanel::OnMouseMove(int x, int y, RECT wr)
{
  if (m_dragSlider >= 0) {
    RECT pr = GetRect(wr);
    RECT tr = GetSliderTrackRect(pr, m_dragSlider);

    double val;
    if (IsFineMode()) {
      // Fine mode (Cmd/Ctrl): delta-based, 1/5th sensitivity from drag start value
      int dx = x - m_dragStartX;
      double pxRange = (double)(tr.right - tr.left);
      const auto& def = SLIDER_DEFS[m_dragSlider];
      double valRange = def.maxVal - def.minVal;
      double delta = ((double)dx / pxRange) * valRange * 0.2;
      val = m_dragStartVal + delta;
    } else {
      // Normal mode: absolute position with grab offset (no jump on click)
      val = PixelToValue(x + m_dragGrabOffset, tr, m_dragSlider);
    }
    SetSliderValue(m_dragSlider, val);
    m_paramsChanged = true;
  }
  else if (m_panelDragging) {
    int defaultCX = (wr.left + wr.right) / 2;
    int defaultCY = wr.bottom - PANEL_H - 10;
    int newLeft = x - m_dragOffsetX;
    int newTop = y - m_dragOffsetY;
    m_offsetX = (newLeft + PANEL_W / 2) - defaultCX;
    m_offsetY = newTop - defaultCY;
  }
}

void DynamicsPanel::OnMouseUp()
{
  m_dragSlider = -1;
  m_panelDragging = false;
}

// --- Drawing ---

void DynamicsPanel::Draw(HDC hdc, RECT wr)
{
  if (!m_visible) return;

  RECT pr = GetRect(wr);

  // Panel background
  {
    OwnedBrush bg(RGB(40, 40, 40));
    bg.Fill(hdc, &pr);
  }

  // Border
  {
    OwnedPen border(PS_SOLID, 1, RGB(80, 80, 80));
    DCPenScope scope(hdc, border);
    MoveToEx(hdc, pr.left, pr.top, nullptr);
    LineTo(hdc, pr.right - 1, pr.top);
    LineTo(hdc, pr.right - 1, pr.bottom - 1);
    LineTo(hdc, pr.left, pr.bottom - 1);
    LineTo(hdc, pr.left, pr.top);
  }

  SetBkMode(hdc, TRANSPARENT);

  // Title + GR meter
  {
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, RGB(255, 160, 40));
    RECT titleR = { pr.left + MARGIN + 2, pr.top + 4, pr.left + 90, pr.top + TITLE_H };
    DrawText(hdc, "DYNAMICS", -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // GR meter bar (horizontal, right of title)
    if (m_avgGR < -0.1) {
      int meterX = pr.left + 92;
      int meterY = pr.top + 8;
      int meterMaxW = pr.right - 60 - meterX;
      double grNorm = std::min(1.0, fabs(m_avgGR) / 20.0); // 0..1 for 0..-20dB
      int meterW = (int)(grNorm * meterMaxW);

      OwnedBrush grBrush(RGB(220, 100, 40));
      RECT meterR = { meterX, meterY, meterX + meterW, meterY + 6 };
      grBrush.Fill(hdc, &meterR);

      // GR label
      SetTextColor(hdc, RGB(220, 100, 40));
      char grText[16];
      snprintf(grText, sizeof(grText), "%.1f dB", m_avgGR);
      RECT grLabelR = { meterX + meterW + 3, pr.top + 4, pr.right - 18, pr.top + TITLE_H };
      SelectObject(hdc, g_fonts.normal11);
      DrawText(hdc, grText, -1, &grLabelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, oldFont);
  }

  // Close button [x]
  {
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, RGB(140, 140, 140));
    RECT closeR = GetCloseButtonRect(pr);
    DrawText(hdc, "x", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }

  // Sliders
  for (int i = 0; i < NUM_SLIDERS; i++) {
    const auto& def = SLIDER_DEFS[i];
    int col = SliderCol(i);
    double val = GetSliderValue(i);
    RECT tr = GetSliderTrackRect(pr, i);
    int thumbX = ValueToPixel(val, tr, i);
    int cy = (tr.top + tr.bottom) / 2;

    // Label
    {
      HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);
      SetTextColor(hdc, RGB(160, 160, 160));
      int labelX = pr.left + ((col == 0) ? MARGIN : R_LABEL_X);
      RECT lr = { labelX, tr.top - 7, labelX + LABEL_W, tr.bottom + 7 };
      DrawText(hdc, def.label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(hdc, oldFont);
    }

    // Track background
    {
      OwnedPen trackPen(PS_SOLID, TRACK_H, RGB(60, 60, 60));
      DCPenScope scope(hdc, trackPen);
      MoveToEx(hdc, tr.left, cy, nullptr);
      LineTo(hdc, tr.right, cy);
    }

    // Filled portion
    if (thumbX > tr.left) {
      OwnedPen fillPen(PS_SOLID, TRACK_H, RGB(255, 160, 40));
      DCPenScope scope(hdc, fillPen);
      MoveToEx(hdc, tr.left, cy, nullptr);
      LineTo(hdc, thumbX, cy);
    }

    // Thumb
    {
      bool active = (m_dragSlider == i);
      int r = active ? THUMB_R + 1 : THUMB_R;
      OwnedBrush thumbBr(RGB(220, 220, 220));
      OwnedPen thumbPen(PS_SOLID, 1, RGB(180, 180, 180));
      HBRUSH oldBr = (HBRUSH)SelectObject(hdc, (HBRUSH)thumbBr);
      HPEN oldPen = (HPEN)SelectObject(hdc, (HPEN)thumbPen);
      Ellipse(hdc, thumbX - r, cy - r, thumbX + r, cy + r);
      SelectObject(hdc, oldBr);
      SelectObject(hdc, oldPen);
    }

    // Value text
    {
      char text[32];
      if (i == 5 && m_params.autoMakeup) {
        // Makeup: show auto-computed value
        snprintf(text, sizeof(text), "%.1f auto", fabs(m_avgGR));
      } else if (def.precision == 1) {
        snprintf(text, sizeof(text), "%.1f %s", val, def.unit);
      } else {
        snprintf(text, sizeof(text), "%.0f %s", val, def.unit);
      }

      HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
      SetTextColor(hdc, RGB(220, 220, 220));
      int valX = pr.left + ((col == 0) ? L_VALUE_X : R_VALUE_X);
      RECT vr = { valX, tr.top - 7, pr.right - MARGIN, tr.bottom + 7 };
      DrawText(hdc, text, -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(hdc, oldFont);
    }
  }

  // Bottom row: Peak/RMS toggle + Apply button
  {
    // Peak/RMS toggle
    RECT rmsR = GetRmsToggleRect(pr);
    {
      OwnedPen btnPen(PS_SOLID, 1, m_params.rmsMode ? RGB(255, 160, 40) : RGB(80, 80, 80));
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, rmsR.left, rmsR.top, nullptr);
      LineTo(hdc, rmsR.right - 1, rmsR.top);
      LineTo(hdc, rmsR.right - 1, rmsR.bottom - 1);
      LineTo(hdc, rmsR.left, rmsR.bottom - 1);
      LineTo(hdc, rmsR.left, rmsR.top);
    }
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, m_params.rmsMode ? RGB(255, 160, 40) : RGB(160, 160, 160));
    DrawText(hdc, m_params.rmsMode ? "RMS" : "Peak", -1, &rmsR,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Dyn toggle
    RECT dynR = GetDynToggleRect(pr);
    {
      COLORREF col = m_showDyn ? RGB(200, 130, 50) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, dynR.left, dynR.top, nullptr);
      LineTo(hdc, dynR.right - 1, dynR.top);
      LineTo(hdc, dynR.right - 1, dynR.bottom - 1);
      LineTo(hdc, dynR.left, dynR.bottom - 1);
      LineTo(hdc, dynR.left, dynR.top);
    }
    SetTextColor(hdc, m_showDyn ? RGB(200, 130, 50) : RGB(80, 80, 80));
    DrawText(hdc, "Dyn", -1, &dynR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Env toggle
    RECT envR = GetEnvToggleRect(pr);
    {
      COLORREF col = m_showEnv ? RGB(0, 180, 220) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, envR.left, envR.top, nullptr);
      LineTo(hdc, envR.right - 1, envR.top);
      LineTo(hdc, envR.right - 1, envR.bottom - 1);
      LineTo(hdc, envR.left, envR.bottom - 1);
      LineTo(hdc, envR.left, envR.top);
    }
    SetTextColor(hdc, m_showEnv ? RGB(0, 180, 220) : RGB(80, 80, 80));
    DrawText(hdc, "Env", -1, &envR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Live toggle
    RECT liveR = GetLiveToggleRect(pr);
    {
      COLORREF col = m_liveMode ? RGB(100, 220, 100) : RGB(60, 60, 60);
      OwnedPen btnPen(PS_SOLID, 1, col);
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, liveR.left, liveR.top, nullptr);
      LineTo(hdc, liveR.right - 1, liveR.top);
      LineTo(hdc, liveR.right - 1, liveR.bottom - 1);
      LineTo(hdc, liveR.left, liveR.bottom - 1);
      LineTo(hdc, liveR.left, liveR.top);
    }
    SetTextColor(hdc, m_liveMode ? RGB(100, 220, 100) : RGB(80, 80, 80));
    DrawText(hdc, "Live", -1, &liveR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Apply button
    RECT ar = GetApplyButtonRect(pr);
    {
      OwnedPen btnPen(PS_SOLID, 1, RGB(255, 160, 40));
      DCPenScope scope(hdc, btnPen);
      MoveToEx(hdc, ar.left, ar.top, nullptr);
      LineTo(hdc, ar.right - 1, ar.top);
      LineTo(hdc, ar.right - 1, ar.bottom - 1);
      LineTo(hdc, ar.left, ar.bottom - 1);
      LineTo(hdc, ar.left, ar.top);
    }
    SetTextColor(hdc, RGB(255, 160, 40));
    DrawText(hdc, "Apply", -1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }
}
