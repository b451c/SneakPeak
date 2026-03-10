// theme.cpp — Load colors from REAPER's active theme
#include "theme.h"
#include "globals.h"
#include "debug.h"
#include <algorithm>

ThemeColors g_theme;

// GetThemeColor function pointer
static int (*s_GetThemeColor)(const char* ini_key, int flagsOptional) = nullptr;

void Theme_SetGetThemeColor(void* func)
{
  s_GetThemeColor = (int(*)(const char*, int))func;
}

// Perceptual luminance 0-255
static int colorLuminance(COLORREF c)
{
  return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

// Ensure minimum contrast between text and background
static COLORREF ensureContrast(COLORREF text, COLORREF bg, int minDiff = 100)
{
  int textLum = colorLuminance(text);
  int bgLum = colorLuminance(bg);
  int diff = abs(textLum - bgLum);
  if (diff >= minDiff) return text;
  // Force bright text on dark bg, dark text on bright bg
  if (bgLum < 128) return RGB(230, 230, 230);
  return RGB(30, 30, 30);
}

// Convert REAPER's OS-dependent color to COLORREF
// On macOS SWELL, colors might be in different byte order
static COLORREF themeColor(const char* key, COLORREF fallback)
{
  if (!s_GetThemeColor) return fallback;
  int c = s_GetThemeColor(key, 0);
  if (c == -1) return fallback;
  // REAPER returns OS-native color; on macOS SWELL this should be usable directly
  return (COLORREF)c;
}

COLORREF ColorBlend(COLORREF c1, COLORREF c2, float ratio)
{
  int r = (int)(GetRValue(c1) * (1.0f - ratio) + GetRValue(c2) * ratio);
  int g = (int)(GetGValue(c1) * (1.0f - ratio) + GetGValue(c2) * ratio);
  int b = (int)(GetBValue(c1) * (1.0f - ratio) + GetBValue(c2) * ratio);
  return RGB(std::max(0, std::min(255, r)),
             std::max(0, std::min(255, g)),
             std::max(0, std::min(255, b)));
}

COLORREF ColorDarken(COLORREF c, float amount)
{
  return ColorBlend(c, RGB(0, 0, 0), amount);
}

COLORREF ColorLighten(COLORREF c, float amount)
{
  return ColorBlend(c, RGB(255, 255, 255), amount);
}

void Theme_Init()
{
  Theme_Refresh();
}

void Theme_Refresh()
{
  // UI chrome colors from REAPER theme
  COLORREF arrangeBg = themeColor("col_arrangebg", RGB(30, 30, 30));
  COLORREF tlBg      = themeColor("col_tl_bg", ColorLighten(arrangeBg, 0.05f));
  COLORREF tlFg      = themeColor("col_tl_fg", RGB(180, 180, 180));
  COLORREF grid1     = themeColor("col_gridlines", RGB(60, 60, 60));
  COLORREF mainText  = themeColor("col_main_text2", RGB(200, 200, 200));
  COLORREF transBg   = themeColor("col_trans_bg", ColorLighten(arrangeBg, 0.06f));

  // --- Dark bg, green waveform (Audition style) ---
  g_theme.waveformBg    = RGB(0, 0, 0);              // black background
  g_theme.waveformSelBg = RGB(230, 230, 230);         // white/light selection background
  g_theme.waveform      = RGB(80, 220, 140);          // green waveform (normal)
  g_theme.waveformSel   = RGB(30, 80, 50);            // dark green waveform (in selection)
  g_theme.waveformRms   = RGB(40, 160, 90);            // RMS overlay (darker green)
  g_theme.waveformRmsSel = RGB(20, 55, 35);            // RMS overlay in selection
  g_theme.centerLine    = RGB(30, 60, 40);             // subtle dark green center line
  g_theme.dbScaleText   = RGB(160, 160, 160);          // light gray dB labels

  // Selection edges — red
  g_theme.selection     = RGB(200, 60, 40);
  g_theme.selectionEdge = RGB(200, 60, 40);

  // Cursors
  g_theme.editCursor    = RGB(220, 40, 40);            // RED edit cursor / playhead line
  g_theme.playhead      = RGB(220, 40, 40);             // red playhead

  // Toolbar — ensure text is always readable
  g_theme.toolbarBg     = transBg;
  g_theme.toolbarButtonBg = ColorLighten(transBg, 0.12f);
  g_theme.toolbarButtonBorder = ColorLighten(transBg, 0.25f);
  g_theme.toolbarHover  = ColorLighten(transBg, 0.25f);
  g_theme.toolbarText   = ensureContrast(mainText, g_theme.toolbarButtonBg, 120);
  g_theme.toolbarDisabled = ColorBlend(g_theme.toolbarButtonBg, g_theme.toolbarText, 0.35f);

  // Ruler
  g_theme.rulerBg       = tlBg;
  g_theme.rulerText     = ensureContrast(tlFg, tlBg, 100);
  g_theme.rulerTick     = ColorLighten(tlBg, 0.3f);
  g_theme.rulerTickMinor = ColorLighten(tlBg, 0.15f);

  // Info bar / scrollbar
  g_theme.infoBg        = ColorLighten(arrangeBg, 0.03f);
  g_theme.infoText      = ensureContrast(ColorBlend(arrangeBg, mainText, 0.7f), g_theme.infoBg, 80);
  g_theme.scrollbarBg   = ColorLighten(arrangeBg, 0.03f);
  g_theme.scrollbarThumb = ColorLighten(arrangeBg, 0.2f);
  g_theme.scrollbarHover = ColorLighten(arrangeBg, 0.3f);

  // Markers (bright yellow default — visible on dark bg)
  g_theme.markerLine    = RGB(220, 200, 60);
  g_theme.markerText    = RGB(220, 200, 60);

  // Clip indicators
  g_theme.clipIndicator = RGB(255, 50, 50);

  // General
  g_theme.border        = grid1;
  g_theme.emptyText     = RGB(100, 100, 100);

  DBG("[SneakPeak] Theme loaded: bg=%06X waveform=%06X cursor=%06X\n",
      g_theme.waveformBg, g_theme.waveform, g_theme.editCursor);
}
