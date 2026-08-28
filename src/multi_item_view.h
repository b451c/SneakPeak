// multi_item_view.h — Multi-item mix/layered view for SneakPeak
#pragma once

#include "platform.h"
#include "globals.h"
#include <vector>
#include <cmath>

struct WaveformSelection;

enum class MultiItemMode { MIX, LAYERED, LAYERED_TRACKS };

struct ItemLayer {
  MediaItem* item = nullptr;
  MediaItem_Take* take = nullptr;
  double position = 0.0;       // absolute timeline position
  double duration = 0.0;       // full item length (not trimmed)
  double itemVol = 1.0;        // D_VOL baked into audio
  int numChannels = 1;
  int colorIndex = 0;          // index into kLayerColors (per-item or per-track)
  int trackColorIndex = 0;     // index into kLayerColors (per-track, for LAYERED_TRACKS)

  std::vector<double> audio;   // interleaved samples (full length)
  int audioFrameCount = 0;     // 0 until the background loader installs the layer
  int plannedFrames = 0;       // frames the loader must deliver (read rate)
  int audioStartFrame = 0;     // frame offset from timelineStart

  // Per-layer peaks (LAYERED mode only)
  std::vector<double> peakMax, peakMin, peakRMS;
};

// Layer colors for LAYERED mode (8 distinct hues)
static const COLORREF kLayerColors[] = {
  RGB(0, 200, 80),    // green
  RGB(0, 200, 200),   // cyan
  RGB(200, 0, 200),   // magenta
  RGB(200, 200, 0),   // yellow
  RGB(255, 140, 0),   // orange
  RGB(60, 120, 255),  // blue
  RGB(255, 120, 160), // pink
  RGB(120, 255, 0),   // lime
};
static const int kNumLayerColors = (int)(sizeof(kLayerColors) / sizeof(kLayerColors[0]));

class MultiItemView {
public:
  // Load items from REAPER selection. Returns false on failure (caller should fallback).
  // outChannels/outSampleRate are set on success.
  bool LoadItems(const std::vector<MediaItem*>& items,
                 int& outChannels, int& outSampleRate);

  // Compute peaks for MIX mode — writes into shared peak arrays (same as single-item)
  void UpdatePeaks(double viewStart, double viewDur, int width, int numChannels,
                   std::vector<double>& peakMax, std::vector<double>& peakMin,
                   std::vector<double>& peakRMS);

  // Draw all layers for LAYERED mode
  void DrawLayers(HDC hdc, RECT rect, int numChannels,
                  double viewStart, double viewDur, float verticalZoom,
                  const WaveformSelection& selection, double gainOffset = 1.0);

  void SetMode(MultiItemMode mode) { m_mode = mode; m_peaksValid = false; }
  MultiItemMode GetMode() const { return m_mode; }
  double GetTimelineStart() const { return m_timelineStart; }
  double GetTimelineEnd() const { return m_timelineEnd; }
  double GetTimelineDuration() const { return m_timelineEnd - m_timelineStart; }
  const std::vector<ItemLayer>& GetLayers() const { return m_layers; }
  // Fill buffer with mixed audio for a frame range (for metering)
  void GetMixedAudio(int startFrame, int endFrame, int numChannels,
                     std::vector<double>& out) const;

  void Invalidate() { m_peaksValid = false; }
  void ScaleLayerAudio(double factor); // multiply all layer audio in-place
  void ScaleLayerAudioRange(double factor, double startTime, double endTime, int sampleRate); // multiply range
  void Clear();

  // Check if any layer's volume changed since load — caller should reload if true
  bool CheckVolumeChanged() const;

  // Phase 2a background loading: LoadItems only PLANS the layers (.reapeaks
  // draw them meanwhile); SneakPeak's loader installs each layer's samples.
  void InstallLayerAudio(size_t idx, std::vector<double>&& audio, int frames, double bakedVol);
  bool AllLayersLoaded() const;
  void DropAudio();                  // forget samples, keep the plan (reload)
  int GetChannels() const { return m_channels; }
  int GetSampleRate() const { return m_sampleRate; }

private:
  void ComputeMixPeaks(double viewStart, double viewDur, int width, int numChannels,
                       std::vector<double>& peakMax, std::vector<double>& peakMin,
                       std::vector<double>& peakRMS);
  void ComputeLayeredPeaks(double viewStart, double viewDur, int width, int numChannels);
  double GetLayerSample(const ItemLayer& layer, int timelineFrame, int ch, int nch) const;
  void ComputeLayerPeaksFromSDK(ItemLayer& layer, double viewStart, double viewDur,
                                int width, int numChannels);

  std::vector<ItemLayer> m_layers;
  MultiItemMode m_mode = MultiItemMode::MIX;
  double m_timelineStart = 0.0;
  double m_timelineEnd = 0.0;
  int m_sampleRate = 44100;
  int m_channels = 1;               // common (upmixed) channel count
  bool m_peaksValid = false;
  double m_cachedViewStart = 0.0;
  double m_cachedViewDur = 0.0;
  int m_cachedWidth = 0;
};
