// gain_panel.h — Floating gain knob overlay (non-destructive, uses D_VOL)
#pragma once

#include "platform.h"
#include <vector>

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
  void ShowBatch(const std::vector<MediaItem*>& items); // multi-item batch mode
  void ShowStandalone(); // standalone mode: no item, internal gain only
  void Hide() { m_visible = false; m_item = nullptr; m_standalone = false; m_batchItems.clear(); m_batchOrigVols.clear(); }
  void Toggle(MediaItem* item);
  bool IsStandalone() const { return m_standalone; }
  bool IsBatch() const { return !m_batchItems.empty(); }

  // State
  double GetDb() const { return m_db; }
  void SetDb(double db);   // clamp and write to item
  void AdjustDb(double delta); // increment/decrement by delta dB
  bool IsDragging() const { return m_knobDragging || m_panelDragging; }
  bool IsPanelDragging() const { return m_panelDragging; }

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
  bool m_standalone = false;

  // Batch mode: multiple items, knob controls relative gain offset
  std::vector<MediaItem*> m_batchItems;
  std::vector<double> m_batchOrigVols; // original D_VOL per item (linear)

  // When true, skip batch D_VOL writes (visual preview + split on release instead)
  bool m_skipBatchWrite = false;
public:
  void SetSkipBatchWrite(bool skip) { m_skipBatchWrite = skip; }
private:

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
