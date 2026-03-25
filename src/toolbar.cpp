// toolbar.cpp — Toolbar rendering using GDI + theme colors
#include "toolbar.h"
#include "theme.h"
#include <cstring>

Toolbar::Toolbar() { InitItems(); }

void Toolbar::InitItems()
{
  memset(m_items, 0, sizeof(m_items));
  auto set = [this](int idx, const char* label, const char* tip, bool sep) {
    m_items[idx].label = label;
    m_items[idx].tooltip = tip;
    m_items[idx].isSeparator = sep;
    m_items[idx].enabled = !sep;
  };
  set(TB_ZOOM_IN,     "Zoom+",     "Zoom In",           false);
  set(TB_ZOOM_OUT,    "Zoom-",     "Zoom Out",          false);
  set(TB_ZOOM_FIT,    "Fit",       "Zoom to Fit",       false);
  set(TB_ZOOM_SEL,    "ZoomSel",   "Zoom to Selection", false);
  set(TB_SEPARATOR_1, nullptr,     nullptr,              true);
  set(TB_PLAY,        "Play",      "Play/Pause",        false);
  set(TB_STOP,        "Stop",      "Stop",              false);
  set(TB_SEPARATOR_2, nullptr,     nullptr,              true);
  set(TB_NORMALIZE,   "Normalize", "Normalize",         false);
  set(TB_FADE_IN,     "Fade In",   "Fade In",           false);
  set(TB_FADE_OUT,    "Fade Out",  "Fade Out",          false);
  set(TB_REVERSE,     "Reverse",   "Reverse",           false);
  set(TB_SEPARATOR_3, nullptr,     nullptr,              true);
  set(TB_VZOOM_IN,    "VZm+",      "Vertical Zoom In",  false);
  set(TB_VZOOM_OUT,   "VZm-",      "Vertical Zoom Out", false);
  set(TB_VZOOM_RESET, "VReset",    "Reset VZoom",       false);
}

void Toolbar::SetRect(int x, int y, int w, int h)
{
  m_rect = { x, y, x + w, y + h };
  RecalcLayout();
}

void Toolbar::RecalcLayout()
{
  int x = m_rect.left + BUTTON_PADDING;
  int y = m_rect.top + BUTTON_PADDING;
  int h = (m_rect.bottom - m_rect.top) - BUTTON_PADDING * 2;

  for (int i = 0; i < TB_COUNT; i++) {
    int w = m_items[i].isSeparator ? SEPARATOR_WIDTH : BUTTON_WIDTH;
    m_items[i].rect = { x, y, x + w, y + h };
    x += w + BUTTON_PADDING;
  }
}

void Toolbar::Paint(HDC hdc)
{
  if (!hdc) return;

  HBRUSH bgBrush = CreateSolidBrush(g_theme.toolbarBg);
  FillRect(hdc, &m_rect, bgBrush);
  DeleteObject(bgBrush);

  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_rect.left, m_rect.bottom - 1, nullptr);
  LineTo(hdc, m_rect.right, m_rect.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(hdc, TRANSPARENT);

  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.toolbar);

  for (int i = 0; i < TB_COUNT; i++) {
    RECT& r = m_items[i].rect;

    if (m_items[i].isSeparator) {
      HPEN sepPen = CreatePen(PS_SOLID, 1, g_theme.border);
      HPEN op = (HPEN)SelectObject(hdc, sepPen);
      int cx = (r.left + r.right) / 2;
      MoveToEx(hdc, cx, r.top + 4, nullptr);
      LineTo(hdc, cx, r.bottom - 4);
      SelectObject(hdc, op);
      DeleteObject(sepPen);
      continue;
    }

    // Always draw button background + border for visibility
    {
      COLORREF bgCol = (i == m_hoverIdx) ? g_theme.toolbarHover : g_theme.toolbarButtonBg;
      COLORREF borderCol = (i == m_hoverIdx) ? g_theme.toolbarText : g_theme.toolbarButtonBorder;
      HBRUSH btnBgBrush = CreateSolidBrush(bgCol);
      FillRect(hdc, &r, btnBgBrush);
      DeleteObject(btnBgBrush);
      HPEN btnPen = CreatePen(PS_SOLID, 1, borderCol);
      HPEN op = (HPEN)SelectObject(hdc, btnPen);
      MoveToEx(hdc, r.left, r.top, nullptr);
      LineTo(hdc, r.right - 1, r.top);
      LineTo(hdc, r.right - 1, r.bottom - 1);
      LineTo(hdc, r.left, r.bottom - 1);
      LineTo(hdc, r.left, r.top);
      SelectObject(hdc, op);
      DeleteObject(btnPen);
    }

    if (m_items[i].label) {
      SetTextColor(hdc, m_items[i].enabled ? g_theme.toolbarText : g_theme.toolbarDisabled);
      DrawText(hdc, m_items[i].label, -1, &r,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }

  SelectObject(hdc, oldFont);
}

int Toolbar::HitTest(int x, int y) const
{
  if (y < m_rect.top || y >= m_rect.bottom) return -1;
  for (int i = 0; i < TB_COUNT; i++) {
    if (m_items[i].isSeparator) continue;
    if (x >= m_items[i].rect.left && x < m_items[i].rect.right &&
        y >= m_items[i].rect.top && y < m_items[i].rect.bottom) {
      return m_items[i].enabled ? i : -1;
    }
  }
  return -1;
}
