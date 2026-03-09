// marker_manager.h — Marker/region drawing, hit-testing, and editing
#pragma once

#include "platform.h"

// Forward declarations
class WaveformView;

class MarkerManager {
public:
  MarkerManager() = default;

  // Drawing
  void DrawMarkers(HDC hdc, const RECT& waveformRect, const RECT& rulerRect, const WaveformView& wv);

  // Hit testing
  int HitTestMarker(int x, const WaveformView& wv, int tolerance = 5) const; // returns enum idx or -1

  // Marker operations
  void AddMarkerAtCursor(const WaveformView& wv);
  void AddRegionFromSelection(const WaveformView& wv);
  void DeleteMarkerByEnumIdx(int enumIdx);
  void EditMarkerDialog(int enumIdx);

  // Drag support
  void StartDrag(int enumIdx);
  void UpdateDrag(int x, const WaveformView& wv);
  void EndDrag();
  bool IsDragging() const { return m_markerDragging; }
  int GetDragEnumIdx() const { return m_dragMarkerEnumIdx; }

  // State
  bool m_showMarkers = true;
  int m_rightClickMarkerIdx = -1; // for context menu on marker

private:
  bool m_markerDragging = false;
  int m_dragMarkerEnumIdx = -1;  // enum index of dragged marker
};
