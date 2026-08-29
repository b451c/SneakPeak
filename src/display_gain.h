// display_gain.h — the gain the waveform columns are drawn with (item volume
// + the gain-knob preview); the spectrogram's colour offset follows it (s20).
#pragma once

struct DisplayGain {
  double itemVol = 1.0;
  double live = 1.0;          // knob preview (1.0 = none)
  double liveStart = -1.0;    // preview range in seconds (-1 = the whole view)
  double liveEnd = -1.0;
  bool operator==(const DisplayGain& o) const {
    return itemVol == o.itemVol && live == o.live && liveStart == o.liveStart && liveEnd == o.liveEnd;
  }
  double At(double t) const {
    double g = itemVol;
    if (live != 1.0 && (liveStart < 0.0 || (t >= liveStart && t < liveEnd))) g *= live;
    return g;
  }
};
