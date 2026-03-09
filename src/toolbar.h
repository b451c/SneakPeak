// toolbar.h — Toolbar for EditView
#pragma once

#include "platform.h"
#include "config.h"
#include "globals.h"

enum ToolbarButton {
  TB_ZOOM_IN = 0,
  TB_ZOOM_OUT,
  TB_ZOOM_FIT,
  TB_ZOOM_SEL,
  TB_SEPARATOR_1,
  TB_PLAY,
  TB_STOP,
  TB_SEPARATOR_2,
  TB_NORMALIZE,
  TB_FADE_IN,
  TB_FADE_OUT,
  TB_REVERSE,
  TB_SEPARATOR_3,
  TB_VZOOM_IN,
  TB_VZOOM_OUT,
  TB_VZOOM_RESET,
  TB_COUNT
};

struct ToolbarItem {
  const char* label;
  const char* tooltip;
  bool isSeparator;
  bool enabled;
  RECT rect;
};

class Toolbar {
public:
  Toolbar();

  void SetRect(int x, int y, int w, int h);
  RECT GetRect() const { return m_rect; }

  void Paint(HDC hdc);
  int HitTest(int x, int y) const;
  void SetHover(int buttonIdx) { m_hoverIdx = buttonIdx; }

  static const int BUTTON_WIDTH = 72;
  static const int BUTTON_PADDING = 2;
  static const int SEPARATOR_WIDTH = 10;

private:
  ToolbarItem m_items[TB_COUNT];
  RECT m_rect = {0, 0, 0, 0};
  int m_hoverIdx = -1;

  void InitItems();
  void RecalcLayout();
};
