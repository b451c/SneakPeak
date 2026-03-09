// marker_manager.cpp — Marker/region drawing, hit-testing, and editing
#include "marker_manager.h"
#include "globals.h"
#include "waveform_view.h"
#include "theme.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

void MarkerManager::DrawMarkers(HDC hdc, const RECT& waveformRect, const RECT& rulerRect, const WaveformView& wv)
{
  if (!wv.HasItem()) return;
  if (!g_EnumProjectMarkers3) return;

  double itemPos = wv.GetItemPosition();
  double itemDur = wv.GetItemDuration();

  SetBkMode(hdc, TRANSPARENT);

  HFONT font = CreateFont(11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
  HFONT oldFont = (HFONT)SelectObject(hdc, font);

  int idx = 0;
  bool isrgn;
  double pos, rgnend;
  const char* name;
  int markrgnidx;
  int color;

  while (g_EnumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend, &name, &markrgnidx, &color)) {
    idx++;

    // Draw regions as shaded areas
    if (isrgn) {
      double localStart = pos - itemPos;
      double localEnd = rgnend - itemPos;
      if (localEnd < 0.0 || localStart > itemDur) continue;
      localStart = std::max(0.0, localStart);
      localEnd = std::min(itemDur, localEnd);

      int x1 = wv.TimeToX(localStart);
      int x2 = wv.TimeToX(localEnd);
      if (x2 <= x1) continue;

      COLORREF rgnColor = (color != 0) ? (COLORREF)(color & 0x00FFFFFF) : RGB(100, 100, 100);
      // Dim region color for overlay effect
      COLORREF dimColor = ColorBlend(RGB(0, 0, 0), rgnColor, 0.25f);

      // Draw region edges
      HPEN rgnPen = CreatePen(PS_SOLID, 1, rgnColor);
      HPEN op = (HPEN)SelectObject(hdc, rgnPen);
      MoveToEx(hdc, x1, rulerRect.top, nullptr);
      LineTo(hdc, x1, waveformRect.bottom);
      MoveToEx(hdc, x2, rulerRect.top, nullptr);
      LineTo(hdc, x2, waveformRect.bottom);
      SelectObject(hdc, op);
      DeleteObject(rgnPen);

      // Region name at top
      if (name && name[0]) {
        // Small colored tab background
        int textW = std::min(x2 - x1, 120);
        RECT tabRect = { x1, rulerRect.top, x1 + textW, rulerRect.top + 14 };
        HBRUSH tabBrush = CreateSolidBrush(dimColor);
        FillRect(hdc, &tabRect, tabBrush);
        DeleteObject(tabBrush);

        SetTextColor(hdc, rgnColor);
        RECT nameRect = { x1 + 3, rulerRect.top + 1, x1 + textW - 2, rulerRect.top + 13 };
        DrawText(hdc, name, -1, &nameRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
      }
      continue;
    }

    // Point marker
    double localT = pos - itemPos;
    if (localT < 0.0 || localT > itemDur) continue;

    int mx = wv.TimeToX(localT);
    if (mx < waveformRect.left || mx >= waveformRect.right) continue;

    // Default color: bright yellow for visibility on dark bg
    COLORREF lineColor = (color != 0) ? (COLORREF)(color & 0x00FFFFFF) : RGB(220, 200, 60);

    // Vertical line — 2px, bright, full height
    HPEN markerPen = CreatePen(PS_SOLID, 2, lineColor);
    HPEN op = (HPEN)SelectObject(hdc, markerPen);
    MoveToEx(hdc, mx, rulerRect.top, nullptr);
    LineTo(hdc, mx, waveformRect.bottom);
    SelectObject(hdc, op);
    DeleteObject(markerPen);

    // Name tab on waveform area (just below ruler, with dark bg for readability)
    const char* label = (name && name[0]) ? name : "";
    char idLabel[32];
    if (!label[0]) { snprintf(idLabel, sizeof(idLabel), "M%d", markrgnidx); label = idLabel; }

    // Dark background tab
    RECT tabRect = { mx + 1, waveformRect.top, mx + 70, waveformRect.top + 15 };
    HBRUSH tabBrush = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hdc, &tabRect, tabBrush);
    DeleteObject(tabBrush);

    SetTextColor(hdc, lineColor);
    RECT nameRect = { mx + 4, waveformRect.top + 1, mx + 68, waveformRect.top + 14 };
    DrawText(hdc, label, -1, &nameRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
  }

  SelectObject(hdc, oldFont);
  DeleteObject(font);
}

int MarkerManager::HitTestMarker(int x, const WaveformView& wv, int tolerance) const
{
  if (!wv.HasItem() || !g_EnumProjectMarkers3) return -1;

  double itemPos = wv.GetItemPosition();
  double itemDur = wv.GetItemDuration();

  int idx = 0;
  bool isrgn;
  double pos, rgnend;
  const char* name;
  int markrgnidx, color;

  while (g_EnumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend, &name, &markrgnidx, &color)) {
    double localT = pos - itemPos;
    if (localT >= 0.0 && localT <= itemDur) {
      int mx = wv.TimeToX(localT);
      if (abs(x - mx) <= tolerance) return idx;
    }
    idx++;
  }
  return -1;
}

void MarkerManager::AddMarkerAtCursor(const WaveformView& wv)
{
  if (!wv.HasItem() || !g_AddProjectMarker2) return;
  double absTime = wv.GetItemPosition() + wv.GetCursorTime();
  g_AddProjectMarker2(nullptr, false, absTime, 0.0, "", -1, 0);
  if (g_UpdateTimeline) g_UpdateTimeline();
}

void MarkerManager::AddRegionFromSelection(const WaveformView& wv)
{
  if (!wv.HasItem() || !wv.HasSelection() || !g_AddProjectMarker2) return;
  WaveformSelection sel = wv.GetSelection();
  double itemPos = wv.GetItemPosition();
  double absStart = itemPos + sel.startTime;
  double absEnd = itemPos + sel.endTime;
  g_AddProjectMarker2(nullptr, true, absStart, absEnd, "", -1, 0);
  if (g_UpdateTimeline) g_UpdateTimeline();
}

void MarkerManager::DeleteMarkerByEnumIdx(int enumIdx)
{
  if (!g_DeleteProjectMarkerByIndex) return;
  g_DeleteProjectMarkerByIndex(nullptr, enumIdx);
  if (g_UpdateTimeline) g_UpdateTimeline();
}

void MarkerManager::EditMarkerDialog(int enumIdx)
{
  if (!g_EnumProjectMarkers3 || !g_SetProjectMarkerByIndex2) return;

  bool isrgn;
  double pos, rgnend;
  const char* name;
  int markrgnidx, color;
  if (!g_EnumProjectMarkers3(nullptr, enumIdx, &isrgn, &pos, &rgnend, &name, &markrgnidx, &color))
    return;

  if (g_GetUserInputs) {
    // Use REAPER's built-in input dialog: "Name,Color (R,G,B)"
    char retvals[512];
    snprintf(retvals, sizeof(retvals), "%s,%d,%d,%d",
             name ? name : "",
             (color != 0) ? GetRValue(color & 0xFFFFFF) : 160,
             (color != 0) ? GetGValue(color & 0xFFFFFF) : 160,
             (color != 0) ? GetBValue(color & 0xFFFFFF) : 160);

    const char* title = isrgn ? "Edit Region" : "Edit Marker";
    if (g_GetUserInputs(title, 4, "Name,Red (0-255),Green (0-255),Blue (0-255)", retvals, sizeof(retvals))) {
      // Parse comma-separated results
      char newName[256] = {};
      int r = 160, g = 160, b = 160;
      char* p = retvals;
      char* comma = strchr(p, ',');
      if (comma) {
        *comma = '\0';
        safe_strncpy(newName, p, sizeof(newName));
        p = comma + 1;
        sscanf(p, "%d,%d,%d", &r, &g, &b);
      } else {
        safe_strncpy(newName, retvals, sizeof(newName));
      }
      r = std::max(0, std::min(255, r));
      g = std::max(0, std::min(255, g));
      b = std::max(0, std::min(255, b));
      int newColor = RGB(r, g, b) | 0x1000000; // REAPER color format

      g_SetProjectMarkerByIndex2(nullptr, enumIdx, isrgn, pos, rgnend, markrgnidx, newName, newColor, 0);
      if (g_UpdateTimeline) g_UpdateTimeline();
      if (g_UpdateArrange) g_UpdateArrange();
    }
  }
}

void MarkerManager::StartDrag(int enumIdx)
{
  m_markerDragging = true;
  m_dragMarkerEnumIdx = enumIdx;
}

void MarkerManager::UpdateDrag(int x, const WaveformView& wv)
{
  if (!m_markerDragging || !wv.HasItem() || !g_SetProjectMarkerByIndex2 || !g_EnumProjectMarkers3)
    return;

  bool isrgn;
  double pos, rgnend;
  const char* name;
  int markrgnidx, color;
  if (g_EnumProjectMarkers3(nullptr, m_dragMarkerEnumIdx, &isrgn, &pos, &rgnend, &name, &markrgnidx, &color)) {
    double newLocalTime = wv.XToTime(x);
    double newPos = wv.GetItemPosition() + newLocalTime;
    newPos = std::max(0.0, newPos);
    double newRgnEnd = isrgn ? (rgnend + (newPos - pos)) : rgnend;
    g_SetProjectMarkerByIndex2(nullptr, m_dragMarkerEnumIdx, isrgn, newPos, newRgnEnd, markrgnidx, name, color, 0);
  }
}

void MarkerManager::EndDrag()
{
  m_markerDragging = false;
  m_dragMarkerEnumIdx = -1;
  if (g_UpdateTimeline) g_UpdateTimeline();
  if (g_UpdateArrange) g_UpdateArrange();
}
