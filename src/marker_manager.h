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

  // State accessors
  bool GetShowMarkers() const { return m_showMarkers; }
  void SetShowMarkers(bool v) { m_showMarkers = v; }
  void ToggleShowMarkers() { m_showMarkers = !m_showMarkers; }
  int GetRightClickMarkerIdx() const { return m_rightClickMarkerIdx; }
  void SetRightClickMarkerIdx(int idx) { m_rightClickMarkerIdx = idx; }

private:
  bool m_showMarkers = true;
  int m_rightClickMarkerIdx = -1;
  bool m_markerDragging = false;
  int m_dragMarkerEnumIdx = -1;  // enum index of dragged marker
};
