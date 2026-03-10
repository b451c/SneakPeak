// levels_panel.h — RMS/Peak level meter for EditView
#pragma once

#include "platform.h"
#include <vector>

class LevelsPanel {
public:
  void Update(const std::vector<double>& audio, int startFrame,
              int endFrame, int sampleRate, int nch, double itemVol, bool playing,
              const bool* channelActive = nullptr);
  void Draw(HDC hdc, RECT rect, int nch);
  bool IsDecaying() const { return m_rmsL > -59.0 || m_rmsR > -59.0 || m_peakHoldL > -59.0 || m_peakHoldR > -59.0; }

private:
  double m_rmsL = -60.0, m_rmsR = -60.0;
  double m_peakL = -60.0, m_peakR = -60.0;
  double m_peakHoldL = -60.0, m_peakHoldR = -60.0;
  int m_peakHoldCountL = 0, m_peakHoldCountR = 0;
  bool m_wasPlaying = false;
  static constexpr int PEAK_HOLD_TICKS = 30; // ~1s at 33ms
  static constexpr double DECAY_RATE = 3.0;  // dB per tick
};
