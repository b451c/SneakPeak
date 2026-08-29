// waveform_view.h — Waveform display and interaction for SneakPeak
#pragma once

#include "platform.h"
#include "display_gain.h"
#include "config.h"
#include "globals.h"
#include "multi_item_view.h"
#include <vector>
#include <string>
#include <cmath>

class AudioStream;

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
  double playrate = 1.0;       // take D_PLAYRATE (take-envelope time = item time * playrate)
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
  void SetShowRMS(bool v) { m_showRMS = v; }
  bool GetShowRMS() const { return m_showRMS; }
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
  // Lanes (per Track): the lane under client y (-1 outside / other modes) and its own zoom
  int LaneAtY(int y) const { return m_multiItemActive ? m_multiItem.LaneAtY(m_rect, y) : -1; }
  void ZoomLane(int lane, float factor) { m_multiItem.ZoomLane(lane, factor); }
  void ResetVerticalZoom() { ZoomVertical(1.0f / m_verticalZoom); m_multiItem.ResetLaneZoom(); }

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
  void DrawPlayhead(HDC hdc);   // drawn by SneakPeak over the cached scene on every playback tick
  void Invalidate() { m_peaksValid = false; }
  bool PeaksValid() const { return m_peaksValid; }   // false = the next Paint recomputes (scene cache key)
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
  // Forget takes REAPER has freed (ValidatePtr2) before anything asks REAPER
  // about them; true when one was dropped. See SneakPeak::OnPaint.
  bool DropDeadTakes();
  void SetSnapToZero(bool snap) { m_snapToZero = snap; }
  bool GetSnapToZero() const { return m_snapToZero; }
  // Public wrapper for the loop-bracket drag (SnapToZeroCrossing is private).
  double SnapTimeToZeroCrossing(double time) const { return SnapToZeroCrossing(time); }

  // Loop Lab (v2.4 INC-A1): loop region in FRAMES, -1/-1 = none. Standalone
  // only; joins the SaveCurrentStandaloneState/Restore move pair like fades.
  bool HasLoop() const { return m_loopStartFrame >= 0 && m_loopEndFrame > m_loopStartFrame; }
  int  GetLoopStart() const { return m_loopStartFrame; }
  int  GetLoopEnd() const { return m_loopEndFrame; }
  void SetLoop(int startFrame, int endFrame) { m_loopStartFrame = startFrame; m_loopEndFrame = endFrame; }
  void ClearLoop() { m_loopStartFrame = -1; m_loopEndFrame = -1; }

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
    double envTime = 0.0;         // take-envelope time = (viewTime - segOffset) * playrate
    double playrate = 1.0;        // take D_PLAYRATE (envelope timebase scale)
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
  // What the columns are scaled by (item volume + the knob preview): the
  // spectrogram's colour offset follows it (s20).
  DisplayGain GetDisplayGain() const {
    return { m_fadeCache.itemVol, m_standaloneGain, m_standaloneGainStart, m_standaloneGainEnd };
  }

  // Channel active state (solo badges: both on by default; audio = take pan balance, host-side)
  bool IsChannelActive(int ch) const { return m_channelActive[ch]; } // ch: 0=L, 1=R
  void ResetChannelsActive() { m_channelActive[0] = m_channelActive[1] = true; } // re-arm badges (solo is per-take)
  int  ChannelButtonAt(int x, int y) const; // badge under (x, y): 0 / 1, or -1
  bool ClickChannelButton(int x, int y); // returns true if hit


  // External audio change detection (via AudioAccessor)
  bool CheckAudioChanged();
  void ReloadAfterExternalChange();

  // Audio data access (for destructive editing)
  std::vector<double>& GetAudioData() { return m_audioData; }
  const std::vector<double>& GetAudioData() const { return m_audioData; }
  int GetAudioSampleCount() const { return m_audioSampleCount; }
  int GetSampleRate() const { return m_sampleRate; }
  int GetSourceSampleRate() const { return m_sourceRate; }
  // Long items load DOWNSAMPLED (PlanRead's 10M-frame cap): such a buffer
  // must never be written back to the source (finding F6).
  bool IsItemBufferDownsampled() const {
    return !m_standaloneMode && m_take && m_sourceRate > 0 && m_sampleRate != m_sourceRate;
  }
  // 8g (design_lazy_buffer.md): a view whose buffer WOULD be downsampled never
  // decodes on select - display, exports and Dynamics do not need it - only when
  // a sample consumer asks (SneakPeak::RequireItemAudio). Multi-item stays eager.
  bool ItemBufferIsLazy() const;
  MediaItem_Take* GetTake() const { return m_take; }
  double GetTakeOffset() const { return m_takeOffset; }
  double GetTakePlayrate() const { return m_takePlayrate; }
  void SetTakePlayrate(double r) { m_takePlayrate = r; m_peaksValid = false; }   // A6.3: the take's rate changed under us

  // Update after destructive edit
  void SetAudioSampleCount(int count) { m_audioSampleCount = count; }
  void SetItemDuration(double dur) { m_itemDuration = dur; }
  void SetItemPosition(double pos) { m_itemPosition = pos; }

  // SDK-peaks hybrid (INC-PK1, .harness/design_sdk_peaks_hybrid.md):
  // single-item ITEM mode shows the waveform from REAPER's .reapeaks
  // immediately while the sample buffer loads in OnTimer slices. Loaded ==
  // m_audioSampleCount > 0; every sample consumer already no-ops on 0.
  bool IsItemAudioLoaded() const {
    if (m_standaloneMode) return true;
    if (m_multiItemActive) return m_multiItem.AllLayersLoaded();
    return m_audioSampleCount > 0;
  }
  // Bumped by every view (re)load; the background loader aborts a job set
  // whose generation no longer matches (phase 2a: one rule for all views).
  unsigned GetLoadGeneration() const { return m_loadGeneration; }
  MultiItemView& GetMultiItemViewMut() { return m_multiItem; }
  int GetPlannedFrames() const { return m_plannedFrames; }
  // Read plan shared by every loader (single, timeline, SET, multi-item):
  // the 10M-frame cap downsamples long spans instead of refusing them.
  // Largest sample buffer SneakPeak will hold for one view (doubles, all
  // channels): Edit Copy refuses items whose Standalone load would exceed it;
  // 8g applies the same rule to the working buffer.
  static constexpr int64_t kMaxBufferBytes = 1LL << 30;
  // Frames above which an item loads DOWNSAMPLED (PlanRead); One-Shot slices
  // longer than this are refused (a one-shot is not a 4-minute file).
  static constexpr int kMaxLoadFrames = 10000000;
  static bool PlanRead(double seconds, int srcRate, int& readRate, int& readFrames);
  // Full-rate chunked reads over [t0, t1) of view time (audio_stream.h,
  // view_stream.cpp): what the loader would build without the 10M-frame cap.
  bool OpenStream(AudioStream& stream, double t0, double t1, bool applyItemVolume) const;
  // 8g: one window of the item through the view's live take accessor (source
  // rate, display channels folded like the loader) for the level meter while a
  // lazy item has no buffer. Main thread only; false without an accessor.
  bool ReadLiveWindow(double t0, int frames, std::vector<double>& out) const;
  int GetSrcChannels() const { return m_srcChannels; }
  bool SdkPeaksPending() const { return m_sdkPeaksPending; }  // .reapeaks absent -> pump builder
  // The background loader's read plan == LoadAudioData's (incl. the 10M-frame
  // downsample cap). Returns false when there is nothing to load.
  bool ComputeItemLoadPlan(int& readRate, int& readFrames) const;
  // Install a finished background load (already channel-mode folded).
  void InstallItemAudio(std::vector<double>&& data, int frames, int rate, int nch);

private:
  void LoadAudioData();
  void LoadConcatenated(const std::vector<MediaItem*>& items);
  void UpdateFadeCacheMulti();
  void UpdateFadeCacheSingle();
  bool CompareFadeParams(const FadeCache& a, const FadeCache& b) const;
  void UpdatePeaks();
  void UpdatePeaksFromSDK();
  void UpdatePeaksFromSDKSegments(int w);   // timeline/SET: one fetch per visible segment
  void DrawWaveformChannel(HDC hdc, int channel, int yTop, int height);
  void DrawSelection(HDC hdc);
  void DrawCursor(HDC hdc);
  void DrawCenterLine(HDC hdc, int yCenter);
  void DrawDbGridLines(HDC hdc, int channel, int yTop, int height);
  void DrawTimeGrid(HDC hdc);
  void DrawDbScale(HDC hdc, int channel, int yTop, int height, float zoom, bool badge);
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
  bool m_showRMS = true;
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
  double m_takePlayrate = 1.0; // D_PLAYRATE of m_take (single-item envelope timebase)
  int m_numChannels = 0;
  int m_sampleRate = 44100;
  int m_sourceRate = 0;   // the take source's own rate (m_sampleRate may be the read rate)

  // Cached audio samples (loaded once per item; in single-item ITEM mode the
  // buffer arrives via the background loader - empty until then, display
  // served by UpdatePeaksFromSDK)
  std::vector<double> m_audioData;  // interleaved [sample * nch + ch]
  int m_audioSampleCount = 0;       // total frames loaded
  int m_srcChannels = 1;            // source channel count before I_CHANMODE fold
  bool m_sdkPeaksPending = false;   // last SDK fetch returned no peaks yet
  unsigned m_loadGeneration = 0;    // see GetLoadGeneration()
  int m_plannedFrames = 0;          // timeline/SET: frames the loader must deliver
  // Phase 2c: the last single-item buffer survives a deselect so re-clicking
  // the same (unchanged) take is instant instead of a full re-decode.
  struct RetainedAudio {
    MediaItem_Take* take = nullptr;
    AudioAccessor* accessor = nullptr;   // state check on reuse
    std::vector<double> data;
    int frames = 0, rate = 0, nch = 0, srcCh = 0;
    double duration = 0.0, offset = 0.0, playrate = 1.0;
    int chanMode = 0;
  };
  RetainedAudio m_retained;
  void RetainCurrentAudio();
public:
  // Destructive write-back: every accessor we hold on the take must be gone
  // before the file is replaced (tmp + rename = new inode; an open decoder keeps
  // serving the OLD inode, and REAPER pools decoders per path - finding F7).
  void ReleaseTakeAccessors();
  void RecreateLiveAccessor();
  AudioAccessor* GetLiveAccessor() const { return m_liveAccessor; }
private:
  bool ReuseRetainedAudio();
  void DropRetainedAudio();

  // View state
  double m_viewStartTime = 0.0;
  double m_viewDuration = 1.0;
  float m_verticalZoom = 1.0f;
  double m_cursorTime = 0.0;

  // Selection
  WaveformSelection m_selection;
  bool m_selecting = false;
  bool m_snapToZero = false;

  // Loop Lab region (v2.4 INC-A1), frames; -1 = none. Standalone-only state.
  int m_loopStartFrame = -1;
  int m_loopEndFrame = -1;

  // Peaks cache (computed from m_audioData, no API calls)
  std::vector<double> m_peakMax;
  std::vector<double> m_peakMin;
  std::vector<double> m_peakRMS;  // RMS per column per channel
  // Per-(column,channel) clip flags, laid out like the peak arrays (col*nch+ch).
  // bit0 = over 0 dBFS after item volume (a truthful WARNING in float modes;
  // in destructive standalone a save WILL clip). bit1 = source flat-top: >=3
  // sampled values at full scale in the RAW data = clipping that has already
  // happened in the file (independent of any gain applied on top).
  std::vector<unsigned char> m_clipFlags;
  bool m_peaksValid = false;
  bool m_peaksFromSdk = false;   // current peaks came from .reapeaks (no RMS / flat-top scan)
  double m_peaksCachedStart = 0.0;
  double m_peaksCachedDuration = 0.0;
  int m_peaksCachedWidth = 0;

  // Fade drag feedback
  int m_fadeDragType = 0;  // 0=none, 1=fadeIn, 2=fadeOut
  int m_fadeDragShape = 0;

  // Channel active (mute buttons)
  bool m_channelActive[2] = { true, true };

  // Volume envelope overlay
  bool m_envShowVolume = false;
  double m_envMaxGain = 2.0; // MAXVAL from envelope chunk (updated in DrawVolumeEnvelope)
  // MAXVAL cache key (profile 2026-07-09): GetEnvelopeStateChunk makes REAPER
  // serialize the WHOLE envelope (tens of thousands of points after Live
  // writes) - ~6 s of a 50 s slider drag went into re-reading this one static
  // value every paint. Re-query only when the envelope handle changes; reset
  // on SetItem/ClearItem. (A mid-session change of REAPER's envelope range
  // preference goes stale until the next item switch - acceptable.)
  TrackEnvelope* m_envMaxGainSrc = nullptr;
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
