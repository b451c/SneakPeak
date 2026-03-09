// theme.h — REAPER theme color integration for EditView
#pragma once

#include "platform.h"

// All UI colors, loaded from REAPER's active theme
struct ThemeColors {
  // Waveform area (Audition-style: light bg, dark waveform)
  COLORREF waveformBg;       // normal background (light teal/mint)
  COLORREF waveformSelBg;    // selection background (white/very light)
  COLORREF waveform;         // waveform stroke color (green, normal areas)
  COLORREF waveformSel;      // waveform stroke in selection (dark green)
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

  // General
  COLORREF border;
  COLORREF emptyText;
};

// Global theme instance
extern ThemeColors g_theme;

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
