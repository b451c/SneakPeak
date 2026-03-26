// ============================================================================
// rendering.cpp — Paint and drawing routines for SneakPeak
//
// OnPaint, DrawModeBar, DrawRuler, DrawScrollbar, DrawSoloButton,
// DrawMasterWaveform, DrawBottomPanel, DrawToast, DrawSplitter.
// Also: solo button hit-test, track solo toggle, solo state polling.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "theme.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

void SneakPeak::OnPaint(HDC hdc)
{
  if (!hdc) return;

  DrawModeBar(hdc);
  DrawRuler(hdc);
  if (m_masterMode) {
    DrawMasterWaveform(hdc);
  } else {
    m_waveform.Paint(hdc);
  }
  if (m_markers.GetShowMarkers()) m_markers.DrawMarkers(hdc, m_waveformRect, m_rulerRect, m_waveform);
  if (m_waveform.HasItem()) m_gainPanel.Draw(hdc, m_waveformRect);
  if (m_waveform.HasItem()) DrawSoloButton(hdc);
  if (m_spectralVisible) {
    DrawSplitter(hdc);
    m_spectral.Paint(hdc, m_waveform);
  }
  if (m_minimapVisible) m_minimap.Paint(hdc, m_waveform);
  DrawScrollbar(hdc);
  DrawBottomPanel(hdc);
  DrawToast(hdc);
}

void SneakPeak::DrawSplitter(HDC hdc)
{
  if (m_splitterRect.bottom <= m_splitterRect.top) return;

  HBRUSH bg = CreateSolidBrush(RGB(45, 45, 45));
  FillRect(hdc, &m_splitterRect, bg);
  DeleteObject(bg);

  // Grip dots in center
  int cx = (m_splitterRect.left + m_splitterRect.right) / 2;
  int cy = (m_splitterRect.top + m_splitterRect.bottom) / 2;
  HBRUSH dot = CreateSolidBrush(RGB(120, 120, 120));
  for (int dx = -12; dx <= 12; dx += 6) {
    RECT d = { cx + dx - 1, cy - 1, cx + dx + 2, cy + 2 };
    FillRect(hdc, &d, dot);
  }
  DeleteObject(dot);
}

void SneakPeak::DrawModeBar(HDC hdc)
{
  int w = m_modeBarRect.right - m_modeBarRect.left;
  int h = m_modeBarRect.bottom - m_modeBarRect.top;
  if (w <= 0 || h <= 0) return;

  // Background
  HBRUSH bgBrush = CreateSolidBrush(g_theme.modeBarBg);
  FillRect(hdc, &m_modeBarRect, bgBrush);
  DeleteObject(bgBrush);

  // Bottom border
  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_modeBarRect.left, m_modeBarRect.bottom - 1, nullptr);
  LineTo(hdc, m_modeBarRect.right, m_modeBarRect.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(hdc, TRANSPARENT);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);

  m_modeBarTabs.clear();
  int xPos = 6;
  int yMid = m_modeBarRect.top + h / 2;

  bool isStandalone = m_waveform.IsStandaloneMode() || !m_standaloneFiles.empty();
  bool isReaper = m_waveform.HasItem() && !m_waveform.IsStandaloneMode();
  bool isEmpty = !m_waveform.HasItem() && m_standaloneFiles.empty();

  // MASTER tab — always available (on the right side, drawn later)
  // We'll draw it after the main content

  if (isEmpty && !m_masterMode) {
    // Empty state
    SetTextColor(hdc, g_theme.emptyText);
    RECT textR = { xPos, m_modeBarRect.top, m_modeBarRect.right - 80, m_modeBarRect.bottom };
    DrawText(hdc, "SneakPeak - Drop audio file or select item", -1, &textR,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (isEmpty && m_masterMode) {
    // Master mode active — show indicator
    COLORREF accent = RGB(200, 80, 80);
    HBRUSH accentBrush = CreateSolidBrush(accent);
    RECT dot = { xPos, yMid - 3, xPos + 7, yMid + 4 };
    FillRect(hdc, &dot, accentBrush);
    DeleteObject(accentBrush);
    xPos += 12;
    SetTextColor(hdc, accent);
    RECT labelR = { xPos, m_modeBarRect.top, xPos + 80, m_modeBarRect.bottom };
    DrawText(hdc, "MASTER", -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else {
    // Mode indicator
    COLORREF accent;
    const char* modeLabel;
    if (m_trackViewMode && isReaper) {
      accent = RGB(80, 200, 100);  // green for track view
      modeLabel = "TRACK";
    } else if (isStandalone && !isReaper) {
      accent = g_theme.modeBarStandaloneAccent;
      modeLabel = "STANDALONE";
    } else {
      accent = g_theme.modeBarReaperAccent;
      modeLabel = "REAPER";
    }

    // Draw indicator dot/diamond
    HBRUSH accentBrush = CreateSolidBrush(accent);
    if (isStandalone && !isReaper) {
      // Orange filled circle (small rounded rect)
      RECT dot = { xPos, yMid - 3, xPos + 7, yMid + 4 };
      FillRect(hdc, &dot, accentBrush);
    } else {
      // Blue diamond — draw as small rotated square using lines
      int cx = xPos + 3, cy = yMid;
      POINT diamond[4] = { {cx, cy-4}, {cx+4, cy}, {cx, cy+4}, {cx-4, cy} };
      HPEN acPen = CreatePen(PS_SOLID, 1, accent);
      HPEN prevPen = (HPEN)SelectObject(hdc, acPen);
      HBRUSH prevBr = (HBRUSH)SelectObject(hdc, accentBrush);
      Polygon(hdc, diamond, 4);
      SelectObject(hdc, prevPen);
      SelectObject(hdc, prevBr);
      DeleteObject(acPen);
    }
    DeleteObject(accentBrush);
    xPos += 12;

    // Mode label
    SetTextColor(hdc, accent);
    RECT labelR = { xPos, m_modeBarRect.top, xPos + 80, m_modeBarRect.bottom };
    DrawText(hdc, modeLabel, -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT labelMeasure = { 0, 0, 200, 20 };
    DrawText(hdc, modeLabel, -1, &labelMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    xPos += (labelMeasure.right - labelMeasure.left) + 8;

    // Separator
    HPEN sepPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HPEN sPrev = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, xPos, m_modeBarRect.top + 3, nullptr);
    LineTo(hdc, xPos, m_modeBarRect.bottom - 3);
    SelectObject(hdc, sPrev);
    DeleteObject(sepPen);
    xPos += 8;

    // Switch to normal weight for tabs
    oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

    int tabAreaRight = m_modeBarRect.right - 8;

    // REAPER pseudo-tab (if we have standalone files and we're in REAPER mode, or for switching back)
    if (isReaper && !m_standaloneFiles.empty()) {
      // Active REAPER tab
      char reaperLabel[256] = "REAPER item";
      if (m_waveform.HasItem()) {
        const char* fp = m_waveform.GetStandaloneFilePath().c_str();
        if (fp[0]) {
          snprintf(reaperLabel, sizeof(reaperLabel), "%s", FileNameFromPath(fp));
        } else {
          // Get item name from waveform
          char buf[256];
          GetItemTitle(buf, sizeof(buf));
          if (buf[0]) snprintf(reaperLabel, sizeof(reaperLabel), "%s", buf);
        }
      }
      RECT tsR = { 0, 0, 300, 20 };
      DrawText(hdc, reaperLabel, -1, &tsR, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      int tw = std::min((int)(tsR.right - tsR.left) + 12, MODE_TAB_MAX_W);

      RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
      HBRUSH tabBg = CreateSolidBrush(g_theme.modeBarActiveTab);
      FillRect(hdc, &tabR, tabBg);
      DeleteObject(tabBg);

      // Blue accent underline
      HPEN ulPen = CreatePen(PS_SOLID, 2, g_theme.modeBarReaperAccent);
      HPEN ulPrev = (HPEN)SelectObject(hdc, ulPen);
      MoveToEx(hdc, tabR.left, tabR.bottom - 1, nullptr);
      LineTo(hdc, tabR.right, tabR.bottom - 1);
      SelectObject(hdc, ulPrev);
      DeleteObject(ulPen);

      SetTextColor(hdc, RGB(220, 220, 220));
      RECT textR = { tabR.left + 6, tabR.top, tabR.right - 6, tabR.bottom };
      DrawText(hdc, reaperLabel, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

      ModeBarTab mbt;
      mbt.rect = tabR;
      mbt.closeRect = {};
      mbt.fileIdx = -1;
      mbt.isReaper = true;
      m_modeBarTabs.push_back(mbt);
      xPos = tabR.right + 2;
    }

    // Standalone file tabs
    for (int i = 0; i < (int)m_standaloneFiles.size() && xPos < tabAreaRight; i++) {
      const auto& fs = m_standaloneFiles[i];
      const char* fname = FileNameFromPath(fs.filePath.c_str());
      bool isDirty = (m_waveform.IsStandaloneMode() && i == m_activeFileIdx) ? m_dirty : fs.dirty;
      char label[160];
      if (isDirty)
        snprintf(label, sizeof(label), "*%s", fname);
      else
        snprintf(label, sizeof(label), "%s", fname);

      RECT tsR2 = { 0, 0, 300, 20 };
      DrawText(hdc, label, -1, &tsR2, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      bool isActive = (m_waveform.IsStandaloneMode() && i == m_activeFileIdx);
      int closeW = isActive ? MODE_TAB_CLOSE_SIZE : 0;
      int tw = std::min((int)(tsR2.right - tsR2.left) + 12 + closeW, MODE_TAB_MAX_W);
      if (xPos + tw > tabAreaRight) tw = tabAreaRight - xPos;
      if (tw < 20) break;

      RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
      HBRUSH tabBg = CreateSolidBrush(isActive ? g_theme.modeBarActiveTab : g_theme.modeBarInactiveTab);
      FillRect(hdc, &tabR, tabBg);
      DeleteObject(tabBg);

      if (isActive) {
        // Orange accent underline for active standalone tab
        HPEN ulPen = CreatePen(PS_SOLID, 2, g_theme.modeBarStandaloneAccent);
        HPEN ulPrev = (HPEN)SelectObject(hdc, ulPen);
        MoveToEx(hdc, tabR.left, tabR.bottom - 1, nullptr);
        LineTo(hdc, tabR.right, tabR.bottom - 1);
        SelectObject(hdc, ulPrev);
        DeleteObject(ulPen);
      }

      SetTextColor(hdc, isActive ? RGB(220, 220, 220) : g_theme.modeBarText);
      int textRight = tabR.right - 4 - closeW;
      RECT textR = { tabR.left + 6, tabR.top, textRight, tabR.bottom };
      DrawText(hdc, label, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

      // Close button on active tab
      RECT closeR = {};
      if (isActive) {
        int csz = MODE_TAB_CLOSE_SIZE - 4;
        int cx = tabR.right - MODE_TAB_CLOSE_SIZE + 1;
        int cy = yMid - csz / 2;
        closeR = { cx, cy, cx + csz, cy + csz };
        SetTextColor(hdc, g_theme.modeBarText);
        DrawText(hdc, "x", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }

      ModeBarTab mbt;
      mbt.rect = tabR;
      mbt.closeRect = closeR;
      mbt.fileIdx = i;
      mbt.isReaper = false;
      m_modeBarTabs.push_back(mbt);
      xPos = tabR.right + 2;
    }

    // If in standalone mode with no REAPER item showing, but we had a REAPER item before,
    // show a "REAPER" pseudo-tab for switching back
    if (!isReaper && !m_standaloneFiles.empty()) {
      // Check if there's a REAPER item available
      if (g_CountSelectedMediaItems && g_CountSelectedMediaItems(nullptr) > 0) {
        const char* rl = "REAPER";
        RECT tsR3 = { 0, 0, 300, 20 };
        DrawText(hdc, rl, -1, &tsR3, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int tw = std::min((int)(tsR3.right - tsR3.left) + 12, MODE_TAB_MAX_W);
        if (xPos + tw <= tabAreaRight) {
          RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
          HBRUSH tabBg = CreateSolidBrush(g_theme.modeBarInactiveTab);
          FillRect(hdc, &tabR, tabBg);
          DeleteObject(tabBg);

          SetTextColor(hdc, g_theme.modeBarReaperAccent);
          RECT textR = { tabR.left + 6, tabR.top, tabR.right - 4, tabR.bottom };
          DrawText(hdc, rl, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

          ModeBarTab mbt;
          mbt.rect = tabR;
          mbt.closeRect = {};
          mbt.fileIdx = -1;
          mbt.isReaper = true;
          m_modeBarTabs.push_back(mbt);
        }
      }
    }
  }

  // MASTER tab — right-aligned, always visible
  {
    SelectObject(hdc, g_fonts.normal11);
    const char* ml = "MASTER";
    RECT tsM = { 0, 0, 200, 20 };
    DrawText(hdc, ml, -1, &tsM, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int tw = (int)(tsM.right - tsM.left) + 14;
    int tabRight = m_modeBarRect.right - 6;
    int tabLeft = tabRight - tw;

    RECT tabR = { tabLeft, m_modeBarRect.top + 1, tabRight, m_modeBarRect.bottom - 1 };
    HBRUSH tabBg = CreateSolidBrush(m_masterMode ? g_theme.modeBarActiveTab : g_theme.modeBarInactiveTab);
    FillRect(hdc, &tabR, tabBg);
    DeleteObject(tabBg);

    if (m_masterMode) {
      HPEN ulPen = CreatePen(PS_SOLID, 2, RGB(200, 80, 80));
      HPEN ulPrev = (HPEN)SelectObject(hdc, ulPen);
      MoveToEx(hdc, tabR.left, tabR.bottom - 1, nullptr);
      LineTo(hdc, tabR.right, tabR.bottom - 1);
      SelectObject(hdc, ulPrev);
      DeleteObject(ulPen);
    }

    SetTextColor(hdc, m_masterMode ? RGB(220, 220, 220) : RGB(200, 80, 80));
    RECT textR = { tabR.left + 7, tabR.top, tabR.right - 7, tabR.bottom };
    DrawText(hdc, ml, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ModeBarTab mbt;
    mbt.rect = tabR;
    mbt.closeRect = {};
    mbt.fileIdx = -2; // special: MASTER tab
    mbt.isReaper = false;
    m_modeBarTabs.push_back(mbt);
  }

  SelectObject(hdc, oldFont);
}

void SneakPeak::DrawRuler(HDC hdc)
{
  int w = m_rulerRect.right - m_rulerRect.left;
  int h = m_rulerRect.bottom - m_rulerRect.top;

  HBRUSH bg = CreateSolidBrush(g_theme.rulerBg);
  FillRect(hdc, &m_rulerRect, bg);
  DeleteObject(bg);

  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_rulerRect.left, m_rulerRect.bottom - 1, nullptr);
  LineTo(hdc, m_rulerRect.right, m_rulerRect.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  if (!m_waveform.HasItem()) return;

  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();
  if (viewDur <= 0) return;

  double pixelsPerSec = (double)w / viewDur;

  static const double intervals[] = {
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
    1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0
  };
  double tickInterval = 300.0;
  for (double iv : intervals) {
    if (iv * pixelsPerSec >= 80.0) { tickInterval = iv; break; }
  }

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, g_theme.rulerText);

  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

  HPEN tickPen = CreatePen(PS_SOLID, 1, g_theme.rulerTick);
  HPEN minorPen = CreatePen(PS_SOLID, 1, g_theme.rulerTickMinor);

  double firstTick = floor(viewStart / tickInterval) * tickInterval;
  int y = m_rulerRect.top;

  for (double t = firstTick; t < viewStart + viewDur; t += tickInterval) {
    if (t < 0) continue;
    int tx = m_waveform.TimeToX(t);
    if (tx < m_rulerRect.left || tx >= m_rulerRect.right) continue;

    HPEN op = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, tx, y + h - 8, nullptr);
    LineTo(hdc, tx, y + h - 1);
    SelectObject(hdc, op);

    // Format as HH:MM:SS;ms
    char label[32];
    {
      int totalSec = (int)t;
      int hours = totalSec / 3600;
      int mins = (totalSec % 3600) / 60;
      int secs = totalSec % 60;
      int ms = (int)((t - totalSec) * 1000.0 + 0.5);
      if (ms >= 1000) { ms -= 1000; secs++; }

      if (tickInterval >= 60.0) {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;00", hours, mins, secs);
      } else if (tickInterval >= 1.0) {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;00", hours, mins, secs);
      } else if (tickInterval >= 0.01) {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;%02d", hours, mins, secs, ms / 10);
      } else {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;%03d", hours, mins, secs, ms);
      }
    }
    RECT textRect = { tx + 3, y + 2, tx + 80, y + h - 8 };
    DrawText(hdc, label, -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    double minorIv = tickInterval / 5.0;
    for (int mi = 1; mi < 5; mi++) {
      int mx = m_waveform.TimeToX(t + mi * minorIv);
      if (mx >= m_rulerRect.left && mx < m_rulerRect.right) {
        HPEN op2 = (HPEN)SelectObject(hdc, minorPen);
        MoveToEx(hdc, mx, y + h - 4, nullptr);
        LineTo(hdc, mx, y + h - 1);
        SelectObject(hdc, op2);
      }
    }
  }

  DeleteObject(tickPen);
  DeleteObject(minorPen);
  SelectObject(hdc, oldFont);
}

void SneakPeak::DrawScrollbar(HDC hdc)
{
  HBRUSH bg = CreateSolidBrush(g_theme.scrollbarBg);
  FillRect(hdc, &m_scrollbarRect, bg);
  DeleteObject(bg);

  if (!m_waveform.HasItem() || m_waveform.GetItemDuration() <= 0) return;

  int sw = m_scrollbarRect.right - m_scrollbarRect.left;
  double totalDur = m_waveform.GetItemDuration();
  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();

  double startRatio = std::max(0.0, viewStart / totalDur);
  double endRatio = std::min(1.0, (viewStart + viewDur) / totalDur);

  int thumbX = m_scrollbarRect.left + (int)(startRatio * (double)sw);
  int thumbW = std::max(20, (int)((endRatio - startRatio) * (double)sw));

  COLORREF thumbColor = m_scrollbarDragging ? g_theme.scrollbarHover : g_theme.scrollbarThumb;
  RECT thumbRect = { thumbX, m_scrollbarRect.top + 2, thumbX + thumbW, m_scrollbarRect.bottom - 2 };
  HBRUSH thumbBrush = CreateSolidBrush(thumbColor);
  FillRect(hdc, &thumbRect, thumbBrush);
  DeleteObject(thumbBrush);
}

static void FormatTimeHMS(double sec, char* buf, int sz)
{
  if (sec < 0) sec = 0;
  int totalMs = static_cast<int>(sec * 1000.0 + 0.5);
  int ms = totalMs % 1000;
  int totalSec = totalMs / 1000;
  int s = totalSec % 60;
  int m = (totalSec / 60) % 60;
  int h = totalSec / 3600;
  snprintf(buf, sz, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

// --- Solo button ---

void SneakPeak::DrawSoloButton(HDC hdc)
{
  if (!m_waveform.HasItem()) return;

  // Position: top-right of waveform area, well left of dB scale and fade handles
  int btnW = 22, btnH = 16;
  int btnX = m_waveformRect.right - DB_SCALE_WIDTH - btnW - 30;
  int btnY = m_waveformRect.top + 10;
  m_soloBtnRect = { btnX, btnY, btnX + btnW, btnY + btnH };

  // Background
  COLORREF bgCol = m_trackSoloed ? RGB(200, 180, 0) : RGB(50, 50, 50);
  HBRUSH bg = CreateSolidBrush(bgCol);
  FillRect(hdc, &m_soloBtnRect, bg);
  DeleteObject(bg);

  // Border
  COLORREF borderCol = m_trackSoloed ? RGB(240, 220, 0) : RGB(80, 80, 80);
  HPEN pen = CreatePen(PS_SOLID, 1, borderCol);
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, m_soloBtnRect.left, m_soloBtnRect.top, nullptr);
  LineTo(hdc, m_soloBtnRect.right - 1, m_soloBtnRect.top);
  LineTo(hdc, m_soloBtnRect.right - 1, m_soloBtnRect.bottom - 1);
  LineTo(hdc, m_soloBtnRect.left, m_soloBtnRect.bottom - 1);
  LineTo(hdc, m_soloBtnRect.left, m_soloBtnRect.top);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Label "S"
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, m_trackSoloed ? RGB(0, 0, 0) : RGB(140, 140, 140));
  DrawText(hdc, "S", 1, &m_soloBtnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(hdc, oldFont);
}

bool SneakPeak::ClickSoloButton(int x, int y)
{
  if (!m_waveform.HasItem()) return false;
  return x >= m_soloBtnRect.left && x < m_soloBtnRect.right &&
         y >= m_soloBtnRect.top && y < m_soloBtnRect.bottom;
}

void SneakPeak::ToggleTrackSolo()
{
  if (!g_GetMediaItem_Track || !g_GetSetMediaTrackInfo) return;

  // Collect unique tracks from all segments (multi-item cross-track support)
  std::vector<MediaTrack*> tracks;
  const auto& segs = m_waveform.GetSegments();
  if (segs.size() > 1) {
    for (const auto& seg : segs) {
      if (!seg.item) continue;
      MediaTrack* tr = g_GetMediaItem_Track(seg.item);
      if (!tr) continue;
      bool found = false;
      for (auto* t : tracks) { if (t == tr) { found = true; break; } }
      if (!found) tracks.push_back(tr);
    }
  } else {
    MediaItem* item = m_waveform.GetItem();
    if (!item) return;
    MediaTrack* tr = g_GetMediaItem_Track(item);
    if (tr) tracks.push_back(tr);
  }
  if (tracks.empty()) return;

  // Check if any track is currently soloed — toggle all together
  bool anySoloed = false;
  for (auto* tr : tracks) {
    int* pSolo = (int*)g_GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
    if (pSolo && *pSolo != 0) { anySoloed = true; break; }
  }

  int newSolo = anySoloed ? 0 : 2;  // 2 = solo-in-place (SIP)
  for (auto* tr : tracks)
    g_GetSetMediaTrackInfo(tr, "I_SOLO", &newSolo);

  m_trackSoloed = (newSolo != 0);
  if (g_UpdateArrange) g_UpdateArrange();
}

void SneakPeak::UpdateSoloState()
{
  if (!g_GetMediaItem_Track || !g_GetSetMediaTrackInfo || !m_waveform.HasItem()) {
    m_trackSoloed = false;
    return;
  }

  // Collect unique tracks from all segments
  std::vector<MediaTrack*> tracks;
  const auto& segs = m_waveform.GetSegments();
  if (segs.size() > 1) {
    for (const auto& seg : segs) {
      if (!seg.item) continue;
      if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)seg.item, "MediaItem*")) continue;
      MediaTrack* tr = g_GetMediaItem_Track(seg.item);
      if (!tr) continue;
      bool found = false;
      for (auto* t : tracks) { if (t == tr) { found = true; break; } }
      if (!found) tracks.push_back(tr);
    }
  } else {
    MediaItem* item = m_waveform.GetItem();
    if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)item, "MediaItem*")) {
      m_trackSoloed = false;
      return;
    }
    MediaTrack* tr = g_GetMediaItem_Track(item);
    if (tr) tracks.push_back(tr);
  }

  // Soloed if any of our tracks is soloed
  m_trackSoloed = false;
  for (auto* tr : tracks) {
    int* pSolo = (int*)g_GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
    if (pSolo && *pSolo != 0) { m_trackSoloed = true; break; }
  }
}

void SneakPeak::DrawMasterWaveform(HDC hdc)
{
  RECT r = m_waveformRect;
  int scaleLeft = r.right - DB_SCALE_WIDTH;
  int waveRight = scaleLeft; // waveform ends before dB scale
  int w = waveRight - r.left;
  int h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return;

  // Dark background (full rect including scale area)
  HBRUSH bgBrush = CreateSolidBrush(g_theme.waveformBg);
  FillRect(hdc, &r, bgBrush);
  DeleteObject(bgBrush);

  int centerY = r.top + h / 2;
  float halfH = (float)(h / 2) * 0.9f; // 90% of half-height

  // Center line
  HPEN centerPen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
  HPEN oldPen = (HPEN)SelectObject(hdc, centerPen);
  MoveToEx(hdc, r.left, centerY, nullptr);
  LineTo(hdc, waveRight, centerY);
  SelectObject(hdc, oldPen);
  DeleteObject(centerPen);

  if (m_masterPeakCount == 0) {
    // No data yet — show label
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_theme.emptyText);
    RECT textRect = r;
    DrawText(hdc, "Master Output — play to see waveform", -1, &textRect,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    return;
  }

  // Draw rolling waveform — most recent samples on the right
  int count = m_masterPeakCount;
  int columnsToShow = std::min(w, count);

  // L channel (top half) — green, red above 0dB
  HPEN greenPenL = CreatePen(PS_SOLID, 1, RGB(40, 160, 60));
  HPEN redPen = CreatePen(PS_SOLID, 1, RGB(220, 50, 50));
  int clipY0dBTop = centerY - (int)(1.0f * halfH);
  int clipY0dBBot = centerY + (int)(1.0f * halfH);
  HPEN prevPen;

  for (int col = 0; col < columnsToShow; col++) {
    int bufIdx = (m_masterPeakHead - columnsToShow + col + MASTER_ROLLING_SIZE) % MASTER_ROLLING_SIZE;
    float pkL = m_masterPeakBufL[bufIdx];

    int x = waveRight - columnsToShow + col;
    int yL = centerY - (int)(pkL * halfH);
    if (yL < r.top) yL = r.top;

    if (pkL > 1.0f) {
      // Green part up to 0dB, red above
      prevPen = (HPEN)SelectObject(hdc, greenPenL);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, clipY0dBTop);
      SelectObject(hdc, redPen);
      MoveToEx(hdc, x, clipY0dBTop, nullptr);
      LineTo(hdc, x, yL);
      SelectObject(hdc, prevPen);
    } else {
      prevPen = (HPEN)SelectObject(hdc, greenPenL);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, yL);
      SelectObject(hdc, prevPen);
    }
  }

  // R channel (bottom half)
  HPEN greenPenR = CreatePen(PS_SOLID, 1, RGB(30, 140, 50));

  for (int col = 0; col < columnsToShow; col++) {
    int bufIdx = (m_masterPeakHead - columnsToShow + col + MASTER_ROLLING_SIZE) % MASTER_ROLLING_SIZE;
    float pkR = m_masterPeakBufR[bufIdx];

    int x = waveRight - columnsToShow + col;
    int yR = centerY + (int)(pkR * halfH);
    if (yR > r.bottom) yR = r.bottom;

    if (pkR > 1.0f) {
      prevPen = (HPEN)SelectObject(hdc, greenPenR);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, clipY0dBBot);
      SelectObject(hdc, redPen);
      MoveToEx(hdc, x, clipY0dBBot, nullptr);
      LineTo(hdc, x, yR);
      SelectObject(hdc, prevPen);
    } else {
      prevPen = (HPEN)SelectObject(hdc, greenPenR);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, yR);
      SelectObject(hdc, prevPen);
    }
  }

  DeleteObject(greenPenL);
  DeleteObject(greenPenR);
  DeleteObject(redPen);

  // Clip line at 0dB (1.0 linear)
  int clipYTop = centerY - (int)(1.0f * halfH);
  int clipYBot = centerY + (int)(1.0f * halfH);
  HPEN clipPen = CreatePen(PS_SOLID, 1, RGB(100, 40, 40));
  prevPen = (HPEN)SelectObject(hdc, clipPen);
  MoveToEx(hdc, r.left, clipYTop, nullptr);
  LineTo(hdc, waveRight, clipYTop);
  MoveToEx(hdc, r.left, clipYBot, nullptr);
  LineTo(hdc, waveRight, clipYBot);
  SelectObject(hdc, prevPen);
  DeleteObject(clipPen);

  // "MASTER" label
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(100, 100, 100));
  RECT lblRect = { r.left + 6, r.top + 4, r.left + 120, r.top + 20 };
  DrawText(hdc, "MASTER", -1, &lblRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

  // dB scale (right column)
  RECT colRect = { scaleLeft, r.top, r.right, r.bottom };
  HBRUSH colBrush = CreateSolidBrush(RGB(25, 25, 25));
  FillRect(hdc, &colRect, colBrush);
  DeleteObject(colBrush);

  HPEN borderPen2 = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
  HPEN oldPen2 = (HPEN)SelectObject(hdc, borderPen2);
  MoveToEx(hdc, scaleLeft, r.top, nullptr);
  LineTo(hdc, scaleLeft, r.bottom);
  SelectObject(hdc, oldPen2);
  DeleteObject(borderPen2);

  SetTextColor(hdc, g_theme.dbScaleText);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

  RECT hdrRect = { scaleLeft + 2, r.top + 1, r.right - 2, r.top + 13 };
  DrawText(hdc, "dB", -1, &hdrRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // -∞ at center
  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
  HPEN tpOld = (HPEN)SelectObject(hdc, tickPen);
  MoveToEx(hdc, scaleLeft + 1, centerY, nullptr);
  LineTo(hdc, scaleLeft + 5, centerY);
  SelectObject(hdc, tpOld);
  RECT infR = { scaleLeft + 5, centerY - 6, r.right - 2, centerY + 6 };
  DrawText(hdc, "-\xE2\x88\x9E", -1, &infR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  static const double dbVals[] = { -48, -36, -24, -18, -12, -6, -3, 0 };
  int lastYTop = centerY, lastYBot = centerY;
  for (double db : dbVals) {
    double lin = pow(10.0, db / 20.0);
    int yOff = (int)(lin * (double)halfH);
    if (yOff < 1) continue;

    // Top half
    int yt = centerY - yOff;
    if (yt > r.top + 2 && lastYTop - yt >= 13) {
      tpOld = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, yt, nullptr);
      LineTo(hdc, scaleLeft + 5, yt);
      SelectObject(hdc, tpOld);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", (int)db);
      RECT tr = { scaleLeft + 5, yt - 6, r.right - 2, yt + 6 };
      DrawText(hdc, lbl, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      lastYTop = yt;
    }
    // Bottom half
    int yb = centerY + yOff;
    if (yb < r.bottom - 2 && yb - lastYBot >= 13) {
      tpOld = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, yb, nullptr);
      LineTo(hdc, scaleLeft + 5, yb);
      SelectObject(hdc, tpOld);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", (int)db);
      RECT tr = { scaleLeft + 5, yb - 6, r.right - 2, yb + 6 };
      DrawText(hdc, lbl, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      lastYBot = yb;
    }
  }
  DeleteObject(tickPen);
  SelectObject(hdc, oldFont);
}

void SneakPeak::DrawBottomPanel(HDC hdc)
{
  // Dark background for entire bottom panel
  HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24));
  FillRect(hdc, &m_bottomPanelRect, bg);
  DeleteObject(bg);

  // Top border
  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_bottomPanelRect.left, m_bottomPanelRect.top, nullptr);
  LineTo(hdc, m_bottomPanelRect.right, m_bottomPanelRect.top);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  int panelW = m_bottomPanelRect.right - m_bottomPanelRect.left;
  int infoW = panelW * 35 / 100; // 35% for info, 65% for RMS
  if (infoW > 320) infoW = 320;
  int dividerX = m_bottomPanelRect.right - infoW;

  // Meters on the left (fills most space)
  RECT metersRect = { m_bottomPanelRect.left, m_bottomPanelRect.top + 1,
                      dividerX - 1, m_bottomPanelRect.bottom };
  m_metersRect = metersRect;
  int meterCh = (m_masterMode || m_meterFromMaster) ? 2 : m_waveform.GetNumChannels();
  m_levels.Draw(hdc, metersRect, meterCh);

  if (!m_waveform.HasItem() && !m_masterMode) return;

  // Divider line
  HPEN divPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
  HPEN prevDivPen = (HPEN)SelectObject(hdc, divPen);
  MoveToEx(hdc, dividerX, m_bottomPanelRect.top + 1, nullptr);
  LineTo(hdc, dividerX, m_bottomPanelRect.bottom);
  SelectObject(hdc, prevDivPen);
  DeleteObject(divPen);

  // Info text on the right side
  SetBkMode(hdc, TRANSPARENT);
  int infoLeft = dividerX + 6;
  int infoRight = m_bottomPanelRect.right - 4;
  int panelTop = m_bottomPanelRect.top + 2;
  int panelBot = m_bottomPanelRect.bottom - 1;
  int rowH = (panelBot - panelTop) / 3;

  // Row 1: Selection / Cursor
  {
    RECT r = { infoLeft, panelTop, infoRight, panelTop + rowH };
    char line[256];
    if (m_waveform.HasSelection()) {
      WaveformSelection sel = m_waveform.GetSelection();
      char sStart[16], sEnd[16], sDur[16];
      FormatTimeHMS(sel.startTime, sStart, sizeof(sStart));
      FormatTimeHMS(sel.endTime, sEnd, sizeof(sEnd));
      FormatTimeHMS(sel.endTime - sel.startTime, sDur, sizeof(sDur));
      snprintf(line, sizeof(line), "Sel: %s - %s  Dur: %s", sStart, sEnd, sDur);
      SetTextColor(hdc, RGB(210, 210, 210));
    } else {
      char sCur[16];
      FormatTimeHMS(m_waveform.GetCursorTime(), sCur, sizeof(sCur));
      snprintf(line, sizeof(line), "Cursor: %s", sCur);
      SetTextColor(hdc, RGB(170, 170, 170));
    }
    DrawText(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Row 2: View range
  {
    RECT r = { infoLeft, panelTop + rowH, infoRight, panelTop + rowH * 2 };
    char vStart[16], vEnd[16], vDur[16];
    FormatTimeHMS(m_waveform.GetViewStart(), vStart, sizeof(vStart));
    FormatTimeHMS(m_waveform.GetViewEnd(), vEnd, sizeof(vEnd));
    FormatTimeHMS(m_waveform.GetViewDuration(), vDur, sizeof(vDur));
    char line[256];
    snprintf(line, sizeof(line), "View: %s - %s  Dur: %s", vStart, vEnd, vDur);
    SetTextColor(hdc, RGB(140, 140, 140));
    DrawText(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Row 3: Format info
  {
    RECT r = { infoLeft, panelTop + rowH * 2, infoRight, panelBot };

    double fileSizeMB = m_cachedFileSizeMB;

    char tTotal[16];
    FormatTimeHMS(m_waveform.GetItemDuration(), tTotal, sizeof(tTotal));

    const char* fmtName = (m_wavAudioFormat == 3) ? "Float" : "PCM";
    char line[256];
    snprintf(line, sizeof(line), "%dHz %s%d %dCh  %.1fMB  %s",
             m_waveform.GetSampleRate(), fmtName, m_wavBitsPerSample,
             m_waveform.GetNumChannels(), fileSizeMB, tTotal);
    SetTextColor(hdc, RGB(110, 110, 110));
    DrawText(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

}

void SneakPeak::ShowToast(const char* text)
{
  snprintf(m_toastText, sizeof(m_toastText), "%s", text);
  m_toastStartTick = GetTickCount();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DrawToast(HDC hdc)
{
  if (!m_toastStartTick) return;
  DWORD elapsed = GetTickCount() - m_toastStartTick;
  if (elapsed > 2000) { m_toastStartTick = 0; return; }

  // Fade out in last 500ms
  int alpha = 255;
  if (elapsed > 1500) alpha = 255 - (int)(255.0 * (elapsed - 1500) / 500.0);

  // Draw centered pill on waveform area
  int textLen = (int)strlen(m_toastText);
  if (textLen == 0) return;

  int pillW = textLen * 9 + 24;
  int pillH = 26;
  int cx = (m_waveformRect.left + m_waveformRect.right) / 2;
  int cy = m_waveformRect.top + 30;
  RECT pill = { cx - pillW/2, cy - pillH/2, cx + pillW/2, cy + pillH/2 };

  // Background
  int bg = (alpha * 40) / 255;
  HBRUSH bgBrush = CreateSolidBrush(RGB(bg, bg + bg/4, bg));
  FillRect(hdc, &pill, bgBrush);
  DeleteObject(bgBrush);

  // Border
  HPEN pen = CreatePen(PS_SOLID, 1, RGB((alpha*80)/255, (alpha*80)/255, (alpha*80)/255));
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, pill.left, pill.top, nullptr);
  LineTo(hdc, pill.right, pill.top);
  LineTo(hdc, pill.right, pill.bottom);
  LineTo(hdc, pill.left, pill.bottom);
  LineTo(hdc, pill.left, pill.top);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Text
  SetBkMode(hdc, TRANSPARENT);
  int g = (alpha * 230) / 255;
  SetTextColor(hdc, RGB(g, g, g));
  DrawText(hdc, m_toastText, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

