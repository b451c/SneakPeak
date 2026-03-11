// levels_panel.h — Peak/RMS/VU level meter for SneakPeak
#pragma once

#include "platform.h"
#include <vector>

enum class MeterMode { PEAK, RMS, VU };

struct MeterGdiCache {
  HBRUSH bgBrush     = nullptr;
  HBRUSH barBgBrush  = nullptr;
  HBRUSH greenBrush  = nullptr;
  HBRUSH yellowBrush = nullptr;
  HBRUSH redBrush    = nullptr;
  HPEN   tickPen     = nullptr;
  HPEN   scalePen    = nullptr;
  HPEN   peakGreen   = nullptr;
  HPEN   peakYellow  = nullptr;
  HPEN   peakRed     = nullptr;
  MeterMode mode     = MeterMode::PEAK;
  bool valid         = false;
};

class LevelsPanel {
public:
  ~LevelsPanel();

  void Update(const std::vector<double>& audio, int startFrame,
              int endFrame, int sampleRate, int nch, double itemVol, bool playing,
              const bool* channelActive = nullptr);
  void Draw(HDC hdc, RECT rect, int nch);
  bool IsDecaying() const { return m_barL > -59.0 || m_barR > -59.0 || m_peakHoldL > -59.0 || m_peakHoldR > -59.0; }

  void SetMode(MeterMode mode) { m_mode = mode; DestroyGdiCache(); }
  MeterMode GetMode() const { return m_mode; }
  const char* GetModeLabel() const;

  // Integration window half-size in samples (caller uses this to size the audio window)
  int GetIntegrationHalfWindow(int sampleRate) const;

private:
  void CreateGdiCache();
  void DestroyGdiCache();

  MeterMode m_mode = MeterMode::PEAK;
  double m_barL = -60.0, m_barR = -60.0;   // main bar (peak/rms/vu depending on mode)
  double m_peakL = -60.0, m_peakR = -60.0;
  double m_peakHoldL = -60.0, m_peakHoldR = -60.0;
  int m_peakHoldCountL = 0, m_peakHoldCountR = 0;
  bool m_wasPlaying = false;
  MeterGdiCache m_gdi;
  static constexpr int PEAK_HOLD_TICKS = 30; // ~1s at 33ms
};
