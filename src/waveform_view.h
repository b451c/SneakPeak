// waveform_view.h — Waveform display and interaction for SneakPeak
#pragma once

#include "platform.h"
#include "config.h"
#include "globals.h"
#include "multi_item_view.h"
#include <vector>
#include <string>
#include <cmath>

struct WaveformSelection {
  double startTime = 0.0;
  double endTime = 0.0;
  bool active = false;
};

struct ItemSegment {
  MediaItem* item = nullptr;
  MediaItem_Take* take = nullptr;
  double position = 0.0;       // absolute timeline position
  double duration = 0.0;
  double relativeOffset = 0.0; // offset within concatenated view
  int audioStartFrame = 0;
  int audioFrameCount = 0;
};


class WaveformView {
public:
  WaveformView();
  ~WaveformView();

  // Item binding
  void SetItem(MediaItem* item);
  void SetItems(const std::vector<MediaItem*>& items);
  void ClearItem();
  bool HasItem() const { return m_item != nullptr || m_standaloneMode; }
  MediaItem* GetItem() const { return m_item; }
  bool IsMultiItem() const { return m_multiItemActive || m_trackViewActive || m_timelineViewActive || m_segments.size() > 1; }
  bool IsTimelineOrMultiItem() const { return m_timelineViewActive || m_multiItemActive; }
  const std::vector<ItemSegment>& GetSegments() const { return m_segments; }

  // Working set (items in a range on one track, gaps collapsed)
  void LoadItemsInRange(MediaTrack* track, double startPos, double endPos);
  void LoadItemsList(const std::vector<MediaItem*>& items);
  bool IsTrackView() const { return m_trackViewActive; }

  // Timeline view (sibling items with gaps preserved, 1:1 with REAPER timeline)
  void LoadTimelineView(const std::vector<MediaItem*>& items);
  bool IsTimelineView() const { return m_timelineViewActive; }
  double GetTimelineOrigin() const { return m_timelineOrigin; }
  const ItemSegment* GetSegmentAtTime(double relTime) const;
  void ScaleAudioBuffer(double factor); // multiply all audio samples in-place
  void ScaleAudioRange(double factor, double startTime, double endTime); // multiply range only

  // Multi-item view mode (Mix/Layered)
  bool IsMultiItemActive() const { return m_multiItemActive; }
  void SetMultiItemMode(MultiItemMode mode) { m_multiItem.SetMode(mode); m_peaksValid = false; }
  MultiItemMode GetMultiItemMode() const { return m_multiItem.GetMode(); }
  void SetShowJoinLines(bool show) { m_showJoinLines = show; }
  bool GetShowJoinLines() const { return m_showJoinLines; }
  const MultiItemView& GetMultiItemView() const { return m_multiItem; }
  void SetBatchGainOffset(double linearOffset);
  double GetBatchGainOffset() const { return m_batchGainOffset; }

  // Standalone file mode (no REAPER item)
  bool LoadFromFile(const std::string& path);
  bool IsStandaloneMode() const { return m_standaloneMode; }
  const std::string& GetStandaloneFilePath() const { return m_standaloneFilePath; }
  int GetStandaloneBitsPerSample() const { return m_standaloneBitsPerSample; }
  int GetStandaloneAudioFormat() const { return m_standaloneAudioFormat; }

  // Geometry
  void SetRect(int x, int y, int w, int h);
  RECT GetRect() const { return m_rect; }

  // View state
  double GetViewStart() const { return m_viewStartTime; }
  double GetViewEnd() const { return m_viewStartTime + m_viewDuration; }
  double GetViewDuration() const { return m_viewDuration; }
  double GetItemDuration() const { return m_itemDuration; }
  double GetItemPosition() const { return m_itemPosition; }
  int GetNumChannels() const { return m_numChannels; }
  float GetVerticalZoom() const { return m_verticalZoom; }

  // Navigation
  void ZoomHorizontal(double factor, double centerTime);
  void ZoomVertical(float factor);
  void ScrollH(double deltaTime);
  void ZoomToFit();
  void ZoomToSelection();

  // Selection
  void StartSelection(double time);
  void UpdateSelection(double time);
  void EndSelection();
  void ClearSelection();
  WaveformSelection GetSelection() const { return m_selection; }
  bool HasSelection() const { return m_selection.active && m_selection.startTime != m_selection.endTime; }

  // Coordinate conversion
  double XToTime(int x) const;
  int TimeToX(double time) const;

  // Multi-item: convert absolute timeline position to concatenated view time
  // Returns -1.0 if the position is not within any segment
  double AbsTimeToRelTime(double absTime) const;

  // Multi-item: convert concatenated view time back to absolute timeline position
  double RelTimeToAbsTime(double relTime) const;

  // Rendering (GDI)
  void Paint(HDC hdc);
  void Invalidate() { m_peaksValid = false; }
  void ReloadAudio();  // re-read samples from source (after normalize etc.)

  // State setters (for tab restore)
  void SetViewStart(double t) { m_viewStartTime = t; }
  void SetViewDuration(double d) { m_viewDuration = d; }
  void SetSelection(const WaveformSelection& sel) { m_selection = sel; }
  void RestoreFromMemory(const std::string& path, std::vector<double>&& audio,
                         int nch, int sr, int frames, int bps, int fmt, double dur);

  // Cursor
  void SetCursorTime(double time) { m_cursorTime = time; }
  double GetCursorTime() const { return m_cursorTime; }

  // Snap to zero-crossing
  void SetSnapToZero(bool snap) { m_snapToZero = snap; }
  bool GetSnapToZero() const { return m_snapToZero; }

  // Volume envelope overlay + editing
  bool GetShowVolumeEnvelope() const { return m_envShowVolume; }
  void SetShowVolumeEnvelope(bool show) { m_envShowVolume = show; m_peaksValid = false; }
  int HitTestEnvelopePoint(int x, int y, int hitRadius = 8) const;
  int EnvYToGainY(double gain, int scalingMode) const; // gain -> Y pixel (REAPER fader scale)
  double EnvPixelToGain(int y, int scalingMode) const; // Y pixel -> gain (REAPER fader scale)

  // Envelope bypass (A/B comparison - skip envGain in rendering)
  bool GetEnvBypassed() const { return m_envBypassed; }
  void SetEnvBypassed(bool v) { m_envBypassed = v; m_peaksValid = false; }

  // Dense envelope reveal range (for >100 points after Apply Dynamics)
  bool HasEnvRevealRange() const { return m_envRevealEnd > m_envRevealStart; }
  void SetEnvRevealRange(double start, double end) { m_envRevealStart = start; m_envRevealEnd = end; }
  void ClearEnvRevealRange() { m_envRevealStart = m_envRevealEnd = 0.0; }
  double GetEnvRevealStart() const { return m_envRevealStart; }
  double GetEnvRevealEnd() const { return m_envRevealEnd; }

  // Per-segment envelope lookup (foundation for timeline/SET envelope support)
  // Maps view-relative time to the correct segment's take envelope.
  // Returns nullptr env if in gap region or no envelope exists.
  struct EnvSegmentInfo {
    TrackEnvelope* env = nullptr; // envelope handle (nullptr = gap/no envelope)
    double envTime = 0.0;         // time relative to take start (for Envelope_Evaluate)
    int segmentIdx = -1;          // segment index (-1 = gap or single-item)
    MediaItem_Take* take = nullptr;
    int scalingMode = 0;          // cached GetEnvelopeScalingMode result
  };
  EnvSegmentInfo GetEnvelopeAtTime(double viewTime) const;

  // Fade drag feedback
  void SetFadeDragInfo(int dragType, int shape);

  // Cache fade parameters (avoid per-paint API calls)
  struct FadeCache {
    double fadeInLen = 0.0;
    double fadeOutLen = 0.0;
    int fadeInShape = 0;
    int fadeOutShape = 0;
    double fadeInDir = 0.0;   // curvature -1..1 (REAPER D_FADEINDIR)
    double fadeOutDir = 0.0;  // curvature -1..1 (REAPER D_FADEOUTDIR)
    double itemVol = 1.0;
  };
  bool UpdateFadeCache(); // returns true if volume/fade changed

  // Active fade parameters (reads from standalone or cache depending on mode)
  struct FadeParams {
    double fadeInLen = 0.0, fadeOutLen = 0.0;
    int fadeInShape = 0, fadeOutShape = 0;
    double fadeInDir = 0.0, fadeOutDir = 0.0;
  };
  FadeParams GetActiveFadeParams() const;
  FadeCache GetFadeCache() const { return m_fadeCache; }
  void SetItemVol(double vol) { m_fadeCache.itemVol = vol; }

  // Standalone fade preview (visual, applied during drag — baked on release)
  struct StandaloneFade {
    double fadeInLen = 0.0;
    double fadeOutLen = 0.0;
    int fadeInShape = 0;
    int fadeOutShape = 0;
    double fadeInDir = 0.0;   // curvature -1..1
    double fadeOutDir = 0.0;  // curvature -1..1
  };
  void SetStandaloneFade(const StandaloneFade& f) { m_standaloneFade = f; }
  StandaloneFade GetStandaloneFade() const { return m_standaloneFade; }
  void ClearStandaloneFade() { m_standaloneFade = {}; }
  bool HasStandaloneFade() const { return m_standaloneFade.fadeInLen > 0.001 || m_standaloneFade.fadeOutLen > 0.001; }

  // Standalone gain preview (visual only, applied per-column in draw)
  void SetStandaloneGain(double gainLinear, double selStart, double selEnd) {
    m_standaloneGain = gainLinear;
    m_standaloneGainStart = selStart;
    m_standaloneGainEnd = selEnd;
  }
  void ClearStandaloneGain() { m_standaloneGain = 1.0; m_standaloneGainStart = -1; m_standaloneGainEnd = -1; }

  // Channel active state (mute buttons: both on by default)
  bool IsChannelActive(int ch) const { return m_channelActive[ch]; } // ch: 0=L, 1=R
  int GetChanMode() const; // returns I_CHANMODE value based on active state
  bool ClickChannelButton(int x, int y); // returns true if hit


  // External audio change detection (via AudioAccessor)
  bool CheckAudioChanged();
  void ReloadAfterExternalChange();

  // Audio data access (for destructive editing)
  std::vector<double>& GetAudioData() { return m_audioData; }
  const std::vector<double>& GetAudioData() const { return m_audioData; }
  int GetAudioSampleCount() const { return m_audioSampleCount; }
  int GetSampleRate() const { return m_sampleRate; }
  MediaItem_Take* GetTake() const { return m_take; }
  double GetTakeOffset() const { return m_takeOffset; }

  // Update after destructive edit
  void SetAudioSampleCount(int count) { m_audioSampleCount = count; }
  void SetItemDuration(double dur) { m_itemDuration = dur; }
  void SetItemPosition(double pos) { m_itemPosition = pos; }

private:
  void LoadAudioData();
  void LoadConcatenated(const std::vector<MediaItem*>& items);
  void UpdateFadeCacheMulti();
  void UpdateFadeCacheSingle();
  bool CompareFadeParams(const FadeCache& a, const FadeCache& b) const;
  void UpdatePeaks();
  void DrawWaveformChannel(HDC hdc, int channel, int yTop, int height);
  void DrawSelection(HDC hdc);
  void DrawCursor(HDC hdc);
  void DrawCenterLine(HDC hdc, int yCenter);
  void DrawDbGridLines(HDC hdc, int channel, int yTop, int height);
  void DrawTimeGrid(HDC hdc);
  void DrawDbScale(HDC hdc, int channel, int yTop, int height);
  void DrawFadeBackground(HDC hdc);
  void DrawFadeEnvelope(HDC hdc);
  void DrawVolumeEnvelope(HDC hdc);
  void DrawStandaloneFadeHandles(HDC hdc);
  void DrawClipIndicators(HDC hdc);
  void DrawItemBoundaries(HDC hdc);
  double SnapToZeroCrossing(double time) const;

public:
  int GetChannelTop(int channel) const;
  int GetChannelHeight() const;

private:
  // Multi-item view
  MultiItemView m_multiItem;
  bool m_multiItemActive = false;
  bool m_showJoinLines = true;
  double m_batchGainOffset = 1.0; // visual gain multiplier for batch mode (linear)

  // Working set / track view (concatenated items, gaps collapsed)
  bool m_trackViewActive = false;

  // Timeline view (sibling items with gaps preserved)
  bool m_timelineViewActive = false;
  double m_timelineOrigin = 0.0; // absolute position of first item

  // Item data
  MediaItem* m_item = nullptr;
  MediaItem_Take* m_take = nullptr;
  std::vector<ItemSegment> m_segments;
  double m_itemPosition = 0.0;
  double m_itemDuration = 0.0;
  double m_takeOffset = 0.0;
  int m_numChannels = 0;
  int m_sampleRate = 44100;

  // Cached audio samples (loaded once per item)
  std::vector<double> m_audioData;  // interleaved [sample * nch + ch]
  int m_audioSampleCount = 0;       // total frames loaded

  // View state
  double m_viewStartTime = 0.0;
  double m_viewDuration = 1.0;
  float m_verticalZoom = 1.0f;
  double m_cursorTime = 0.0;

  // Selection
  WaveformSelection m_selection;
  bool m_selecting = false;
  bool m_snapToZero = false;

  // Peaks cache (computed from m_audioData, no API calls)
  std::vector<double> m_peakMax;
  std::vector<double> m_peakMin;
  std::vector<double> m_peakRMS;  // RMS per column per channel
  std::vector<int> m_clipColumns;  // column indices where signal clips
  bool m_peaksValid = false;
  double m_peaksCachedStart = 0.0;
  double m_peaksCachedDuration = 0.0;
  int m_peaksCachedWidth = 0;

  // Fade drag feedback
  int m_fadeDragType = 0;  // 0=none, 1=fadeIn, 2=fadeOut
  int m_fadeDragShape = 0;

  // Channel active (mute buttons)
  bool m_channelActive[2] = { true, true };

  // Volume envelope overlay
  bool m_envShowVolume = true;
  double m_envMaxGain = 2.0; // MAXVAL from envelope chunk (updated in DrawVolumeEnvelope)
  bool m_envBypassed = false;      // A/B: skip envGain in rendering
  double m_envRevealStart = 0.0; // reveal range for dense envelopes (time coords)
  double m_envRevealEnd = 0.0;   // both 0 = inactive

  // Cached fade/volume parameters
  FadeCache m_fadeCache;

  // Live audio accessor for change detection
  AudioAccessor* m_liveAccessor = nullptr;

  // Standalone file mode
  bool m_standaloneMode = false;
  std::string m_standaloneFilePath;
  int m_standaloneBitsPerSample = 16;
  int m_standaloneAudioFormat = 1;
  StandaloneFade m_standaloneFade;     // fade preview during drag
  double m_standaloneGain = 1.0;       // visual gain preview
  double m_standaloneGainStart = -1.0; // selection start (-1 = full file)
  double m_standaloneGainEnd = -1.0;   // selection end

  // Geometry
  RECT m_rect = {0, 0, 0, 0};
};
