// config.h — Layout constants for SneakPeak
#pragma once

#include "platform.h"

// Layout
inline constexpr int TOOLBAR_HEIGHT = 0;
inline constexpr int RULER_HEIGHT = 28;
inline constexpr int SCROLLBAR_HEIGHT = 14;
inline constexpr int BOTTOM_PANEL_HEIGHT = 52;
inline constexpr int CHANNEL_SEPARATOR_HEIGHT = 3;
inline constexpr int DB_SCALE_WIDTH = 42;
inline constexpr int MIN_WINDOW_WIDTH = 400;
inline constexpr int MIN_WINDOW_HEIGHT = 200;
inline constexpr int SPLITTER_HEIGHT = 5;
inline constexpr int MIN_WAVEFORM_HEIGHT = 80;
inline constexpr int MIN_SPECTRAL_HEIGHT = 60;
inline constexpr int MINIMAP_HEIGHT = 20;

// Mode bar
inline constexpr int MODE_BAR_HEIGHT = 20;
inline constexpr int MAX_STANDALONE_FILES = 8;
inline constexpr int MODE_TAB_MAX_W = 140;
inline constexpr int MODE_TAB_CLOSE_SIZE = 14;

// Waveform
inline constexpr int WAVEFORM_PADDING_TOP = 4;
inline constexpr int WAVEFORM_PADDING_BOTTOM = 4;
inline constexpr float DEFAULT_VERTICAL_ZOOM = 1.0f;
inline constexpr float MAX_VERTICAL_ZOOM = 10.0f;
inline constexpr float MIN_VERTICAL_ZOOM = 0.1f;
inline constexpr double ZOOM_FACTOR = 1.25;  // per scroll step

// Fade handles
inline constexpr int FADE_HANDLE_HIT_ZONE = 16;   // pixels: horizontal hit area for fade handle
inline constexpr int FADE_HANDLE_TOP_ZONE = 30;    // pixels: vertical hit zone from waveform top
inline constexpr int FADE_HANDLE_HALF_SIZE = 5;    // pixels: half-size of fade handle square
inline constexpr double FADE_MIN_LEN = 0.001;      // seconds: minimum fade length to display/hit-test

// Gain split parameters
inline constexpr double GAIN_XFADE_SEC = 0.01;    // crossfade overlap at split points (SET mode)
inline constexpr double GAIN_EDGE_EPS_SET = 0.015; // edge alignment epsilon for SET (> GAIN_XFADE_SEC)
inline constexpr double GAIN_EDGE_EPS = 0.001;     // edge alignment epsilon for timeline/single

// Rendering
inline constexpr double CLIP_RED_RATIO = 0.7;      // fraction of peak height before red clip starts
inline constexpr int DB_GRID_MIN_SPACING = 30;      // min pixels between dB grid lines
inline constexpr int DB_LABEL_MIN_SPACING = 13;     // min pixels between dB scale labels
inline constexpr double RULER_TICK_MIN_PX = 150.0;  // min pixels per ruler tick interval
inline constexpr int CHAN_BTN_WIDTH = 18;            // channel button width
inline constexpr int CHAN_BTN_HEIGHT = 16;           // channel button height

// Interaction
inline constexpr int EDGE_ZONE = 40;           // pixels from edge for auto-scroll
inline constexpr double AUTO_SCROLL_SPEED = 0.08; // fraction of view per tick during auto-scroll
inline constexpr int PLAY_GRACE_TICKS = 5;     // ticks to skip auto-stop after play start
inline constexpr int ZERO_SNAP_RANGE = 512;    // sample search radius for zero-crossing snap
inline constexpr int TIMELINE_EDIT_GUARD_TICKS = 5; // ticks to suppress LoadSelectedItem after edit

// Utility
#include <cstring>
inline const char* FileNameFromPath(const char* path) {
  if (!path) return "";
  const char* p = strrchr(path, '/');
#ifdef _WIN32
  const char* p2 = strrchr(path, '\\');
  if (p2 > p) p = p2;
#endif
  return p ? p + 1 : path;
}
