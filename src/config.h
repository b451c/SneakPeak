// config.h — Layout constants for EditView
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

// Waveform
inline constexpr int WAVEFORM_PADDING_TOP = 4;
inline constexpr int WAVEFORM_PADDING_BOTTOM = 4;
inline constexpr float DEFAULT_VERTICAL_ZOOM = 1.0f;
inline constexpr float MAX_VERTICAL_ZOOM = 10.0f;
inline constexpr float MIN_VERTICAL_ZOOM = 0.1f;
inline constexpr double ZOOM_FACTOR = 1.25;  // per scroll step
