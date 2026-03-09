// gain_panel.cpp — Floating gain slider overlay (non-destructive, uses D_VOL)
#include "gain_panel.h"
#include "globals.h"
#include "config.h"
#include "theme.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

void GainPanel::Show(MediaItem* item)
{
  if (!item) return;
  m_item = item;
  m_visible = true;
  ReadFromItem();
}

void GainPanel::Toggle(MediaItem* item)
{
  if (m_visible) Hide();
  else Show(item);
}

void GainPanel::ReadFromItem()
{
  if (!m_item || !g_GetMediaItemInfo_Value) { m_db = 0.0; return; }
  double vol = g_GetMediaItemInfo_Value(m_item, "D_VOL");
  if (vol <= 0.0) vol = 1.0;
  m_db = 20.0 * log10(vol);
  m_db = std::max(MIN_DB, std::min(MAX_DB, m_db));
}

void GainPanel::WriteToItem()
{
  if (!m_item || !g_SetMediaItemInfo_Value || !g_UpdateArrange) return;
  double linear = pow(10.0, m_db / 20.0);
  g_SetMediaItemInfo_Value(m_item, "D_VOL", linear);
  g_UpdateArrange();
}

void GainPanel::ResetTo0dB()
{
  m_db = 0.0;
  WriteToItem();
}

RECT GainPanel::GetRect(RECT waveformRect) const
{
  int cx = (waveformRect.left + waveformRect.right) / 2 + m_offsetX;
  int cy = waveformRect.top + 30 + m_offsetY;
  // Clamp to stay within waveform area
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

bool GainPanel::SliderHitTest(int x, int y, RECT waveformRect) const
{
  if (!m_visible) return false;
  RECT r = GetRect(waveformRect);
  int sliderL = r.left + 30;
  int sliderR = r.right - 52;
  return x >= sliderL && x <= sliderR && y >= r.top && y < r.bottom;
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

double GainPanel::SliderXToDb(int x, RECT r) const
{
  int sliderL = r.left + 32;
  int sliderR = r.right - 54;
  if (IsFineMode()) {
    // Fine mode: pixel delta from anchor → dB delta scaled down 5x
    double pxPerDb = (double)(sliderR - sliderL) / (MAX_DB - MIN_DB);
    double deltaDb = (double)(x - m_dragAnchorX) / (pxPerDb * 5.0);
    return std::max(MIN_DB, std::min(MAX_DB, m_dragAnchorDb + deltaDb));
  }
  double ratio = (double)(x - sliderL) / (double)(sliderR - sliderL);
  ratio = std::max(0.0, std::min(1.0, ratio));
  return MIN_DB + ratio * (MAX_DB - MIN_DB);
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

  // Slider area
  if (SliderHitTest(x, y, waveformRect)) {
    m_sliderDragging = true;
    m_dragAnchorX = x;
    m_dragAnchorDb = m_db;
    if (!IsFineMode()) {
      m_db = SliderXToDb(x, r);
      WriteToItem();
    }
    return true;
  }

  // Otherwise: start panel drag
  m_panelDragging = true;
  m_dragOffsetX = x - r.left;
  m_dragOffsetY = y - r.top;
  return true;
}

void GainPanel::OnMouseMove(int x, int y, RECT waveformRect)
{
  if (m_sliderDragging) {
    RECT r = GetRect(waveformRect);
    m_db = SliderXToDb(x, r);
    WriteToItem();
  }
  else if (m_panelDragging) {
    // Calculate new offset so panel follows mouse
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
  m_sliderDragging = false;
  m_panelDragging = false;
}

void GainPanel::Draw(HDC hdc, RECT waveformRect)
{
  if (!m_visible) return;

  // Re-read from item in case it changed externally
  if (m_item && !m_sliderDragging) ReadFromItem();

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
  HFONT font = CreateFont(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
  HFONT oldFont = (HFONT)SelectObject(hdc, font);

  // Volume icon (drag handle area)
  SetTextColor(hdc, RGB(180, 180, 180));
  RECT iconRect = { r.left + 4, r.top + 2, r.left + 28, r.bottom - 2 };
  DrawText(hdc, "Vol", -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Slider track
  int sliderL = r.left + 32;
  int sliderR = r.right - 54;
  int sliderY = (r.top + r.bottom) / 2;

  HPEN trackPen = CreatePen(PS_SOLID, 2, RGB(70, 70, 70));
  oldPen = (HPEN)SelectObject(hdc, trackPen);
  MoveToEx(hdc, sliderL, sliderY, nullptr);
  LineTo(hdc, sliderR, sliderY);
  SelectObject(hdc, oldPen);
  DeleteObject(trackPen);

  // Slider position
  double range = MAX_DB - MIN_DB;
  double ratio = (m_db - MIN_DB) / range;
  ratio = std::max(0.0, std::min(1.0, ratio));
  int thumbX = sliderL + (int)(ratio * (double)(sliderR - sliderL));

  // Center tick (0 dB)
  double zeroRatio = (0.0 - MIN_DB) / range;
  int zeroX = sliderL + (int)(zeroRatio * (double)(sliderR - sliderL));
  HPEN zeroPen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
  oldPen = (HPEN)SelectObject(hdc, zeroPen);
  MoveToEx(hdc, zeroX, sliderY - 5, nullptr);
  LineTo(hdc, zeroX, sliderY + 6);
  SelectObject(hdc, oldPen);
  DeleteObject(zeroPen);

  // Active track (colored from 0dB to thumb)
  COLORREF activeColor = (m_db >= 0) ? RGB(220, 80, 60) : RGB(60, 180, 120);
  HPEN activePen = CreatePen(PS_SOLID, 3, activeColor);
  oldPen = (HPEN)SelectObject(hdc, activePen);
  MoveToEx(hdc, zeroX, sliderY, nullptr);
  LineTo(hdc, thumbX, sliderY);
  SelectObject(hdc, oldPen);
  DeleteObject(activePen);

  // Thumb knob
  HBRUSH thumbBrush = CreateSolidBrush(RGB(220, 220, 220));
  RECT thumbRect = { thumbX - 4, sliderY - 8, thumbX + 4, sliderY + 8 };
  FillRect(hdc, &thumbRect, thumbBrush);
  DeleteObject(thumbBrush);

  // dB readout
  char dbText[16];
  if (m_db <= MIN_DB + 0.5) {
    snprintf(dbText, sizeof(dbText), "-inf");
  } else {
    snprintf(dbText, sizeof(dbText), "%+.1f dB", m_db);
  }
  SetTextColor(hdc, RGB(100, 200, 255));
  RECT dbRect = { r.right - 52, r.top + 2, r.right - 2, r.bottom - 2 };
  DrawText(hdc, dbText, -1, &dbRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Close "x" button
  SetTextColor(hdc, RGB(140, 140, 140));
  RECT xRect = { r.right - 14, r.top + 1, r.right - 1, r.top + 14 };
  DrawText(hdc, "x", -1, &xRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectObject(hdc, oldFont);
  DeleteObject(font);
}
