// gain_panel.h — Floating gain knob overlay (non-destructive, uses D_VOL)
#pragma once

#include "platform.h"

class MediaItem;

class GainPanel {
public:
  void Draw(HDC hdc, RECT waveformRect);
  bool HitTest(int x, int y, RECT waveformRect) const;
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
  bool IsDragging() const { return m_knobDragging || m_panelDragging; }

private:
  void ReadFromItem();
  void WriteToItem();
  void ResetTo0dB();
  static bool IsFineMode();

  // Knob angle helpers
  double DbToAngle(double db) const;
  double AngleToDb(double angle) const;

  bool m_visible = false;
  bool m_knobDragging = false;
  bool m_panelDragging = false;
  int m_dragOffsetX = 0;
  int m_dragOffsetY = 0;
  double m_db = 0.0;
  double m_dragAnchorDb = 0.0;
  int m_dragAnchorY = 0;
  MediaItem* m_item = nullptr;

  // Panel position offset from default center position
  int m_offsetX = 0;
  int m_offsetY = 0;

  static constexpr double MIN_DB = -60.0;
  static constexpr double MAX_DB = 12.0;
  static const int PANEL_W = 110;
  static const int PANEL_H = 32;
  static const int KNOB_RADIUS = 10;

  // Knob arc: 7 o'clock (225°) to 5 o'clock (-45° / 315°)
  static constexpr double ARC_START = 225.0; // degrees, bottom-left
  static constexpr double ARC_END = -45.0;   // degrees, bottom-right (= 315)
  static constexpr double ARC_RANGE = 270.0; // total sweep
};
