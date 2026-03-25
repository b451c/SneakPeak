// theme.h — REAPER theme color integration for SneakPeak
#pragma once

#include "platform.h"

// All UI colors, loaded from REAPER's active theme
struct ThemeColors {
  // Waveform area (Audition-style: light bg, dark waveform)
  COLORREF waveformBg;       // normal background (light teal/mint)
  COLORREF waveformSelBg;    // selection background (white/very light)
  COLORREF waveform;         // waveform stroke color (green, normal areas)
  COLORREF waveformSel;      // waveform stroke in selection (dark green)
  COLORREF waveformRms;      // RMS overlay (darker green, normal)
  COLORREF waveformRmsSel;   // RMS overlay in selection
  COLORREF centerLine;       // zero-crossing line
  COLORREF dbScaleText;      // dB scale labels

  // Toolbar
  COLORREF toolbarBg;
  COLORREF toolbarText;
  COLORREF toolbarButtonBg;
  COLORREF toolbarButtonBorder;
  COLORREF toolbarHover;
  COLORREF toolbarDisabled;

  // Ruler / timeline
  COLORREF rulerBg;
  COLORREF rulerText;
  COLORREF rulerTick;
  COLORREF rulerTickMinor;

  // Selection
  COLORREF selection;
  COLORREF selectionEdge;

  // Cursors
  COLORREF editCursor;
  COLORREF playhead;

  // Info bar / scrollbar
  COLORREF infoBg;
  COLORREF infoText;
  COLORREF scrollbarBg;
  COLORREF scrollbarThumb;
  COLORREF scrollbarHover;

  // Markers
  COLORREF markerLine;
  COLORREF markerText;

  // Clip indicators
  COLORREF clipIndicator;

  // Mode bar
  COLORREF modeBarBg;
  COLORREF modeBarText;
  COLORREF modeBarActiveTab;
  COLORREF modeBarInactiveTab;
  COLORREF modeBarStandaloneAccent;
  COLORREF modeBarReaperAccent;

  // General
  COLORREF border;
  COLORREF emptyText;
};

// Global theme instance
extern ThemeColors g_theme;

// Cached fonts — created once, destroyed at exit
struct ThemeFonts {
  HFONT normal11 = nullptr;  // Arial 11 normal (ruler, dB scale, mode bar tabs)
  HFONT bold12   = nullptr;  // Arial 12 bold (solo, gain panel, channel btns, mode label)
  HFONT normal10 = nullptr;  // Arial 10 normal (spectral freq labels)
  HFONT bold10   = nullptr;  // Arial 10 bold (spectral freq bold)
  HFONT bold14   = nullptr;  // Arial 14 bold (fade envelope label)
  HFONT bold11   = nullptr;  // Arial 11 bold (markers)
  HFONT normal13 = nullptr;  // Arial 13 normal (spectral loading)
  HFONT toolbar  = nullptr;  // Arial 13 normal (toolbar buttons)
};
extern ThemeFonts g_fonts;
void Theme_CreateFonts();
void Theme_DestroyFonts();

// Set the GetThemeColor function pointer (call from main.cpp)
void Theme_SetGetThemeColor(void* func);

// Initialize theme colors from REAPER (call after API is loaded)
void Theme_Init();

// Refresh theme colors (call when theme changes)
void Theme_Refresh();

// Helper: darken/lighten a color
COLORREF ColorBlend(COLORREF c1, COLORREF c2, float ratio);
COLORREF ColorDarken(COLORREF c, float amount);
COLORREF ColorLighten(COLORREF c, float amount);
