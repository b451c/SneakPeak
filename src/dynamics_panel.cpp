// dynamics_panel.cpp — Inline dynamics control panel with real-time sliders
#include "dynamics_panel.h"
#include "theme.h"
#include "config.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

// Slider definitions: label, min, max, unit, precision
const DynamicsPanel::SliderDef DynamicsPanel::SLIDER_DEFS[NUM_SLIDERS] = {
  { "Target",  -60.0,   12.0, "dB", 1 },
  { "Above",     0.0,  200.0, "%",  0 },
  { "Below",     0.0,  200.0, "%",  0 },
  { "Attack",    0.0,  500.0, "ms", 0 },
  { "Release",   0.0, 1000.0, "ms", 0 },
};

// --- Layout constants (relative to panel left/top) ---
static constexpr int MARGIN = 4;
static constexpr int LABEL_W = 42;
static constexpr int LABEL_GAP = 4;
static constexpr int LEFT_TRACK_W = 74;
static constexpr int RIGHT_TRACK_W = 62;
static constexpr int VALUE_GAP = 4;
// COL_GAP removed — columns defined by absolute x-offsets below

// Left column: label starts at MARGIN, track starts after label
static constexpr int L_TRACK_X = MARGIN + LABEL_W + LABEL_GAP;         // 50
static constexpr int L_VALUE_X = L_TRACK_X + LEFT_TRACK_W + VALUE_GAP; // 128

// Right column: starts after left column values + gap
static constexpr int R_LABEL_X = 188;
static constexpr int R_TRACK_X = R_LABEL_X + LABEL_W + LABEL_GAP;     // 234
static constexpr int R_VALUE_X = R_TRACK_X + RIGHT_TRACK_W + VALUE_GAP; // 300

// Apply button
static constexpr int APPLY_W = 50;
static constexpr int APPLY_H = 14;

// --- Value access ---

double DynamicsPanel::GetSliderValue(int idx) const
{
  switch (idx) {
    case 0: return m_params.targetDb;
    case 1: return m_params.compressAbove;
    case 2: return m_params.compressBelow;
    case 3: return m_params.attackMs;
    case 4: return m_params.releaseMs;
    default: return 0.0;
  }
}

void DynamicsPanel::SetSliderValue(int idx, double val)
{
  const auto& def = SLIDER_DEFS[idx];
  val = std::max(def.minVal, std::min(def.maxVal, val));
  switch (idx) {
    case 0: m_params.targetDb = val; break;
    case 1: m_params.compressAbove = val; break;
    case 2: m_params.compressBelow = val; break;
    case 3: m_params.attackMs = val; break;
    case 4: m_params.releaseMs = val; break;
  }
}

// --- Lifecycle ---

void DynamicsPanel::Show(const DynamicsParams& params, double avgPeakDb)
{
  m_params = params;
  m_avgPeakDb = avgPeakDb;
  // Resolve sentinel target (-100 = use average peak)
  if (m_params.targetDb <= -99.0)
    m_params.targetDb = avgPeakDb;
  m_visible = true;
  m_paramsChanged = false;
  m_applyRequested = false;
  m_dragSlider = -1;
}

void DynamicsPanel::Hide()
{
  m_visible = false;
  m_dragSlider = -1;
  m_panelDragging = false;
}

// --- Layout ---

RECT DynamicsPanel::GetRect(RECT wr) const
{
  int cx = (wr.left + wr.right) / 2 + m_offsetX;
  int cy = wr.bottom - PANEL_H - 10 + m_offsetY;
  int left = cx - PANEL_W / 2;
  // Clamp to waveform bounds
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
  int x = pr.left + R_TRACK_X;
  int y = pr.top + TITLE_H + 2 * ROW_H + 2;
  return { x, y, x + APPLY_W, y + APPLY_H };
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
    // Expand hit zone vertically for easier grabbing
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

  // Slider hit
  int slider = HitTestSlider(x, y, pr);
  if (slider >= 0) {
    m_dragSlider = slider;
    RECT tr = GetSliderTrackRect(pr, slider);
    double val = PixelToValue(x, tr, slider);
    SetSliderValue(slider, val);
    m_paramsChanged = true;
    return true;
  }

  // Panel drag (anywhere else inside panel)
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
    double val = PixelToValue(x, tr, m_dragSlider);
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

  // Title
  {
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, RGB(255, 160, 40));
    RECT titleR = { pr.left + MARGIN + 2, pr.top + 4, pr.right - 16, pr.top + TITLE_H };
    DrawText(hdc, "DYNAMICS", -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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

    // Filled portion (left of thumb)
    if (thumbX > tr.left) {
      OwnedPen fillPen(PS_SOLID, TRACK_H, RGB(255, 160, 40));
      DCPenScope scope(hdc, fillPen);
      MoveToEx(hdc, tr.left, cy, nullptr);
      LineTo(hdc, thumbX, cy);
    }

    // Thumb circle
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
      if (def.precision == 1)
        snprintf(text, sizeof(text), "%.1f %s", val, def.unit);
      else
        snprintf(text, sizeof(text), "%.0f %s", val, def.unit);

      HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
      SetTextColor(hdc, RGB(220, 220, 220));
      int valX = pr.left + ((col == 0) ? L_VALUE_X : R_VALUE_X);
      RECT vr = { valX, tr.top - 7, pr.right - MARGIN, tr.bottom + 7 };
      DrawText(hdc, text, -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(hdc, oldFont);
    }
  }

  // Apply button
  {
    RECT ar = GetApplyButtonRect(pr);

    // Button border
    OwnedPen btnPen(PS_SOLID, 1, RGB(255, 160, 40));
    DCPenScope scope(hdc, btnPen);
    MoveToEx(hdc, ar.left, ar.top, nullptr);
    LineTo(hdc, ar.right - 1, ar.top);
    LineTo(hdc, ar.right - 1, ar.bottom - 1);
    LineTo(hdc, ar.left, ar.bottom - 1);
    LineTo(hdc, ar.left, ar.top);

    // Button text
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, RGB(255, 160, 40));
    DrawText(hdc, "Apply", -1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }
}
