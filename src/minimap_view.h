// minimap_view.h — Miniature overview of entire item waveform
#pragma once

#include "platform.h"
#include <vector>

class WaveformView;

class MinimapView {
public:
  void SetRect(int x, int y, int w, int h);
  RECT GetRect() const { return m_rect; }
  void Paint(HDC hdc, const WaveformView& wv);
  void ComputePeaks(const WaveformView& wv);
  void Invalidate() { m_peaksValid = false; }
  double XToTime(int x, double itemDuration) const;

private:
  RECT m_rect = {};
  std::vector<double> m_peakMax, m_peakMin;
  bool m_peaksValid = false;
  int m_cachedWidth = 0;
};
