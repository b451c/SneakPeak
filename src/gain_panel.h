// gain_panel.h — Floating gain slider overlay (non-destructive, uses D_VOL)
#pragma once

#include "platform.h"

class MediaItem;

class GainPanel {
public:
  void Draw(HDC hdc, RECT waveformRect);
  bool HitTest(int x, int y, RECT waveformRect) const;
  bool SliderHitTest(int x, int y, RECT waveformRect) const;
  RECT GetRect(RECT waveformRect) const;

  // Mouse interaction (Cmd/Ctrl = fine/precise mode)
  bool OnMouseDown(int x, int y, RECT waveformRect);
  void OnMouseMove(int x, int y, RECT waveformRect);
  void OnMouseUp();
  bool OnDoubleClick(int x, int y, RECT waveformRect);

  // Visibility — Show reads current D_VOL from item
  bool IsVisible() const { return m_visible; }
  void Show(MediaItem* item);
  void Hide() { m_visible = false; m_item = nullptr; }
  void Toggle(MediaItem* item);

  // State
  double GetDb() const { return m_db; }
  bool IsDragging() const { return m_sliderDragging || m_panelDragging; }

private:
  void ReadFromItem();
  void WriteToItem();
  void ResetTo0dB();
  double SliderXToDb(int x, RECT r) const;
  static bool IsFineMode();

  bool m_visible = false;
  bool m_sliderDragging = false;
  bool m_panelDragging = false;
  int m_dragOffsetX = 0;
  int m_dragOffsetY = 0;
  double m_db = 0.0;
  double m_dragAnchorDb = 0.0;  // dB at drag start (for fine mode)
  int m_dragAnchorX = 0;        // x at drag start (for fine mode)
  MediaItem* m_item = nullptr;

  // Panel position offset from default center position
  int m_offsetX = 0;
  int m_offsetY = 0;

  static constexpr double MIN_DB = -60.0;
  static constexpr double MAX_DB = 12.0;
  static const int PANEL_W = 200;
  static const int PANEL_H = 32;
};
