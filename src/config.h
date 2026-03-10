// config.h — Layout constants for EditView
#pragma once

#include "platform.h"

// Layout
static const int TOOLBAR_HEIGHT = 0;
static const int RULER_HEIGHT = 24;
static const int SCROLLBAR_HEIGHT = 14;
static const int BOTTOM_PANEL_HEIGHT = 52;
static const int CHANNEL_SEPARATOR_HEIGHT = 3;
static const int DB_SCALE_WIDTH = 42;
static const int MIN_WINDOW_WIDTH = 400;
static const int MIN_WINDOW_HEIGHT = 200;

// Waveform
static const int WAVEFORM_PADDING_TOP = 4;
static const int WAVEFORM_PADDING_BOTTOM = 4;
static const float DEFAULT_VERTICAL_ZOOM = 1.0f;
static const float MAX_VERTICAL_ZOOM = 10.0f;
static const float MIN_VERTICAL_ZOOM = 0.1f;
static const double ZOOM_FACTOR = 1.25;  // per scroll step
