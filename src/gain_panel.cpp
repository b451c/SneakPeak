// gain_panel.cpp — Floating gain knob overlay (non-destructive, uses D_VOL)
#include "gain_panel.h"
#include "globals.h"
#include "config.h"
#include "theme.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void GainPanel::Show(MediaItem* item)
{
  if (!item) return;
  m_item = item;
  m_batchItems.clear();
  m_batchOrigVols.clear();
  m_visible = true;
  ReadFromItem();
}

void GainPanel::ShowBatch(const std::vector<MediaItem*>& items)
{
  if (items.empty()) return;
  m_batchItems = items;
  m_batchOrigVols.clear();
  m_item = items[0]; // primary item for compatibility
  m_standalone = false;
  m_visible = true;

  // Store original D_VOL for each item
  for (MediaItem* it : m_batchItems) {
    double vol = 1.0;
    if (g_GetMediaItemInfo_Value) vol = g_GetMediaItemInfo_Value(it, "D_VOL");
    if (vol <= 0.0) vol = 1.0;
    m_batchOrigVols.push_back(vol);
  }
  m_db = 0.0; // knob starts at 0 dB (no offset)
}

void GainPanel::ShowStandalone()
{
  m_item = nullptr;
  m_batchItems.clear();
  m_batchOrigVols.clear();
  m_standalone = true;
  m_visible = true;
  m_db = 0.0;
}

void GainPanel::Toggle(MediaItem* item)
{
  if (m_visible) Hide();
  else Show(item);
}

void GainPanel::ReadFromItem()
{
  if (!m_batchItems.empty()) return; // batch mode: knob is relative offset, don't read
  if (!m_item || !g_GetMediaItemInfo_Value) { m_db = 0.0; return; }
  double vol = g_GetMediaItemInfo_Value(m_item, "D_VOL");
  if (vol <= 0.0) vol = 1.0;
  m_db = 20.0 * log10(vol);
  m_db = std::max(MIN_DB, std::min(MAX_DB, m_db));
}

void GainPanel::WriteToItem()
{
  if (!g_SetMediaItemInfo_Value || !g_UpdateArrange) return;

  // Batch mode: apply relative offset to each item's original volume
  if (!m_batchItems.empty()) {
    double offsetLinear = pow(10.0, m_db / 20.0);
    for (size_t i = 0; i < m_batchItems.size(); i++) {
      double origVol = (i < m_batchOrigVols.size()) ? m_batchOrigVols[i] : 1.0;
      g_SetMediaItemInfo_Value(m_batchItems[i], "D_VOL", origVol * offsetLinear);
    }
    g_UpdateArrange();
    return;
  }

  if (!m_item) return;
  double linear = pow(10.0, m_db / 20.0);
  g_SetMediaItemInfo_Value(m_item, "D_VOL", linear);
  g_UpdateArrange();
}

void GainPanel::ResetTo0dB()
{
  if (!m_batchItems.empty()) {
    // Restore original volumes
    m_db = 0.0;
    if (g_SetMediaItemInfo_Value) {
      for (size_t i = 0; i < m_batchItems.size(); i++) {
        double origVol = (i < m_batchOrigVols.size()) ? m_batchOrigVols[i] : 1.0;
        g_SetMediaItemInfo_Value(m_batchItems[i], "D_VOL", origVol);
      }
      if (g_UpdateArrange) g_UpdateArrange();
    }
    return;
  }
  m_db = 0.0;
  WriteToItem();
}

double GainPanel::DbToAngle(double db) const
{
  double ratio = (db - MIN_DB) / (MAX_DB - MIN_DB);
  ratio = std::max(0.0, std::min(1.0, ratio));
  return ARC_START - ratio * ARC_RANGE; // CW from start
}

double GainPanel::AngleToDb(double angle) const
{
  double ratio = (ARC_START - angle) / ARC_RANGE;
  ratio = std::max(0.0, std::min(1.0, ratio));
  return MIN_DB + ratio * (MAX_DB - MIN_DB);
}

RECT GainPanel::GetRect(RECT waveformRect) const
{
  int cx = (waveformRect.left + waveformRect.right) / 2 + m_offsetX;
  int cy = waveformRect.top + 30 + m_offsetY;
  int left = cx - PANEL_W / 2;
  int top = cy;
  if (left < waveformRect.left) left = waveformRect.left;
  if (left + PANEL_W > waveformRect.right) left = waveformRect.right - PANEL_W;
  if (top < waveformRect.top) top = waveformRect.top;
  if (top + PANEL_H > waveformRect.bottom) top = waveformRect.bottom - PANEL_H;
  return { left, top, left + PANEL_W, top + PANEL_H };
}

bool GainPanel::HitTest(int x, int y, RECT waveformRect) const
{
  if (!m_visible) return false;
  RECT r = GetRect(waveformRect);
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool GainPanel::OnDoubleClick(int x, int y, RECT waveformRect)
{
  if (!m_visible) return false;
  if (!HitTest(x, y, waveformRect)) return false;
  ResetTo0dB();
  return true;
}

bool GainPanel::IsFineMode()
{
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool GainPanel::OnMouseDown(int x, int y, RECT waveformRect)
{
  if (!m_visible) return false;
  if (!HitTest(x, y, waveformRect)) return false;

  RECT r = GetRect(waveformRect);

  // Close button (top-right 14px)
  if (x >= r.right - 14 && y < r.top + 14) {
    Hide();
    return true;
  }

  // Knob area: left 32px of panel
  if (x < r.left + 32) {
    m_knobDragging = true;
    m_dragAnchorY = y;
    m_dragAnchorDb = m_db;
    return true;
  }

  // Otherwise: panel drag
  m_panelDragging = true;
  m_dragOffsetX = x - r.left;
  m_dragOffsetY = y - r.top;
  return true;
}

void GainPanel::OnMouseMove(int x, int y, RECT waveformRect)
{
  if (m_knobDragging) {
    // Vertical drag: up = louder, down = quieter
    int dy = m_dragAnchorY - y; // positive = up = increase
    double sensitivity = IsFineMode() ? 0.1 : 0.5; // dB per pixel
    m_db = m_dragAnchorDb + (double)dy * sensitivity;
    m_db = std::max(MIN_DB, std::min(MAX_DB, m_db));
    WriteToItem();
  }
  else if (m_panelDragging) {
    int defaultCX = (waveformRect.left + waveformRect.right) / 2;
    int defaultCY = waveformRect.top + 30;
    int newLeft = x - m_dragOffsetX;
    int newTop = y - m_dragOffsetY;
    m_offsetX = (newLeft + PANEL_W / 2) - defaultCX;
    m_offsetY = newTop - defaultCY;
  }
}

void GainPanel::OnMouseUp()
{
  m_knobDragging = false;
  m_panelDragging = false;
}

void GainPanel::Draw(HDC hdc, RECT waveformRect)
{
  if (!m_visible) return;

  if (m_item && !m_knobDragging) ReadFromItem();

  RECT r = GetRect(waveformRect);

  // Panel background
  HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
  FillRect(hdc, &r, bg);
  DeleteObject(bg);

  // Border
  HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, r.left, r.top, nullptr);
  LineTo(hdc, r.right - 1, r.top);
  LineTo(hdc, r.right - 1, r.bottom - 1);
  LineTo(hdc, r.left, r.bottom - 1);
  LineTo(hdc, r.left, r.top);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(hdc, TRANSPARENT);

  // --- Knob ---
  int knobCX = r.left + 18;
  int knobCY = (r.top + r.bottom) / 2;
  int kr = KNOB_RADIUS;

  // Knob background circle
  HBRUSH knobBg = CreateSolidBrush(RGB(55, 55, 55));
  HPEN knobOutline = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
  HBRUSH oldBr = (HBRUSH)SelectObject(hdc, knobBg);
  oldPen = (HPEN)SelectObject(hdc, knobOutline);
  Ellipse(hdc, knobCX - kr, knobCY - kr, knobCX + kr, knobCY + kr);
  SelectObject(hdc, oldBr);
  SelectObject(hdc, oldPen);
  DeleteObject(knobBg);
  DeleteObject(knobOutline);

  // Arc track (gray) — draw as series of small segments
  HPEN arcTrack = CreatePen(PS_SOLID, 2, RGB(80, 80, 80));
  oldPen = (HPEN)SelectObject(hdc, arcTrack);
  {
    int arcR = kr - 2;
    for (int i = 0; i < 24; i++) {
      double frac = (double)i / 24.0;
      double angleDeg = ARC_START - frac * ARC_RANGE;
      double angleRad = angleDeg * M_PI / 180.0;
      int ax = knobCX + (int)(cos(angleRad) * arcR);
      int ay = knobCY - (int)(sin(angleRad) * arcR);
      if (i == 0) MoveToEx(hdc, ax, ay, nullptr);
      else LineTo(hdc, ax, ay);
    }
  }
  SelectObject(hdc, oldPen);
  DeleteObject(arcTrack);

  // Active arc (colored)
  COLORREF activeColor = (m_db >= 0) ? RGB(220, 80, 60) : RGB(60, 180, 120);
  HPEN arcActive = CreatePen(PS_SOLID, 2, activeColor);
  oldPen = (HPEN)SelectObject(hdc, arcActive);
  {
    int arcR = kr - 2;
    double zeroAngle = DbToAngle(0.0);
    double valAngle = DbToAngle(m_db);
    double startAng = std::min(zeroAngle, valAngle);
    double endAng = std::max(zeroAngle, valAngle);
    double sweepDeg = endAng - startAng;
    int steps = std::max(2, (int)(sweepDeg / 3.0));
    for (int i = 0; i <= steps; i++) {
      double angleDeg = startAng + (endAng - startAng) * (double)i / (double)steps;
      double angleRad = angleDeg * M_PI / 180.0;
      int ax = knobCX + (int)(cos(angleRad) * arcR);
      int ay = knobCY - (int)(sin(angleRad) * arcR);
      if (i == 0) MoveToEx(hdc, ax, ay, nullptr);
      else LineTo(hdc, ax, ay);
    }
  }
  SelectObject(hdc, oldPen);
  DeleteObject(arcActive);

  // Pointer line
  double ptrAngleDeg = DbToAngle(m_db);
  double ptrAngleRad = ptrAngleDeg * M_PI / 180.0;
  int ptrInner = 3;
  int ptrOuter = kr - 1;
  HPEN ptrPen = CreatePen(PS_SOLID, 2, RGB(220, 220, 220));
  oldPen = (HPEN)SelectObject(hdc, ptrPen);
  MoveToEx(hdc, knobCX + (int)(cos(ptrAngleRad) * ptrInner),
                knobCY - (int)(sin(ptrAngleRad) * ptrInner), nullptr);
  LineTo(hdc, knobCX + (int)(cos(ptrAngleRad) * ptrOuter),
              knobCY - (int)(sin(ptrAngleRad) * ptrOuter));
  SelectObject(hdc, oldPen);
  DeleteObject(ptrPen);

  // --- dB readout ---
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);

  char dbText[16];
  if (m_db <= MIN_DB + 0.5)
    snprintf(dbText, sizeof(dbText), "-inf");
  else
    snprintf(dbText, sizeof(dbText), "%+.1f dB", m_db);

  SetTextColor(hdc, RGB(100, 200, 255));
  RECT dbRect = { r.left + 32, r.top + 2, r.right - 14, r.bottom - 2 };
  DrawText(hdc, dbText, -1, &dbRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Close "x" button
  SetTextColor(hdc, RGB(140, 140, 140));
  RECT xRect = { r.right - 14, r.top + 1, r.right - 1, r.top + 14 };
  DrawText(hdc, "x", -1, &xRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectObject(hdc, oldFont);
}
