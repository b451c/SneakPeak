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
#include "audio_ops.h"
#include "theme.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

// Device-pixel ratio for crisp HiDPI Blend2D rendering. macOS Retina backing is
// 2x while SWELL window coords are logical points; Windows client coords are
// already physical px (legibility there is the size-preset feature, not this dpr);
// Linux/SWELL has no reliable scale. The renderer multiplies by this and the
// overlay scaled-blits onto the real window DC so a 2x image lands on the Retina
// backing crisply. (See .harness/design_phase2_architecture.md - DPI section.)
double SneakPeak::GetUiDpr() const
{
  // SWELL_IsRetinaHWND is macOS-only (guarded by SWELL_TARGET_OSX inside SWELL),
  // so it must NOT be referenced on Linux/GDK or the premium build fails to link.
  // Windows + Linux have no per-window Retina backing here -> dpr is 1.0.
#ifdef SWELL_TARGET_OSX
  return (m_hwnd && SWELL_IsRetinaHWND(m_hwnd)) ? 2.0 : 1.0;
#else
  return 1.0;
#endif
}

// HiDPI overlay pass: drawn on the REAL window DC after the 1x backbuffer is
// composited (drawing through the 1x OnPaint memDC would force the panel back to
// 1x and defeat Retina). Empty when no premium overlay is active. The premium
// Dynamics Panel moves here in Phase 2 (Inc 3+).
void SneakPeak::OnPaintOverlay(HDC hdc)
{
  if (!hdc) return;
#ifdef SNEAKPEAK_BLEND2D_PANEL
  m_dynamicsPanel.DrawPremium(hdc, m_waveformRect, GetUiDpr());
  // Premium gain knob (above dynamics, below settings/toast per the z-order spec).
  // Same HasItem gate as the GDI path had in OnPaint.
  if (m_waveform.HasItem())
    m_gainPanel.DrawPremium(hdc, m_waveformRect, GetUiDpr(), m_waveform.HasSelection());
  // Premium L/R meters into the bottom-panel meters rect (computed by
  // DrawBottomPanel earlier in this same paint; bg/divider/info stay GDI).
  if (m_showMeters && m_metersRect.right > m_metersRect.left) {
    int meterCh = (m_masterMode || m_meterFromMaster) ? 2 : m_waveform.GetNumChannels();
    m_levels.DrawPremium(hdc, m_metersRect, meterCh, GetUiDpr());
  }
  DrawToastPremium(hdc);   // always LAST - the toast sits above every premium surface
  {
    // Settings panel (above dynamics): hand it the host-owned preference values.
    SettingsPrefs sp;
    sp.rulerMode       = (int)m_rulerMode;
    sp.meterMode       = (int)m_levels.GetMode();
    sp.meterFromMaster = m_meterFromMaster;
    sp.showMeters      = m_showMeters;
    sp.showRuler       = m_showRuler;
    sp.showRMS         = m_waveform.GetShowRMS();
    sp.snapZero        = m_waveform.GetSnapToZero();
    sp.minimap         = m_minimapVisible;
    sp.zoomOnCursor    = m_zoomOnEditCursor;
    m_settingsPanel.DrawPremium(hdc, m_waveformRect, GetUiDpr(), sp);
  }
#endif
}

void SneakPeak::OnPaint(HDC hdc)
{
  if (!hdc) return;

  // Recreate the GDI fonts at the current g_uiScale if the scale changed. This is
  // the ONLY place fonts are rebuilt - done here, before any SelectObject, so a
  // mid-paint scale change can never leave a dangling HFONT selected into a DC.
  if (g_fontsNeedRescale) { Theme_CreateFonts(); g_fontsNeedRescale = false; }

  DrawModeBar(hdc);
  DrawRuler(hdc);
  if (m_masterMode) {
    DrawMasterWaveform(hdc);
  } else {
    // Panel overlay toggles: sync envelope visibility from panel toggle
    if (m_dynamicsPanel.IsVisible()) {
      bool wantEnv = m_dynamicsPanel.GetShowEnv();
      if (m_waveform.GetShowVolumeEnvelope() != wantEnv)
        m_waveform.SetShowVolumeEnvelope(wantEnv);
    }
    m_waveform.Paint(hdc);
    bool showDyn = m_waveform.HasItem() && m_dynamicsVisible && m_dynamics.HasResults() &&
        !m_waveform.IsMultiItemActive() &&
        (!m_dynamicsPanel.IsVisible() || m_dynamicsPanel.GetShowDyn());
    if (showDyn)
      DrawDynamicsCurve(hdc);
    // T2-1: live tension readout beside the cursor during a curvature drag
    if (m_envTensionDragging) {
      char tbuf[16];
      snprintf(tbuf, sizeof(tbuf), "%+.2f", m_envTensionCur);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(0, 200, 255));
      HFONT oldF = (HFONT)SelectObject(hdc, g_fonts.normal10);
      RECT tr = { m_lastMouseX + 14, m_lastMouseY - 18,
                  m_lastMouseX + 90, m_lastMouseY - 2 };
      DrawTextUTF8(hdc, tbuf, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(hdc, oldF);
    }
#ifndef SNEAKPEAK_BLEND2D_PANEL
    m_dynamicsPanel.Draw(hdc, m_waveformRect);
#endif
    // (premium panel drawn in OnPaintOverlay on the real window DC)
    // Envelope selection rectangle overlay: frosted translucent interior
    // (the spectral-marquee treatment; replaces the old hatched-lines fake)
    // + cyan border. Clamped to the waveform rect - the drag can leave it.
    if (m_envRectSelecting) {
      int rx1 = std::min(m_envRectStartX, m_envRectEndX);
      int ry1 = std::min(m_envRectStartY, m_envRectEndY);
      int rx2 = std::max(m_envRectStartX, m_envRectEndX);
      int ry2 = std::max(m_envRectStartY, m_envRectEndY);
      int cx1 = std::max(rx1, (int)m_waveformRect.left);
      int cy1 = std::max(ry1, (int)m_waveformRect.top);
      int cx2 = std::min(rx2, (int)m_waveformRect.right);
      int cy2 = std::min(ry2, (int)m_waveformRect.bottom);
      DrawFrostedRect(hdc, cx1, cy1, cx2, cy2, 40);  // ~16% toward white
      // Cyan border
      HPEN rectPen = CreatePen(PS_SOLID, 1, g_theme.volumeEnvelope);
      HPEN oldPen = (HPEN)SelectObject(hdc, rectPen);
      HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, rx1, ry1, rx2, ry2);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldBrush);
      DeleteObject(rectPen);
    }
  }
  if (m_markers.GetShowMarkers()) m_markers.DrawMarkers(hdc, m_waveformRect, m_rulerRect, m_waveform);
#ifndef SNEAKPEAK_BLEND2D_PANEL
  // GDI gain panel (OFF build only - the premium build draws it in OnPaintOverlay)
  if (m_waveform.HasItem()) m_gainPanel.Draw(hdc, m_waveformRect, m_waveform.HasSelection());
#endif
  if (m_waveform.HasItem()) DrawSoloButton(hdc);
  if (m_spectralVisible) {
    DrawSplitter(hdc);
    m_spectral.Paint(hdc, m_waveform);
  }
  if (m_minimapVisible) m_minimap.Paint(hdc, m_waveform);
  DrawScrollbar(hdc);
  if (m_showMeters) DrawBottomPanel(hdc);
#ifndef SNEAKPEAK_BLEND2D_PANEL
  // GDI toast (OFF build only - the premium build draws it last in OnPaintOverlay)
  DrawToast(hdc);
#endif
}

void SneakPeak::DrawSplitter(HDC hdc)
{
  if (m_splitterRect.bottom <= m_splitterRect.top) return;

  HBRUSH bg = CreateSolidBrush(RGB(45, 45, 45));
  FillRect(hdc, &m_splitterRect, bg);
  DeleteObject(bg);

  // Grip dots in center (dot size 3px = hairline detail, KEEP; spread scales)
  int cx = (m_splitterRect.left + m_splitterRect.right) / 2;
  int cy = (m_splitterRect.top + m_splitterRect.bottom) / 2;
  HBRUSH dot = CreateSolidBrush(RGB(120, 120, 120));
  for (int dx = -SP(12); dx <= SP(12); dx += SP(6)) {
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

  // Hover feedback helper: brighten a fill colour for the element under the cursor.
  auto lightenCol = [](COLORREF c, int d) -> COLORREF {
    int r = GetRValue(c) + d, g = GetGValue(c) + d, b = GetBValue(c) + d;
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
  };

  m_modeBarTabs.clear();
  int xPos = SP(6);
  int yMid = m_modeBarRect.top + h / 2;

  bool isStandalone = m_waveform.IsStandaloneMode() || !m_standaloneFiles.empty();
  bool isReaper = m_waveform.HasItem() && !m_waveform.IsStandaloneMode();
  bool isEmpty = !m_waveform.HasItem() && m_standaloneFiles.empty();

  // MASTER tab — always available (on the right side, drawn later)
  // We'll draw it after the main content

  if (isEmpty && !m_masterMode) {
    // Empty state
    SetTextColor(hdc, g_theme.emptyText);
    RECT textR = { xPos, m_modeBarRect.top, m_modeBarRect.right - SP(80), m_modeBarRect.bottom };
    DrawTextUTF8(hdc, "SneakPeak v" SNEAKPEAK_VERSION " - Drop audio file or select item", -1, &textR,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (isEmpty && m_masterMode) {
    // Master mode active — show indicator
    COLORREF accent = RGB(200, 80, 80);
    HBRUSH accentBrush = CreateSolidBrush(accent);
    RECT dot = { xPos, yMid - SP(3), xPos + SP(7), yMid + SP(4) };
    FillRect(hdc, &dot, accentBrush);
    DeleteObject(accentBrush);
    xPos += SP(12);
    SetTextColor(hdc, accent);
    RECT labelR = { xPos, m_modeBarRect.top, xPos + SP(80), m_modeBarRect.bottom };
    DrawTextUTF8(hdc, "MASTER", -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else {
    // Mode indicator
    COLORREF accent;
    const char* modeLabel;
    if (m_workingSet.active && isReaper) {
      accent = RGB(80, 200, 100);  // green for track view
      modeLabel = "SET";
    } else if (isStandalone && !isReaper) {
      accent = g_theme.modeBarStandaloneAccent;
      modeLabel = "STANDALONE";
    } else if (m_waveform.IsTimelineView()) {
      accent = RGB(180, 140, 255);  // purple for timeline
      modeLabel = "TIMELINE";
    } else if (m_waveform.IsMultiItemActive()) {
      accent = RGB(255, 180, 80);   // orange for multi-item
      modeLabel = "MULTI";
    } else {
      accent = g_theme.modeBarReaperAccent;
      modeLabel = "ITEM";
    }

    // Draw indicator dot/diamond
    HBRUSH accentBrush = CreateSolidBrush(accent);
    if (isStandalone && !isReaper) {
      // Orange filled circle (small rounded rect)
      RECT dot = { xPos, yMid - SP(3), xPos + SP(7), yMid + SP(4) };
      FillRect(hdc, &dot, accentBrush);
    } else {
      // Blue diamond — draw as small rotated square using lines
      int cx = xPos + SP(3), cy = yMid;
      POINT diamond[4] = { {cx, cy-SP(4)}, {cx+SP(4), cy}, {cx, cy+SP(4)}, {cx-SP(4), cy} };
      HPEN acPen = CreatePen(PS_SOLID, 1, accent);
      HPEN prevPen = (HPEN)SelectObject(hdc, acPen);
      HBRUSH prevBr = (HBRUSH)SelectObject(hdc, accentBrush);
      Polygon(hdc, diamond, 4);
      SelectObject(hdc, prevPen);
      SelectObject(hdc, prevBr);
      DeleteObject(acPen);
    }
    DeleteObject(accentBrush);
    xPos += SP(12);

    // Mode label
    SetTextColor(hdc, accent);
    RECT labelR = { xPos, m_modeBarRect.top, xPos + SP(80), m_modeBarRect.bottom };
    DrawTextUTF8(hdc, modeLabel, -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT labelMeasure = { 0, 0, 200, 20 };
    DrawTextUTF8(hdc, modeLabel, -1, &labelMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int labelW = labelMeasure.right - labelMeasure.left;
    m_modeLabelRect = { xPos - SP(12), m_modeBarRect.top, xPos + labelW + SP(4), m_modeBarRect.bottom };
    xPos += labelW + SP(8);

    // Separator
    HPEN sepPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HPEN sPrev = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, xPos, m_modeBarRect.top + SP(3), nullptr);
    LineTo(hdc, xPos, m_modeBarRect.bottom - SP(3));
    SelectObject(hdc, sPrev);
    DeleteObject(sepPen);
    xPos += SP(8);

    // Switch to normal weight for tabs
    oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

    int tabAreaRight = m_modeBarRect.right - SP(8);

    // ITEM pseudo-tab (if we have standalone files and we're in REAPER mode, or for switching back)
    if (isReaper && !m_standaloneFiles.empty()) {
      // Active ITEM tab
      char reaperLabel[256] = "ITEM";
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
      DrawTextUTF8(hdc, reaperLabel, -1, &tsR, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      int tw = std::min((int)(tsR.right - tsR.left) + SP(12), SP(MODE_TAB_MAX_W));

      bool hovTab = (m_modeBarHover == (int)m_modeBarTabs.size());
      RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
      HBRUSH tabBg = CreateSolidBrush(hovTab ? lightenCol(g_theme.modeBarActiveTab, 14)
                                             : g_theme.modeBarActiveTab);
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
      RECT textR = { tabR.left + SP(6), tabR.top, tabR.right - SP(6), tabR.bottom };
      DrawTextUTF8(hdc, reaperLabel, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

      ModeBarTab mbt;
      mbt.rect = tabR;
      mbt.closeRect = {};
      mbt.fileIdx = -1;
      mbt.isReaper = true;
      m_modeBarTabs.push_back(mbt);
      xPos = tabR.right + SP(2);
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
      DrawTextUTF8(hdc, label, -1, &tsR2, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      bool isActive = (m_waveform.IsStandaloneMode() && i == m_activeFileIdx);
      int closeW = isActive ? SP(MODE_TAB_CLOSE_SIZE) : 0;
      int tw = std::min((int)(tsR2.right - tsR2.left) + SP(12) + closeW, SP(MODE_TAB_MAX_W));
      if (xPos + tw > tabAreaRight) tw = tabAreaRight - xPos;
      if (tw < SP(20)) break;

      bool hovTab = (m_modeBarHover == (int)m_modeBarTabs.size());
      RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
      HBRUSH tabBg = CreateSolidBrush(
          isActive ? g_theme.modeBarActiveTab
                   : (hovTab ? lightenCol(g_theme.modeBarInactiveTab, 14)
                             : g_theme.modeBarInactiveTab));
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

      SetTextColor(hdc, (isActive || hovTab) ? RGB(220, 220, 220) : g_theme.modeBarText);
      int textRight = tabR.right - SP(4) - closeW;
      RECT textR = { tabR.left + SP(6), tabR.top, textRight, tabR.bottom };
      DrawTextUTF8(hdc, label, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

      // Close button on active tab
      RECT closeR = {};
      if (isActive) {
        int csz = SPmin(MODE_TAB_CLOSE_SIZE - 4);
        int cx = tabR.right - SP(MODE_TAB_CLOSE_SIZE) + 1;
        int cy = yMid - csz / 2;
        closeR = { cx, cy, cx + csz, cy + csz };
        SetTextColor(hdc, g_theme.modeBarText);
        DrawTextUTF8(hdc, "x", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }

      ModeBarTab mbt;
      mbt.rect = tabR;
      mbt.closeRect = closeR;
      mbt.fileIdx = i;
      mbt.isReaper = false;
      m_modeBarTabs.push_back(mbt);
      xPos = tabR.right + SP(2);
    }

    // If in standalone mode with no REAPER item showing, but we had a REAPER item before,
    // show an "ITEM" pseudo-tab for switching back
    if (!isReaper && !m_standaloneFiles.empty()) {
      // Check if there's a REAPER item available
      if (g_CountSelectedMediaItems && g_CountSelectedMediaItems(nullptr) > 0) {
        const char* rl = "ITEM";
        RECT tsR3 = { 0, 0, 300, 20 };
        DrawTextUTF8(hdc, rl, -1, &tsR3, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int tw = std::min((int)(tsR3.right - tsR3.left) + SP(12), SP(MODE_TAB_MAX_W));
        if (xPos + tw <= tabAreaRight) {
          bool hovTab = (m_modeBarHover == (int)m_modeBarTabs.size());
          RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
          HBRUSH tabBg = CreateSolidBrush(hovTab ? lightenCol(g_theme.modeBarInactiveTab, 14)
                                                 : g_theme.modeBarInactiveTab);
          FillRect(hdc, &tabR, tabBg);
          DeleteObject(tabBg);

          SetTextColor(hdc, g_theme.modeBarReaperAccent);
          RECT textR = { tabR.left + SP(6), tabR.top, tabR.right - SP(4), tabR.bottom };
          DrawTextUTF8(hdc, rl, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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

  // Right cluster order (left -> right): version, Support, MASTER tab, gear.
  // The gear is the LAST element at the far right edge (user feedback: it sat
  // between content and looked like a stray dot at 100%).
#ifdef SNEAKPEAK_BLEND2D_PANEL
  const int gearZone = SP(24);   // reserved at the right edge for the gear
#else
  const int gearZone = 0;
#endif

  // Version + Support link — subtle, right of content, left of MASTER
  {
    SelectObject(hdc, g_fonts.normal11);
    int verRight = m_modeBarRect.right - SP(70) - gearZone;
    SetTextColor(hdc, m_modeBarHover == MB_HOVER_VERSION ? RGB(150, 150, 150)
                                                         : RGB(90, 90, 90));
    RECT verR = { verRight - SP(130), m_modeBarRect.top, verRight - SP(50), m_modeBarRect.bottom };
    DrawTextUTF8(hdc, "v" SNEAKPEAK_VERSION, -1, &verR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    m_versionRect = verR;
    // Support button (heart + text)
    SetTextColor(hdc, m_modeBarHover == MB_HOVER_SUPPORT ? RGB(225, 120, 120)
                                                         : RGB(160, 80, 80));
    m_supportRect = { verRight - SP(46), m_modeBarRect.top, verRight, m_modeBarRect.bottom };
    DrawTextUTF8(hdc, "\xe2\x99\xa5 Support", -1, &m_supportRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

#ifdef SNEAKPEAK_BLEND2D_PANEL
  // Settings gear (premium): opens the Settings panel. Far right, bigger glyph
  // (bold14 - at normal11 it read as a stray dot at 100%).
  {
    HFONT prevF = (HFONT)SelectObject(hdc, g_fonts.bold14);
    SetTextColor(hdc, m_modeBarHover == MB_HOVER_GEAR ? RGB(225, 225, 225)
                                                      : RGB(150, 150, 150));
    m_gearRect = { m_modeBarRect.right - SP(26), m_modeBarRect.top,
                   m_modeBarRect.right - SP(2),  m_modeBarRect.bottom };
    DrawTextUTF8(hdc, "\xE2\x9A\x99", -1, &m_gearRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, prevF);
  }
#endif

  // MASTER tab — right-aligned (left of the gear), always visible
  {
    SelectObject(hdc, g_fonts.normal11);
    const char* ml = "MASTER";
    RECT tsM = { 0, 0, 200, 20 };
    DrawTextUTF8(hdc, ml, -1, &tsM, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int tw = (int)(tsM.right - tsM.left) + SP(14);
    int tabRight = m_modeBarRect.right - SP(6) - gearZone;
    int tabLeft = tabRight - tw;

    bool hovTab = (m_modeBarHover == (int)m_modeBarTabs.size());
    RECT tabR = { tabLeft, m_modeBarRect.top + 1, tabRight, m_modeBarRect.bottom - 1 };
    HBRUSH tabBg = CreateSolidBrush(
        m_masterMode ? g_theme.modeBarActiveTab
                     : (hovTab ? lightenCol(g_theme.modeBarInactiveTab, 14)
                               : g_theme.modeBarInactiveTab));
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

    SetTextColor(hdc, m_masterMode ? RGB(220, 220, 220)
                                   : (hovTab ? RGB(230, 110, 110) : RGB(200, 80, 80)));
    RECT textR = { tabR.left + SP(7), tabR.top, tabR.right - SP(7), tabR.bottom };
    DrawTextUTF8(hdc, ml, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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
  if (!m_showRuler) return;   // hidden: zero-height rect, nothing to paint

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
  if (m_rulerMode == RulerMode::BarsBeats) { DrawRulerBarsBeats(hdc); return; }

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
    if (iv * pixelsPerSec >= (double)SP(80)) { tickInterval = iv; break; }  // labels widen with the scaled font
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
    MoveToEx(hdc, tx, y + h - SP(8), nullptr);
    LineTo(hdc, tx, y + h - 1);
    SelectObject(hdc, op);

    // Format as HH:MM:SS;ms (use absolute timeline time in working set mode)
    char label[32];
    {
      double displayTime = (m_rulerMode != RulerMode::Relative) ? m_waveform.RelTimeToAbsTime(t) : t;
      int totalSec = (int)displayTime;
      int hours = totalSec / 3600;
      int mins = (totalSec % 3600) / 60;
      int secs = totalSec % 60;
      int ms = (int)((displayTime - totalSec) * 1000.0 + 0.5);
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
    // Vertically centered in the band above the major ticks (matches Bars&Beats;
    // DT_TOP glued the numbers to the ruler's top edge).
    RECT textRect = { tx + SP(3), y, tx + SP(80), y + h - SP(7) };
    DrawTextUTF8(hdc, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    double minorIv = tickInterval / 5.0;
    for (int mi = 1; mi < 5; mi++) {
      int mx = m_waveform.TimeToX(t + mi * minorIv);
      if (mx >= m_rulerRect.left && mx < m_rulerRect.right) {
        HPEN op2 = (HPEN)SelectObject(hdc, minorPen);
        MoveToEx(hdc, mx, y + h - SP(4), nullptr);
        LineTo(hdc, mx, y + h - 1);
        SelectObject(hdc, op2);
      }
    }
  }

  DeleteObject(tickPen);
  DeleteObject(minorPen);
  SelectObject(hdc, oldFont);

  // Group indicator bar at bottom of ruler (3px high)
  if (m_workingSet.active && m_workingSet.track && g_GetTrackNumMediaItems &&
      g_GetTrackMediaItem && g_GetMediaItemInfo_Value) {
    static const COLORREF groupColors[] = {
      RGB(90, 160, 220), RGB(220, 140, 60), RGB(120, 200, 120),
      RGB(200, 100, 180), RGB(180, 180, 80), RGB(100, 200, 200),
    };
    static const int NUM_COLORS = 6;
    int barH = SPmin(3);
    int barY = m_rulerRect.bottom - barH;
    int trackCount = g_GetTrackNumMediaItems(m_workingSet.track);

    for (int i = 0; i < trackCount; i++) {
      MediaItem* mi = g_GetTrackMediaItem(m_workingSet.track, i);
      if (!mi) continue;
      double p = g_GetMediaItemInfo_Value(mi, "D_POSITION");
      double l = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
      if (p + l <= m_workingSet.startPos || p >= m_workingSet.endPos) continue;

      int gid = (int)g_GetMediaItemInfo_Value(mi, "I_GROUPID");
      if (gid == 0) continue;

      // Map item position to SneakPeak view coordinates
      double relStart = m_waveform.AbsTimeToRelTime(p);
      double relEnd = m_waveform.AbsTimeToRelTime(p + l);
      int x1 = m_waveform.TimeToX(relStart);
      int x2 = m_waveform.TimeToX(relEnd);
      x1 = std::max((int)m_rulerRect.left, x1);
      x2 = std::min((int)m_rulerRect.right, x2);
      if (x2 <= x1) continue;

      COLORREF col = groupColors[gid % NUM_COLORS];
      HBRUSH br = CreateSolidBrush(col);
      RECT r = { x1, barY, x2, barY + barH };
      FillRect(hdc, &r, br);
      DeleteObject(br);
    }
  }
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
  int thumbW = std::max(SP(20), (int)((endRatio - startRatio) * (double)sw));

  COLORREF thumbColor = m_scrollbarDragging ? g_theme.scrollbarHover : g_theme.scrollbarThumb;
  RECT thumbRect = { thumbX, m_scrollbarRect.top + SP(2), thumbX + thumbW, m_scrollbarRect.bottom - SP(2) };
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
  int btnW = SP(22), btnH = SP(16);
  int btnX = m_waveformRect.right - SP(DB_SCALE_WIDTH) - btnW - SP(30);
  int btnY = m_waveformRect.top + SP(10);
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
  DrawTextUTF8(hdc, "S", 1, &m_soloBtnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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

void SneakPeak::DrawDynamicsCurve(HDC hdc)
{
  if (!m_dynamics.HasResults()) return;
  const auto& results = m_dynamics.GetResults();
  int count = (int)results.size();
  if (count == 0) return;

  RECT wr = m_waveform.GetRect();
  int waveL = wr.left;
  int waveR = wr.right - SP(DB_SCALE_WIDTH);
  int waveW = waveR - waveL;
  if (waveW <= 0) return;

  // Use waveform's amplitude-based Y mapping (channel 0) so dynamics curves
  // align exactly with the dB scale and grid lines on the waveform.
  // Waveform maps: y = centerY - linear_amplitude * halfH
  // where centerY is the channel center (silence) and halfH accounts for zoom.
  int chTop = m_waveform.GetChannelTop(0);
  int chH = m_waveform.GetChannelHeight();
  int centerY = chTop + chH / 2;
  float halfH = (float)(chH / 2) * m_waveform.GetVerticalZoom();
  int yClampTop = chTop;
  int yClampBot = chTop + chH - 1;
  if (chH < 10) return;

  // Helper: convert dB to Y pixel using same formula as DrawWaveformChannel/DrawDbScale
  auto dbToY = [&](double db) -> int {
    double linear = pow(10.0, db / 20.0);
    int y = centerY - (int)(linear * (double)halfH);
    return std::max(yClampTop, std::min(yClampBot, y));
  };

  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();
  double viewEnd = viewStart + viewDur;
  const auto& params = m_dynamics.GetParams();

  // Fade parameters
  auto fp = m_waveform.GetActiveFadeParams();
  double itemDur = m_waveform.GetItemDuration();

  // Gain knob visual offset (applies during drag, 1.0 = no change)
  double gainOffsetDb = 20.0 * log10(std::max(m_waveform.GetBatchGainOffset(), 1e-12));

  // Binary search: first result at or after viewStart
  int startIdx;
  {
    int lo = 0, hi = count;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (results[mid].time < viewStart) lo = mid + 1;
      else hi = mid;
    }
    startIdx = std::max(0, lo - 1); // one before for line continuity
  }

  // De-Ess Listen (INC-3): an amber lane at the top of the waveform over every
  // span with > 1 dB of de-ess reduction - the offline analog of a sidechain
  // "listen" switch (shows exactly where the de-esser bites, and any misfires).
  {
    const auto& dsGRs = m_dynamics.GetDeEssGRs();
    if (m_dynamicsPanel.IsVisible() && m_dynamicsPanel.GetDsListen() &&
        dsGRs.size() == results.size()) {
      HBRUSH amber = CreateSolidBrush(RGB(255, 170, 40));
      const int laneTop = wr.top + 1, laneBot = laneTop + SPmin(4);
      int spanX0 = -1;
      for (int i = startIdx; i < count && results[i].time <= viewEnd; i++) {
        bool hot = dsGRs[(size_t)i] < -1.0;
        int x = waveL + (int)((results[i].time - viewStart) / viewDur * (double)waveW);
        x = std::max(waveL, std::min(waveR, x));
        if (hot && spanX0 < 0) spanX0 = x;
        if (!hot && spanX0 >= 0) {
          RECT lane = { spanX0, laneTop, std::max(x, spanX0 + 2), laneBot };
          FillRect(hdc, &lane, amber);
          spanX0 = -1;
        }
      }
      if (spanX0 >= 0) {
        RECT lane = { spanX0, laneTop, waveR, laneBot };
        FillRect(hdc, &lane, amber);
      }
      DeleteObject(amber);
    }
  }

  // Stride: how many analysis points map to one pixel column
  double resultDt = (count > 1) ? (results[count - 1].time - results[0].time) / (double)(count - 1) : 0.001;
  double secsPerPx = viewDur / (double)waveW;
  int stride = std::max(1, (int)(secsPerPx / resultDt));

  // Helper: compute fade-adjusted dB for a dynamics point
  auto getAdjDb = [&](const DynamicsPoint& pt) -> double {
    double fadeGain = 1.0;
    if (fp.fadeInLen > 0.0 && pt.time < fp.fadeInLen)
      fadeGain *= ApplyFadeShape(pt.time / fp.fadeInLen, fp.fadeInShape, -fp.fadeInDir);
    if (fp.fadeOutLen > 0.0 && pt.time > itemDur - fp.fadeOutLen)
      fadeGain *= ApplyFadeShape((itemDur - pt.time) / fp.fadeOutLen, fp.fadeOutShape, fp.fadeOutDir);
    return pt.db + gainOffsetDb + 20.0 * log10(std::max(fadeGain, 1e-12));
  };

  // Helper: find the index of the point with max dB in a stride window (matches
  // waveform's max-peak-per-column behavior). Returns index into results[].
  auto getMaxIdxInStride = [&](int baseIdx) -> int {
    double maxDb = -200.0;
    int maxIdx = baseIdx;
    int end = std::min(count, baseIdx + stride);
    for (int j = baseIdx; j < end; j++) {
      double db = getAdjDb(results[j]);
      if (db > maxDb) { maxDb = db; maxIdx = j; }
    }
    return maxIdx;
  };

  // Compression preview params
  double thresh = m_dynamics.GetThreshold();
  double makeupDb = params.autoMakeup ? -m_dynamics.GetAvgGainReduction() : params.makeupDb;
  bool showComp = (params.ratio > 1.01);

  // Silence threshold: don't draw curves below this (prevents ugly spikes to bottom)
  static constexpr double SILENCE_FLOOR_DB = -45.0;

  // --- Gain reduction shading (filled area between original and compressed curves) ---
  bool showGR = showComp && (!m_dynamicsPanel.IsVisible() || m_dynamicsPanel.GetShowGR());
  if (showGR) {
    OwnedPen grPen(PS_SOLID, 1, RGB(100, 38, 28));
    DCPenScope scope(hdc, grPen);
    int lastPx = -2;

    for (int i = startIdx; i < count; i += stride) {
      int idx = getMaxIdxInStride(i);
      const auto& pt = results[idx];
      double adjDb = getAdjDb(pt);
      if (pt.time > viewEnd + 0.01) break;
      int px = m_waveform.TimeToX(pt.time);
      if (px < waveL || px > waveR) continue;
      if (px == lastPx) continue;
      lastPx = px;

      if (adjDb < SILENCE_FLOOR_DB) continue;

      int origY = dbToY(adjDb);
      double compDb = adjDb + pt.smoothedGR + makeupDb;
      int compY = dbToY(compDb);

      // Only shade where compression reduces level (compY below origY = lower amplitude)
      // Skip every other column for semi-transparent look (GDI has no alpha blending)
      if (compY > origY + 1 && (px & 1) == 0) {
        MoveToEx(hdc, px, origY, nullptr);
        LineTo(hdc, px, compY);
      }
    }
  }

  // --- Orange amplitude curve ---
  {
    OwnedPen dynPen(PS_SOLID, 1, RGB(200, 130, 50));
    DCPenScope scope(hdc, dynPen);
    bool first = true;
    int lastPx = -2;

    for (int i = startIdx; i < count; i += stride) {
      int idx = getMaxIdxInStride(i);
      double adjDb = getAdjDb(results[idx]);
      if (results[idx].time > viewEnd + 0.01) break;
      int px = m_waveform.TimeToX(results[idx].time);
      if (px < waveL || px > waveR) continue;
      if (px == lastPx) continue;
      lastPx = px;

      if (adjDb < SILENCE_FLOOR_DB) { first = true; continue; }

      int py = dbToY(adjDb);

      if (first) { MoveToEx(hdc, px, py, nullptr); first = false; }
      else LineTo(hdc, px, py);
    }
  }

  // --- Compression preview curve (uses precomputed gain-smoothed GR) ---
  if (showComp) {
    OwnedPen compPen(PS_SOLID, 1, RGB(140, 100, 200));
    DCPenScope scope(hdc, compPen);
    bool first = true;
    int lastPx = -2;

    for (int i = startIdx; i < count; i += stride) {
      int idx = getMaxIdxInStride(i);
      const auto& pt = results[idx];
      double adjDb = getAdjDb(pt);
      if (pt.time > viewEnd + 0.01) break;
      int px = m_waveform.TimeToX(pt.time);
      if (px < waveL || px > waveR) continue;
      if (px == lastPx) continue;
      lastPx = px;

      if (adjDb < SILENCE_FLOOR_DB) { first = true; continue; }

      // Use precomputed gain-smoothed GR from ComputeCompression
      double compDb = adjDb + pt.smoothedGR + makeupDb;
      int py = dbToY(compDb);

      if (first) { MoveToEx(hdc, px, py, nullptr); first = false; }
      else LineTo(hdc, px, py);
    }
  }

  // --- Threshold line (yellow horizontal) ---
  {
    int targetY = dbToY(thresh);

    OwnedPen targetPen(PS_SOLID, 1, RGB(180, 160, 50));
    DCPenScope scope(hdc, targetPen);
    MoveToEx(hdc, waveL, targetY, nullptr);
    LineTo(hdc, waveR, targetY);

    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(180, 160, 50));
    char label[32];
    snprintf(label, sizeof(label), "Thresh %.0f dB", thresh);
    RECT lr = { waveL + SP(4), targetY - SP(13), waveL + SP(120), targetY - 1 };
    DrawTextUTF8(hdc, label, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }

  // --- Gate threshold line (dim red horizontal, only when gate is active) ---
  if (params.gateThreshDb > -99.0) {
    int gateY = dbToY(params.gateThreshDb);

    OwnedPen gatePen(PS_SOLID, 1, RGB(140, 70, 70));
    DCPenScope scope(hdc, gatePen);
    MoveToEx(hdc, waveL, gateY, nullptr);
    LineTo(hdc, waveR, gateY);

    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(140, 70, 70));
    char label[32];
    snprintf(label, sizeof(label), "Gate %.0f dB", params.gateThreshDb);
    RECT lr = { waveL + SP(4), gateY + SP(2), waveL + SP(120), gateY + SP(14) };
    DrawTextUTF8(hdc, label, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
  }
}

void SneakPeak::DrawMasterWaveform(HDC hdc)
{
  RECT r = m_waveformRect;
  int scaleLeft = r.right - SP(DB_SCALE_WIDTH);
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
    DrawTextUTF8(hdc, "Master Output — play to see waveform", -1, &textRect,
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
  RECT lblRect = { r.left + SP(6), r.top + SP(4), r.left + SP(120), r.top + SP(20) };
  DrawTextUTF8(hdc, "MASTER", -1, &lblRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

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

  RECT hdrRect = { scaleLeft + SP(2), r.top + 1, r.right - SP(2), r.top + SP(13) };
  DrawTextUTF8(hdc, "dB", -1, &hdrRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // -∞ at center
  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
  HPEN tpOld = (HPEN)SelectObject(hdc, tickPen);
  MoveToEx(hdc, scaleLeft + 1, centerY, nullptr);
  LineTo(hdc, scaleLeft + SP(5), centerY);
  SelectObject(hdc, tpOld);
  RECT infR = { scaleLeft + SP(5), centerY - SP(6), r.right - SP(2), centerY + SP(6) };
  DrawTextUTF8(hdc, "-\xE2\x88\x9E", -1, &infR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  static const double dbVals[] = { -48, -36, -24, -18, -12, -6, -3, 0 };
  int lastYTop = centerY, lastYBot = centerY;
  for (double db : dbVals) {
    double lin = pow(10.0, db / 20.0);
    int yOff = (int)(lin * (double)halfH);
    if (yOff < 1) continue;

    // Top half
    int yt = centerY - yOff;
    if (yt > r.top + SP(2) && lastYTop - yt >= SP(DB_LABEL_MIN_SPACING)) {
      tpOld = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, yt, nullptr);
      LineTo(hdc, scaleLeft + SP(5), yt);
      SelectObject(hdc, tpOld);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", (int)db);
      RECT tr = { scaleLeft + SP(5), yt - SP(6), r.right - SP(2), yt + SP(6) };
      DrawTextUTF8(hdc, lbl, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      lastYTop = yt;
    }
    // Bottom half
    int yb = centerY + yOff;
    if (yb < r.bottom - SP(2) && yb - lastYBot >= SP(DB_LABEL_MIN_SPACING)) {
      tpOld = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, yb, nullptr);
      LineTo(hdc, scaleLeft + SP(5), yb);
      SelectObject(hdc, tpOld);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", (int)db);
      RECT tr = { scaleLeft + SP(5), yb - SP(6), r.right - SP(2), yb + SP(6) };
      DrawTextUTF8(hdc, lbl, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
  if (infoW > SP(320)) infoW = SP(320);
  int dividerX = m_bottomPanelRect.right - infoW;

  // Meters on the left (fills most space)
  RECT metersRect = { m_bottomPanelRect.left, m_bottomPanelRect.top + 1,
                      dividerX - 1, m_bottomPanelRect.bottom };
  m_metersRect = metersRect;
#ifndef SNEAKPEAK_BLEND2D_PANEL
  // GDI meter bars (OFF build only - the premium build draws them in OnPaintOverlay
  // at device resolution; this GDI panel still owns the bg/divider/info text)
  int meterCh = (m_masterMode || m_meterFromMaster) ? 2 : m_waveform.GetNumChannels();
  m_levels.Draw(hdc, metersRect, meterCh);
#endif

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
  int infoLeft = dividerX + SP(6);
  int infoRight = m_bottomPanelRect.right - SP(4);
  int panelTop = m_bottomPanelRect.top + SP(2);
  int panelBot = m_bottomPanelRect.bottom - 1;
  int rowH = (panelBot - panelTop) / 3;

  // Row 1: Selection / Cursor
  {
    RECT r = { infoLeft, panelTop, infoRight, panelTop + rowH };
    char line[256];
    if (m_waveform.HasSelection()) {
      WaveformSelection sel = m_waveform.GetSelection();
      bool tv = (m_rulerMode != RulerMode::Relative);
      double s1 = tv ? m_waveform.RelTimeToAbsTime(sel.startTime) : sel.startTime;
      double s2 = tv ? m_waveform.RelTimeToAbsTime(sel.endTime) : sel.endTime;
      char sStart[16], sEnd[16], sDur[16];
      FormatTimeHMS(s1, sStart, sizeof(sStart));
      FormatTimeHMS(s2, sEnd, sizeof(sEnd));
      FormatTimeHMS(std::abs(s2 - s1), sDur, sizeof(sDur));
      snprintf(line, sizeof(line), "Sel: %s - %s  Dur: %s", sStart, sEnd, sDur);
      SetTextColor(hdc, RGB(210, 210, 210));
    } else {
      char sCur[16];
      double ct = (m_rulerMode != RulerMode::Relative)
        ? m_waveform.RelTimeToAbsTime(m_waveform.GetCursorTime())
        : m_waveform.GetCursorTime();
      FormatTimeHMS(ct, sCur, sizeof(sCur));
      snprintf(line, sizeof(line), "Cursor: %s", sCur);
      SetTextColor(hdc, RGB(170, 170, 170));
    }
    DrawTextUTF8(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Row 2: View range
  {
    RECT r = { infoLeft, panelTop + rowH, infoRight, panelTop + rowH * 2 };
    char vStart[16], vEnd[16], vDur[16];
    double vs = (m_rulerMode != RulerMode::Relative) ? m_waveform.RelTimeToAbsTime(m_waveform.GetViewStart()) : m_waveform.GetViewStart();
    double ve = (m_rulerMode != RulerMode::Relative) ? m_waveform.RelTimeToAbsTime(m_waveform.GetViewEnd()) : m_waveform.GetViewEnd();
    FormatTimeHMS(vs, vStart, sizeof(vStart));
    FormatTimeHMS(ve, vEnd, sizeof(vEnd));
    FormatTimeHMS(m_waveform.GetViewDuration(), vDur, sizeof(vDur));
    char line[256];
    snprintf(line, sizeof(line), "View: %s - %s  Dur: %s", vStart, vEnd, vDur);
    SetTextColor(hdc, RGB(140, 140, 140));
    DrawTextUTF8(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
    DrawTextUTF8(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

}

void SneakPeak::ShowToast(const char* text)
{
  snprintf(m_toastText, sizeof(m_toastText), "%s", text);
  m_toastStartTick = GetTickCount();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DrawToastPremium(HDC hdc)
{
  if (!m_toastStartTick) return;
  DWORD elapsed = GetTickCount() - m_toastStartTick;
  if (elapsed > 2000) { m_toastStartTick = 0; return; }
  int textLen = (int)strlen(m_toastText);
  if (textLen == 0) return;

  // Same lifetime/fade/geometry model as the GDI toast (solid 1.5s, 500ms fade).
  double alpha = 1.0;
  if (elapsed > 1500) alpha = 1.0 - (double)(elapsed - 1500) / 500.0;

  int pillW = textLen * SP(9) + SP(24);
  int pillH = SP(26);
  int cx = (m_waveformRect.left + m_waveformRect.right) / 2;
  int cy = m_waveformRect.top + SP(30);

  ToastVM vm;
  vm.text    = m_toastText;
  vm.alpha   = alpha;
  vm.uiScale = g_uiScale;
  COLORREF bg = g_theme.waveformBg;   // COLORREF (0x00BBGGRR) -> ARGB
  vm.bgColor = 0xFF000000u | ((uint32_t)GetRValue(bg) << 16) |
               ((uint32_t)GetGValue(bg) << 8) | (uint32_t)GetBValue(bg);

  m_toastCanvas.RenderToast(hdc, cx - pillW / 2, cy - pillH / 2, pillW, pillH,
                            GetUiDpr(), vm);
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

  int pillW = textLen * SP(9) + SP(24);
  int pillH = SP(26);
  int cx = (m_waveformRect.left + m_waveformRect.right) / 2;
  int cy = m_waveformRect.top + SP(30);
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
  DrawTextUTF8(hdc, m_toastText, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// --- Bars & Beats ruler ---

void SneakPeak::DrawRulerBarsBeats(HDC hdc)
{
  if (!g_TimeMap2_timeToBeats || !g_TimeMap_GetMeasureInfo || !g_TimeMap_GetTimeSigAtTime) return;

  int w = m_rulerRect.right - m_rulerRect.left;
  int h = m_rulerRect.bottom - m_rulerRect.top;
  int y = m_rulerRect.top;

  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();
  if (viewDur <= 0) return;

  // Convert view bounds to absolute time for tempo map queries
  double absStart = m_waveform.RelTimeToAbsTime(viewStart);
  double absEnd = m_waveform.RelTimeToAbsTime(viewStart + viewDur);

  // Find starting measure
  int startMeasure = 0;
  g_TimeMap2_timeToBeats(nullptr, absStart, &startMeasure, nullptr, nullptr, nullptr);
  if (startMeasure > 0) startMeasure--;

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, g_theme.rulerText);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

  HPEN majorPen = CreatePen(PS_SOLID, 1, g_theme.rulerTick);
  HPEN minorPen = CreatePen(PS_SOLID, 1, g_theme.rulerTickMinor);

  // Iterate measures until we pass the visible range
  for (int m = startMeasure; ; m++) {
    double qnStart, qnEnd;
    int tsNum, tsDenom;
    double tempo;
    double measTime = g_TimeMap_GetMeasureInfo(nullptr, m, &qnStart, &qnEnd, &tsNum, &tsDenom, &tempo);
    if (measTime > absEnd + 1.0) break; // past visible range

    double nextMeasTime = g_TimeMap_GetMeasureInfo(nullptr, m + 1, nullptr, nullptr, nullptr, nullptr, nullptr);
    double measDur = nextMeasTime - measTime;
    if (measDur <= 0) break;

    // Convert to relative time for pixel positioning
    double relMeasTime = m_waveform.AbsTimeToRelTime(measTime);
    int mx = m_waveform.TimeToX(relMeasTime);

    // Draw major tick (measure start) + label
    if (mx >= m_rulerRect.left && mx < m_rulerRect.right) {
      HPEN op = (HPEN)SelectObject(hdc, majorPen);
      MoveToEx(hdc, mx, y + h - SP(8), nullptr);
      LineTo(hdc, mx, y + h - 1);
      SelectObject(hdc, op);

      char label[32];
      snprintf(label, sizeof(label), "%d", m + 1); // 1-based measure number
      RECT lr = { mx + SP(3), y + 1, mx + SP(60), y + h - SP(2) };
      DrawTextUTF8(hdc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Draw beat subdivisions within this measure
    if (tsNum <= 0) tsNum = 4;
    double beatDur = measDur / (double)tsNum;
    double minBeatPx = (double)SP(20); // minimum pixels between beat ticks
    double beatPx = beatDur / viewDur * (double)w;
    if (beatPx < minBeatPx) continue; // too dense, skip beats

    for (int b = 1; b < tsNum; b++) {
      double beatTime = measTime + beatDur * (double)b;
      double relBeat = m_waveform.AbsTimeToRelTime(beatTime);
      int bx = m_waveform.TimeToX(relBeat);
      if (bx >= m_rulerRect.left && bx < m_rulerRect.right) {
        HPEN op = (HPEN)SelectObject(hdc, minorPen);
        MoveToEx(hdc, bx, y + h - SP(5), nullptr);
        LineTo(hdc, bx, y + h - 1);
        SelectObject(hdc, op);

        // Beat label at higher zoom
        if (beatPx > (double)SP(50)) {
          char bl[16];
          snprintf(bl, sizeof(bl), "%d:%d", m + 1, b + 1);
          RECT br = { bx + SP(2), y + 1, bx + SP(50), y + h - SP(2) };
          DrawTextUTF8(hdc, bl, -1, &br, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
      }
    }
  }

  DeleteObject(majorPen);
  DeleteObject(minorPen);
  SelectObject(hdc, oldFont);
}

