// waveform_view.cpp — Waveform rendering using GDI
// Audio data is loaded once into memory, peaks computed from cache (zero API calls per paint)
#include "waveform_view.h"
#include "audio_stream.h"
#include <mutex>
#include "audio_engine.h"
#include "audio_ops.h"
#include "reaper_plugin.h"
#include "theme.h"
#include "debug.h"
#include <cstring>
#include <cstdio>

// ApplyFadeShape is now shared via audio_ops.h

// Take-envelope point times are item time * D_PLAYRATE (REAPER convention; SWS
// BR_Envelope does the same) - cached per take/segment for the view<->envelope
// time mapping (forum #107: envelope landed early on rate != 1.0 items).
static double TakePlayrate(MediaItem_Take* take)
{
  if (!take || !g_GetSetMediaItemTakeInfo) return 1.0;
  double* p = (double*)g_GetSetMediaItemTakeInfo(take, "D_PLAYRATE", nullptr);
  return (p && *p > 0.0) ? *p : 1.0;
}

WaveformView::WaveformView() {}
WaveformView::~WaveformView() {}

void WaveformView::SetItem(MediaItem* item)
{
  // Don't let single-item loads overwrite multi-item view
  // To go back to single item, caller must ClearItem() first
  if (m_segments.size() > 1) {
    DBG("[WaveformView] SetItem BLOCKED: multi-item active (%d segments)\n",
        (int)m_segments.size());
    return;
  }

  if (m_item == item && !m_standaloneMode) return;
  m_loadGeneration++;

  m_standaloneMode = false;
  m_standaloneFilePath.clear();
  m_standaloneGain = 1.0;
  m_standaloneGainStart = -1.0;
  m_standaloneGainEnd = -1.0;
  m_trackViewActive = false;
  m_item = item;
  m_take = nullptr;
  m_segments.clear();
  m_peaksValid = false;
  m_selection = {};
  m_audioData.clear();
  m_audioSampleCount = 0;
  m_envMaxGainSrc = nullptr;

  if (!item) return;

  m_take = g_GetActiveTake ? g_GetActiveTake(item) : nullptr;
  if (!m_take) { m_item = nullptr; return; }

  if (g_GetMediaItemInfo_Value) {
    m_itemPosition = g_GetMediaItemInfo_Value(item, "D_POSITION");
    m_itemDuration = g_GetMediaItemInfo_Value(item, "D_LENGTH");
  }

  if (g_GetSetMediaItemTakeInfo) {
    double* pOffset = (double*)g_GetSetMediaItemTakeInfo(m_take, "D_STARTOFFS", nullptr);
    if (pOffset) m_takeOffset = *pOffset;
  }
  m_takePlayrate = TakePlayrate(m_take);

  int srcChannels = 1;
  if (g_GetMediaItemTake_Source) {
    PCM_source* src = g_GetMediaItemTake_Source(m_take);
    if (src) {
      srcChannels = src->GetNumChannels();
      m_sourceRate = m_sampleRate = (int)src->GetSampleRate();
      if (srcChannels < 1) srcChannels = 1;
      if (srcChannels > 2) srcChannels = 2;
    }
  }
  m_numChannels = srcChannels;
  m_srcChannels = srcChannels;

  m_cursorTime = 0.0;

  // Phase 2c: same take, unchanged - reinstall the retained buffer, no load.
  if (ReuseRetainedAudio()) {
    UpdateFadeCache();
    m_viewStartTime = 0.0;
    m_viewDuration = m_itemDuration;
    return;
  }
  DropRetainedAudio();

  // SDK-peaks hybrid (INC-PK1): skip the synchronous full decode - the
  // waveform draws from REAPER's .reapeaks immediately and the sample buffer
  // arrives via SneakPeak's background loader (StepItemAudioLoad). Display
  // channel count still reflects I_CHANMODE; the loader folds the buffer to
  // match on install.
  if (g_GetMediaItemTake_Peaks) {
    if (g_GetSetMediaItemTakeInfo && srcChannels == 2) {
      int* pChanMode = (int*)g_GetSetMediaItemTakeInfo(m_take, "I_CHANMODE", nullptr);
      int chanMode = pChanMode ? *pChanMode : 0;
      if (chanMode >= 2 && chanMode <= 4) m_numChannels = 1;
    }
    if (g_CreateTakeAudioAccessor)
      { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); m_liveAccessor = g_CreateTakeAudioAccessor(m_take); }
    UpdateFadeCache();  // first paint needs D_VOL/fades
    m_viewStartTime = 0.0;
    m_viewDuration = m_itemDuration;
    DBG("[SneakPeak] SetItem (SDK peaks): pos=%.3f dur=%.3f srcCh=%d dispCh=%d sr=%d\n",
        m_itemPosition, m_itemDuration, m_srcChannels, m_numChannels, m_sampleRate);
    return;
  }

  // Legacy path (SDK peaks API unavailable): load all audio synchronously
  LoadAudioData();

  // Create live accessor for external change detection
  if (m_take && g_CreateTakeAudioAccessor) {
    std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
    m_liveAccessor = g_CreateTakeAudioAccessor(m_take);
  }

  // Apply take channel mode (mono downmix) after loading
  if (g_GetSetMediaItemTakeInfo && m_take && srcChannels == 2 &&
      (int)m_audioData.size() >= m_audioSampleCount * 2) {
    int* pChanMode = (int*)g_GetSetMediaItemTakeInfo(m_take, "I_CHANMODE", nullptr);
    int chanMode = pChanMode ? *pChanMode : 0;
    if (chanMode == 2) {
      // Mono downmix: average L+R
      int frames = m_audioSampleCount;
      std::vector<double> mono(frames);
      for (int i = 0; i < frames; i++) {
        mono[i] = (m_audioData[i * 2] + m_audioData[i * 2 + 1]) * 0.5;
      }
      m_audioData = std::move(mono);
      m_numChannels = 1;
    } else if (chanMode == 3) {
      // Mono (L only)
      int frames = m_audioSampleCount;
      std::vector<double> mono(frames);
      for (int i = 0; i < frames; i++) {
        mono[i] = m_audioData[i * 2];
      }
      m_audioData = std::move(mono);
      m_numChannels = 1;
    } else if (chanMode == 4) {
      // Mono (R only)
      int frames = m_audioSampleCount;
      std::vector<double> mono(frames);
      for (int i = 0; i < frames; i++) {
        mono[i] = m_audioData[i * 2 + 1];
      }
      m_audioData = std::move(mono);
      m_numChannels = 1;
    }
  }

  m_viewStartTime = 0.0;
  m_viewDuration = m_itemDuration;

  DBG("[SneakPeak] SetItem: pos=%.3f dur=%.3f offset=%.3f ch=%d sr=%d samples=%d\n",
      m_itemPosition, m_itemDuration, m_takeOffset, m_numChannels, m_sampleRate, m_audioSampleCount);
}

void WaveformView::SetItems(const std::vector<MediaItem*>& items)
{
  m_loadGeneration++;
  if (items.size() <= 1) {
    if (!items.empty()) SetItem(items[0]);
    return;
  }

  m_standaloneMode = false;
  m_standaloneFilePath.clear();
  m_standaloneGain = 1.0;
  m_standaloneGainStart = -1.0;
  m_standaloneGainEnd = -1.0;
  m_trackViewActive = false;
  m_item = items[0];
  m_take = nullptr;
  m_peaksValid = false;
  m_selection = {};
  m_audioData.clear();
  m_audioSampleCount = 0;
  m_segments.clear();
  m_multiItemActive = false;

  // Try new multi-item view (absolute timeline, mix/layered)
  int outCh = 0, outSr = 0;
  if (m_multiItem.LoadItems(items, outCh, outSr)) {
    m_multiItemActive = true;
    m_numChannels = outCh;
    m_sampleRate = outSr;
    m_itemPosition = m_multiItem.GetTimelineStart();
    m_itemDuration = m_multiItem.GetTimelineDuration();
    m_takeOffset = 0.0;
    m_viewStartTime = 0.0;
    m_viewDuration = m_itemDuration;
    m_cursorTime = 0.0;

    // Set m_take to first item's take for compatibility
    if (g_GetActiveTake) m_take = g_GetActiveTake(items[0]);
    m_takePlayrate = TakePlayrate(m_take);

    // Build segments for compatibility (AbsTimeToRelTime etc.)
    for (const auto& layer : m_multiItem.GetLayers()) {
      ItemSegment seg;
      seg.item = layer.item;
      seg.take = layer.take;
      seg.position = layer.position;
      seg.duration = layer.duration;
      seg.relativeOffset = layer.position - m_itemPosition;
      seg.playrate = TakePlayrate(layer.take);
      seg.audioStartFrame = layer.audioStartFrame;
      seg.audioFrameCount = layer.audioFrameCount;
      m_segments.push_back(seg);
    }

    DBG("[SneakPeak] SetItems (multi-item): %d layers, timeline=%.3f-%.3f dur=%.3f\n",
        (int)m_multiItem.GetLayers().size(), m_itemPosition,
        m_itemPosition + m_itemDuration, m_itemDuration);
    return;
  }

  // Fallback: concatenation with gaps collapsed
  DBG("[SneakPeak] SetItems: MultiItemView failed, falling back to concat\n");
  m_multiItem.Clear();
  LoadConcatenated(items);
}

void WaveformView::LoadConcatenated(const std::vector<MediaItem*>& items)
{
  if (items.empty()) return;
  m_loadGeneration++;
  if (!g_GetActiveTake || !g_GetMediaItemInfo_Value || !g_GetMediaItemTake_Source) return;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return;

  m_take = g_GetActiveTake(items[0]);
  if (!m_take) { m_item = nullptr; return; }
  m_takePlayrate = TakePlayrate(m_take);
  PCM_source* src0 = g_GetMediaItemTake_Source(m_take);
  if (!src0) { m_item = nullptr; m_take = nullptr; return; }
  m_sourceRate = m_sampleRate = (int)src0->GetSampleRate();
  m_numChannels = src0->GetNumChannels();
  if (m_numChannels < 1) m_numChannels = 1;
  if (m_numChannels > 2) m_numChannels = 2;

  m_itemPosition = g_GetMediaItemInfo_Value(items[0], "D_POSITION");

  struct ItemInfo { MediaItem* item; double pos; double dur; };
  std::vector<ItemInfo> itemInfos;
  for (MediaItem* it : items) {
    double p = g_GetMediaItemInfo_Value(it, "D_POSITION");
    double d = g_GetMediaItemInfo_Value(it, "D_LENGTH");
    itemInfos.push_back({it, p, d});
  }

  m_audioData.clear();
  m_audioSampleCount = 0;
  m_segments.clear();

  {
    double spanSec = 0.0;
    for (const auto& ii : itemInfos) spanSec += ii.dur;
    int readRate = 0, readFrames = 0;
    if (PlanRead(spanSec, m_sampleRate, readRate, readFrames)) m_sampleRate = readRate;
  }
  m_srcChannels = m_numChannels;

  double totalDuration = 0.0;
  for (size_t idx = 0; idx < itemInfos.size(); idx++) {
    MediaItem* item = itemInfos[idx].item;
    MediaItem_Take* take = g_GetActiveTake(item);
    if (!take) continue;

    double pos = itemInfos[idx].pos;
    double dur = itemInfos[idx].dur;

    double effectiveDur = dur;
    if (idx + 1 < itemInfos.size()) {
      double nextPos = itemInfos[idx + 1].pos;
      if (nextPos < pos + dur) {
        effectiveDur = nextPos - pos;
        if (effectiveDur <= 0.0) continue;
      }
    }

    ItemSegment seg;
    seg.item = item;
    seg.take = take;
    seg.position = pos;
    seg.duration = effectiveDur;
    seg.relativeOffset = totalDuration;
    seg.playrate = TakePlayrate(take);
    seg.audioStartFrame = m_audioSampleCount;

    int frames = (int)(effectiveDur * (double)m_sampleRate);
    if (frames <= 0) continue;
    // Phase 2a: no decode here - the background loader fills the buffer
    // segment by segment (D_VOL baked there); .reapeaks serve the display.
    m_audioSampleCount += frames;

    seg.audioFrameCount = frames;
    totalDuration += effectiveDur;
    m_segments.push_back(seg);
  }
  m_plannedFrames = m_audioSampleCount;   // loader target (see PlanRead)
  m_audioSampleCount = 0;                  // buffer arrives from the loader
  m_audioData.clear();

  m_itemDuration = totalDuration;
  m_takeOffset = 0.0;
  m_viewStartTime = 0.0;
  m_viewDuration = totalDuration;
  m_cursorTime = 0.0;
}

void WaveformView::LoadItemsInRange(MediaTrack* track, double startPos, double endPos)
{
  if (!track || !g_GetTrackNumMediaItems || !g_GetTrackMediaItem || !g_GetMediaItemInfo_Value)
    return;

  int count = g_GetTrackNumMediaItems(track);
  if (count <= 0) return;

  std::vector<MediaItem*> items;
  for (int i = 0; i < count; i++) {
    MediaItem* mi = g_GetTrackMediaItem(track, i);
    if (!mi) continue;
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    double len = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
    if (pos + len > startPos && pos < endPos)
      items.push_back(mi);
  }
  if (items.empty()) return;

  LoadItemsList(items);

  DBG("[SneakPeak] LoadItemsInRange: %d items in range [%.3f-%.3f], duration=%.3f\n",
      (int)items.size(), startPos, endPos, m_itemDuration);
}

void WaveformView::LoadItemsList(const std::vector<MediaItem*>& items)
{
  if (items.empty()) return;

  std::vector<MediaItem*> sorted = items;
  std::sort(sorted.begin(), sorted.end(), [](MediaItem* a, MediaItem* b) {
    return g_GetMediaItemInfo_Value(a, "D_POSITION") < g_GetMediaItemInfo_Value(b, "D_POSITION");
  });

  m_item = sorted[0];
  m_multiItemActive = false;
  m_trackViewActive = true;

  LoadConcatenated(sorted);
  m_peaksValid = false;

  DBG("[SneakPeak] LoadItemsList: %d items, duration=%.3f\n",
      (int)sorted.size(), m_itemDuration);
}

void WaveformView::LoadTimelineView(const std::vector<MediaItem*>& items)
{
  if (items.empty()) return;
  m_loadGeneration++;
  if (!g_GetActiveTake || !g_GetMediaItemInfo_Value || !g_GetMediaItemTake_Source) return;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return;

  // Sort by position
  std::vector<MediaItem*> sorted = items;
  std::sort(sorted.begin(), sorted.end(), [](MediaItem* a, MediaItem* b) {
    return g_GetMediaItemInfo_Value(a, "D_POSITION") < g_GetMediaItemInfo_Value(b, "D_POSITION");
  });

  MediaItem_Take* firstTake = g_GetActiveTake(sorted[0]);
  if (!firstTake) return;
  PCM_source* src = g_GetMediaItemTake_Source(firstTake);
  if (!src) return;

  m_sourceRate = m_sampleRate = (int)src->GetSampleRate();
  m_numChannels = src->GetNumChannels();
  if (m_numChannels < 1) m_numChannels = 1;
  if (m_numChannels > 2) m_numChannels = 2;

  double firstPos = g_GetMediaItemInfo_Value(sorted.front(), "D_POSITION");
  double lastEnd = 0.0;
  for (auto* it : sorted) {
    double e = g_GetMediaItemInfo_Value(it, "D_POSITION") + g_GetMediaItemInfo_Value(it, "D_LENGTH");
    if (e > lastEnd) lastEnd = e;
  }
  double totalSpan = lastEnd - firstPos;

  // Sanity: refuse absurd gaps (span >> content); long spans are fine -
  // PlanRead downsamples them like every other view (the old 600 s cap
  // silently fell back to a single-item/multi reload after edits on long
  // items - forum-visible on the 17-min file).
  double totalItemDur = 0.0;
  for (auto* it : sorted) totalItemDur += g_GetMediaItemInfo_Value(it, "D_LENGTH");
  if (totalSpan > totalItemDur * 10.0) return; // fallback

  int readRate = 0, totalFrames = 0;
  if (!PlanRead(totalSpan, m_sampleRate, readRate, totalFrames)) return;
  m_sampleRate = readRate;
  m_srcChannels = m_numChannels;

  // Phase 2a: segments are planned here, decoded by the background loader
  // (silence gaps + D_VOL baked there); the display uses .reapeaks meanwhile.
  m_audioData.clear();
  m_segments.clear();

  for (auto* it : sorted) {
    MediaItem_Take* take = g_GetActiveTake(it);
    if (!take) continue;

    double pos = g_GetMediaItemInfo_Value(it, "D_POSITION");
    double dur = g_GetMediaItemInfo_Value(it, "D_LENGTH");
    double relOff = pos - firstPos;
    int startFrame = (int)(relOff * m_sampleRate);
    int frames = (int)(dur * m_sampleRate);
    if (startFrame < 0 || startFrame + frames > totalFrames) continue;

    ItemSegment seg;
    seg.item = it;
    seg.take = take;
    seg.position = pos;
    seg.duration = dur;
    seg.relativeOffset = relOff;
    seg.playrate = TakePlayrate(take);
    seg.audioStartFrame = startFrame;
    seg.audioFrameCount = frames;
    m_segments.push_back(seg);
  }

  m_item = sorted[0];
  m_take = firstTake;
  m_takePlayrate = TakePlayrate(m_take);
  m_itemPosition = firstPos;
  m_itemDuration = totalSpan;
  m_plannedFrames = totalFrames;   // loader target
  m_audioSampleCount = 0;          // buffer arrives from the loader
  m_takeOffset = 0.0;
  m_timelineViewActive = true;
  m_timelineOrigin = firstPos;
  m_trackViewActive = false;
  m_multiItemActive = false;
  m_peaksValid = false;

  DBG("[SneakPeak] LoadTimelineView: %d items, span=%.3f, origin=%.3f\n",
      (int)sorted.size(), totalSpan, firstPos);
}

void WaveformView::ScaleAudioBuffer(double factor)
{
  if (factor == 1.0) return;
  if (m_multiItemActive) {
    m_multiItem.ScaleLayerAudio(factor);
  } else if (!m_audioData.empty()) {
    for (size_t i = 0; i < m_audioData.size(); i++)
      m_audioData[i] *= factor;
  }
  m_peaksValid = false;
}

void WaveformView::ScaleAudioRange(double factor, double startTime, double endTime)
{
  if (factor == 1.0 || startTime >= endTime) return;
  if (m_multiItemActive) {
    m_multiItem.ScaleLayerAudioRange(factor, startTime, endTime, m_sampleRate);
  } else if (!m_audioData.empty()) {
    int f0 = std::max(0, (int)(startTime * m_sampleRate));
    int f1 = std::min(m_audioSampleCount, (int)(endTime * m_sampleRate));
    for (int f = f0; f < f1; f++) {
      for (int ch = 0; ch < m_numChannels; ch++)
        m_audioData[(size_t)f * m_numChannels + ch] *= factor;
    }
  }
  m_peaksValid = false;
}

const ItemSegment* WaveformView::GetSegmentAtTime(double relTime) const
{
  for (const auto& seg : m_segments) {
    if (relTime >= seg.relativeOffset && relTime < seg.relativeOffset + seg.duration)
      return &seg;
  }
  return nullptr;
}

void WaveformView::ClearItem()
{
  m_loadGeneration++;
  RetainCurrentAudio();
  if (m_liveAccessor && g_DestroyAudioAccessor) {
    std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
    g_DestroyAudioAccessor(m_liveAccessor);
    m_liveAccessor = nullptr;
  }
  m_item = nullptr;
  m_take = nullptr;
  m_takePlayrate = 1.0;
  m_segments.clear();
  m_multiItemActive = false;
  m_trackViewActive = false;
  m_timelineViewActive = false;
  m_timelineOrigin = 0.0;
  m_multiItem.Clear();
  m_batchGainOffset = 1.0;
  m_peaksValid = false;
  m_envMaxGainSrc = nullptr;
  ClearEnvRevealRange();
  m_selection = {};
  m_numChannels = 0;
  m_itemDuration = 0.0;
  m_audioData.clear();
  m_audioSampleCount = 0;
  m_standaloneMode = false;
  m_standaloneFilePath.clear();
  m_standaloneGain = 1.0;
  m_standaloneGainStart = -1.0;
  m_standaloneGainEnd = -1.0;
  m_fadeCache = {};
  m_fadeCache.itemVol = 1.0;
}

bool WaveformView::CheckAudioChanged()
{
  if (!m_liveAccessor || !g_AudioAccessorStateChanged) return false;
  std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
  return g_AudioAccessorStateChanged(m_liveAccessor);
}

void WaveformView::ReloadAfterExternalChange()
{
  // Validate take pointer before reload (may be dangling after undo)
  if (m_take && g_ValidatePtr2 && !g_ValidatePtr2(nullptr, m_take, "MediaItem_Take*")) {
    m_take = nullptr;
    return;
  }
  if (m_liveAccessor && g_AudioAccessorValidateState) {
    std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
    g_AudioAccessorValidateState(m_liveAccessor);
  }
  // SDK-peaks hybrid: drop the stale buffer and let the caller restart the
  // background loader - display falls back to .reapeaks meanwhile (which
  // REAPER refreshes itself on source changes). No code depends on the
  // buffer being back synchronously here (unlike post-edit ReloadAudio).
  if (!m_standaloneMode) {
    m_audioData.clear();
    m_audioSampleCount = 0;
    m_multiItem.DropAudio();
    m_loadGeneration++;
  } else if (m_audioSampleCount > 0) {
    LoadAudioData();
  }
  m_peaksValid = false;
}

bool WaveformView::LoadFromFile(const std::string& path)
{
  ClearItem();

  WavInfo info;
  if (!AudioEngine::ReadAudioFile(path, info, m_audioData)) {
    DBG("[SneakPeak] LoadFromFile failed: %s\n", path.c_str());
    return false;
  }

  m_standaloneMode = true;
  m_standaloneFilePath = path;
  m_standaloneBitsPerSample = info.bitsPerSample;
  m_standaloneAudioFormat = info.audioFormat;
  m_sampleRate = info.sampleRate;
  m_numChannels = info.numChannels;
  if (m_numChannels < 1) m_numChannels = 1;
  if (m_numChannels > 2) m_numChannels = 2;
  m_sourceRate = m_sampleRate;       // the buffer IS the file: the info line reads these,
  m_srcChannels = m_numChannels;     // never the last item's (stale otherwise)
  m_audioSampleCount = info.numFrames;
  m_itemDuration = (double)info.numFrames / (double)m_sampleRate;
  m_itemPosition = 0.0;
  m_takeOffset = 0.0;
  m_viewStartTime = 0.0;
  m_viewDuration = m_itemDuration;
  m_cursorTime = 0.0;
  m_peaksValid = false;

  DBG("[SneakPeak] LoadFromFile: %s (%d frames, %dch, %dHz, %.3fs)\n",
      path.c_str(), info.numFrames, m_numChannels, m_sampleRate, m_itemDuration);
  return true;
}

void WaveformView::RestoreFromMemory(const std::string& path, std::vector<double>&& audio,
                                     int nch, int sr, int frames, int bps, int fmt, double dur)
{
  ClearItem();

  m_standaloneMode = true;
  m_standaloneFilePath = path;
  m_standaloneBitsPerSample = bps;
  m_standaloneAudioFormat = fmt;
  m_sampleRate = sr;
  m_numChannels = nch;
  if (m_numChannels < 1) m_numChannels = 1;
  if (m_numChannels > 2) m_numChannels = 2;
  m_sourceRate = m_sampleRate;
  m_srcChannels = m_numChannels;
  m_audioSampleCount = frames;
  m_audioData = std::move(audio);
  m_itemDuration = dur;
  m_itemPosition = 0.0;
  m_takeOffset = 0.0;
  m_peaksValid = false;

  DBG("[SneakPeak] RestoreFromMemory: %s (%d frames, %dch, %dHz, %.3fs)\n",
      path.c_str(), frames, m_numChannels, m_sampleRate, m_itemDuration);
}

// Load ALL audio samples into memory — called once per item
// Shared read plan for the synchronous loader below and the background item
// loader (SneakPeak::StepItemAudioLoad) - single source of truth, including
// the 10M-frame downsample cap for very long items.
bool WaveformView::PlanRead(double seconds, int srcRate, int& readRate, int& readFrames)
{
  if (seconds <= 0.0 || srcRate <= 0) return false;
  int totalFrames = (int)(seconds * (double)srcRate);
  // Safety cap: 30 minutes of stereo 96kHz = ~346M samples, ~2.6GB
  // For practical use, cap at 10M frames (~3.5 min stereo 48kHz)
  // Beyond that, we'll downsample on load
  const int MAX_FRAMES = kMaxLoadFrames;

  readRate = srcRate;
  readFrames = totalFrames;

  // If too many frames, read at lower rate to keep memory sane
  if (totalFrames > MAX_FRAMES) {
    int ratio = (totalFrames + MAX_FRAMES - 1) / MAX_FRAMES;
    readRate = srcRate / ratio;
    if (readRate < 8000) readRate = 8000;
    readFrames = (int)(seconds * (double)readRate) + 1;
  }
  return readFrames > 0;
}

bool WaveformView::ComputeItemLoadPlan(int& readRate, int& readFrames) const
{
  if (!m_take) return false;
  return PlanRead(m_itemDuration, m_sampleRate, readRate, readFrames);
}

// StateChanged under the accessor lock (A8.1).
static bool AccessorChanged(AudioAccessor* a)
{
  if (!a || !g_AudioAccessorStateChanged) return false;
  std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
  return g_AudioAccessorStateChanged(a);
}

bool WaveformView::ReadLiveWindow(double t0, int frames, std::vector<double>& out) const
{
  if (!m_liveAccessor || m_standaloneMode || !m_take || m_sourceRate <= 0 || frames <= 0 ||
      !g_GetAudioAccessorSamples) return false;
  const int srcNch = std::max(1, m_srcChannels);
  out.assign((size_t)frames * (size_t)srcNch, 0.0);
  {
    std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
    if (g_GetAudioAccessorSamples(m_liveAccessor, m_sourceRate, srcNch, t0, frames, out.data()) < 0)
      return false;
  }
  const int chanMode = TakeChanMode(m_take);
  if (FoldedChannels(srcNch, chanMode) != srcNch) {   // audio_stream.h: loader parity
    FoldChanMode(out.data(), frames, chanMode);
    out.resize((size_t)frames);
  }
  return true;
}

// A script can delete an item without an undo point, so REAPER's project-state
// counter never moves and SneakPeak::PollProjectState does not rebuild the
// view: the segment stayed and the next paint asked REAPER for the envelope
// of a freed take (GetTakeEnvelopeByName crashed once the memory was reused).
// Only the take is forgotten - the item stays so SegmentsMatchProject sees
// the mismatch and rebuilds the view.
bool WaveformView::DropDeadTakes()
{
  if (m_standaloneMode || !g_ValidatePtr2) return false;
  auto dead = [](MediaItem_Take* take) {
    return take && !g_ValidatePtr2(nullptr, (void*)take, "MediaItem_Take*");
  };
  bool dropped = false;
  for (auto& seg : m_segments)
    if (dead(seg.take)) { seg.take = nullptr; dropped = true; }
  if (m_multiItemActive && m_multiItem.DropDeadTakes(dead)) dropped = true;
  if (dead(m_take)) { m_take = nullptr; dropped = true; }
  return dropped;
}

bool WaveformView::ItemBufferIsLazy() const
{
  if (m_standaloneMode || m_multiItemActive || !m_take || m_sourceRate <= 0) return false;
  if (m_sampleRate != m_sourceRate) return true;   // installed, or planned (timeline/SET)
  int readRate = 0, readFrames = 0;                // single item: planned on install only
  return ComputeItemLoadPlan(readRate, readFrames) && readRate != m_sourceRate;
}

// Install a finished background item load (buffer already channel-folded).
void WaveformView::InstallItemAudio(std::vector<double>&& data, int frames, int rate, int nch)
{
  if (m_standaloneMode || !m_take) return;
  m_audioData = std::move(data);
  m_audioSampleCount = frames;
  m_sampleRate = rate;   // may be the downsampled read rate (see the cap above)
  m_numChannels = (nch < 1) ? 1 : nch;
  m_peaksValid = false;  // switch display to the audio path
  DBG("[SneakPeak] InstallItemAudio: %d frames (%d ch) @ %d Hz, %.1f MB\n",
      frames, m_numChannels, rate,
      (double)(m_audioData.size() * sizeof(double)) / (1024.0 * 1024.0));
}

void WaveformView::LoadAudioData()
{
  m_audioData.clear();
  m_audioSampleCount = 0;

  if (!m_take || m_itemDuration <= 0.0) return;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return;

  int nch = m_numChannels;
  if (nch < 1) nch = 1;

  int readRate = 0, readFrames = 0;
  if (!ComputeItemLoadPlan(readRate, readFrames)) return;
  if (readRate != m_sampleRate) {
    m_sampleRate = readRate; // update so peak calculations use correct rate
    DBG("[SneakPeak] Downsampling: readRate=%d readFrames=%d\n", readRate, readFrames);
  }

  AudioAccessor* accessor = nullptr;
  { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); accessor = g_CreateTakeAudioAccessor(m_take); }
  if (!accessor) return;

  m_audioData.resize((size_t)readFrames * nch, 0.0);

  // Read in chunks of 64k frames to avoid huge single API calls
  static const int CHUNK_FRAMES = 65536;
  int framesRead = 0;

  while (framesRead < readFrames) {
    int framesThisChunk = std::min(CHUNK_FRAMES, readFrames - framesRead);
    double chunkTime = (double)framesRead / (double)readRate;

    int ret;
    {
      std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
      ret = g_GetAudioAccessorSamples(accessor, readRate, nch,
                                      chunkTime, framesThisChunk,
                                      m_audioData.data() + (size_t)framesRead * nch);
    }
    if (ret <= 0) {
      // Zero out remaining
      size_t offset = (size_t)framesRead * nch;
      std::fill(m_audioData.begin() + offset, m_audioData.end(), 0.0);
      break;
    }

    framesRead += framesThisChunk;
  }

  m_audioSampleCount = readFrames;
  { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); g_DestroyAudioAccessor(accessor); }

  DBG("[SneakPeak] Loaded %d frames (%d ch) into %.1f MB\n",
      m_audioSampleCount, nch, (double)(m_audioData.size() * sizeof(double)) / (1024.0 * 1024.0));
}

void WaveformView::ReloadAudio()
{
  m_peaksValid = false;
  // ITEM views: drop the buffer and bump the generation - the background
  // loader (OnTimer pump) re-plans and refills it; .reapeaks serve meanwhile.
  if (!m_standaloneMode) {
    m_audioData.clear();
    m_audioSampleCount = 0;
    m_multiItem.DropAudio();
    m_loadGeneration++;
    return;
  }
  LoadAudioData();
}

void WaveformView::SetRect(int x, int y, int w, int h)
{
  if (m_rect.left != x || m_rect.top != y ||
      (m_rect.right - m_rect.left) != w || (m_rect.bottom - m_rect.top) != h) {
    m_peaksValid = false;
  }
  m_rect.left = x;
  m_rect.top = y;
  m_rect.right = x + w;
  m_rect.bottom = y + h;
}

int WaveformView::GetChannelTop(int channel) const
{
  int chH = GetChannelHeight();
  return m_rect.top + channel * (chH + SP(CHANNEL_SEPARATOR_HEIGHT));
}

int WaveformView::GetChannelHeight() const
{
  int totalH = m_rect.bottom - m_rect.top;
  if (m_numChannels <= 1) return totalH;
  return (totalH - SP(CHANNEL_SEPARATOR_HEIGHT) * (m_numChannels - 1)) / m_numChannels;
}

// --- Navigation ---

void WaveformView::ZoomHorizontal(double factor, double centerTime)
{
  double newDuration = m_viewDuration / factor;
  double minDur = 32.0 / (double)m_sampleRate;
  double maxDur = m_itemDuration;
  if (maxDur < 0.1) maxDur = 0.1;
  newDuration = std::max(minDur, std::min(maxDur, newDuration));

  double ratio = (centerTime - m_viewStartTime) / m_viewDuration;
  m_viewStartTime = centerTime - ratio * newDuration;
  m_viewDuration = newDuration;

  if (m_viewStartTime < 0.0)
    m_viewStartTime = 0.0;
  if (m_viewStartTime + m_viewDuration > m_itemDuration)
    m_viewStartTime = std::max(0.0, m_itemDuration - m_viewDuration);

  m_peaksValid = false;
}

void WaveformView::ZoomVertical(float factor)
{
  m_verticalZoom *= factor;
  m_verticalZoom = std::max(MIN_VERTICAL_ZOOM, std::min(MAX_VERTICAL_ZOOM, m_verticalZoom));
}

void WaveformView::ScrollH(double deltaTime)
{
  m_viewStartTime += deltaTime;
  double maxStart = std::max(0.0, m_itemDuration - m_viewDuration);
  m_viewStartTime = std::max(0.0, std::min(maxStart, m_viewStartTime));
  m_peaksValid = false;
}

void WaveformView::ZoomToFit()
{
  m_viewStartTime = 0.0;
  m_viewDuration = m_itemDuration;
  if (m_viewDuration < 0.001) m_viewDuration = 0.001;
  m_peaksValid = false;
}

void WaveformView::ZoomToSelection()
{
  if (!HasSelection()) return;
  double s = std::min(m_selection.startTime, m_selection.endTime);
  double e = std::max(m_selection.startTime, m_selection.endTime);
  double dur = e - s;
  if (dur < 0.0001) return;
  m_viewStartTime = s - dur * 0.05;
  m_viewDuration = dur * 1.1;
  m_peaksValid = false;
}

// --- Selection ---

void WaveformView::StartSelection(double time) {
  m_selection.startTime = time;
  m_selection.endTime = time;
  m_selection.active = true;
  m_selecting = true;
}
void WaveformView::UpdateSelection(double time) { if (m_selecting) m_selection.endTime = time; }
void WaveformView::EndSelection() {
  m_selecting = false;
  if (m_selection.startTime > m_selection.endTime) std::swap(m_selection.startTime, m_selection.endTime);
  if (m_snapToZero && m_numChannels > 0) {
    m_selection.startTime = SnapToZeroCrossing(m_selection.startTime);
    m_selection.endTime = SnapToZeroCrossing(m_selection.endTime);
  }
}

// Nearest sign change on channel 0 within +-ZERO_SNAP_RANGE of `frame`
// (`frame` itself when there is none).
static int NearestZeroCrossing(const double* data, int frames, int nch, int frame)
{
  frame = std::max(0, std::min(frame, frames - 1));
  int bestDist = ZERO_SNAP_RANGE + 1;
  int bestFrame = frame;
  int searchRange = std::min(ZERO_SNAP_RANGE, frames - 1);

  for (int d = 0; d <= searchRange; d++) {
    for (int sign = -1; sign <= 1; sign += 2) {
      int f = frame + sign * d;
      if (f < 0 || f >= frames - 1) continue;
      double s0 = data[(size_t)f * nch];
      double s1 = data[(size_t)(f + 1) * nch];
      if (s0 * s1 <= 0.0) {  // zero crossing (or exact zero)
        if (d < bestDist) {
          bestDist = d;
          bestFrame = f;
        }
      }
    }
    if (bestDist <= d) break;  // found closest
  }
  return bestFrame;
}

double WaveformView::SnapToZeroCrossing(double time) const {
  // A long single item has no working buffer (lazy, 8g) or one at a reduced
  // rate: look the crossing up in a window read from the source at its own
  // rate instead (A6.7).
  if (!m_standaloneMode && m_take && !m_multiItemActive && m_segments.size() <= 1 &&
      m_sourceRate > 0 && (m_audioSampleCount <= 0 || IsItemBufferDownsampled())) {
    const int span = 2 * ZERO_SNAP_RANGE + 2;
    // window start on the source's sample grid, so the result is a source sample
    const double t0 = std::max(0.0, std::floor(time * (double)m_sourceRate - (double)(ZERO_SNAP_RANGE + 1)) /
                                        (double)m_sourceRate);
    std::vector<double> win;
    if (!ReadLiveWindow(t0, span, win) || win.size() < (size_t)span) return time;
    const int nch = (int)(win.size() / (size_t)span);
    const int f = NearestZeroCrossing(win.data(), span, nch, (int)((time - t0) * (double)m_sourceRate + 0.5));
    return t0 + (double)f / (double)m_sourceRate;
  }

  if (m_audioData.empty() || m_audioSampleCount < 2 || m_numChannels <= 0 || m_sampleRate <= 0) return time;
  const int f = NearestZeroCrossing(m_audioData.data(), m_audioSampleCount, m_numChannels,
                                    (int)(time * (double)m_sampleRate));
  return (double)f / (double)m_sampleRate;
}

void WaveformView::ClearSelection() { m_selection = {}; m_selecting = false; }

void WaveformView::SetBatchGainOffset(double linearOffset)
{
  if (linearOffset != m_batchGainOffset) {
    m_batchGainOffset = linearOffset;
    m_peaksValid = false;
    m_multiItem.Invalidate();
  }
}

// --- Coordinate conversion ---

double WaveformView::XToTime(int x) const {
  int w = m_rect.right - m_rect.left - SP(DB_SCALE_WIDTH);
  if (w < 1) w = m_rect.right - m_rect.left;
  if (w <= 0) return m_viewStartTime;
  return m_viewStartTime + ((double)(x - m_rect.left) / (double)w) * m_viewDuration;
}

int WaveformView::TimeToX(double time) const {
  int w = m_rect.right - m_rect.left - SP(DB_SCALE_WIDTH);
  if (w < 1) w = m_rect.right - m_rect.left;
  if (m_viewDuration <= 0.0) return m_rect.left;
  return m_rect.left + (int)(((time - m_viewStartTime) / m_viewDuration) * (double)w);
}

double WaveformView::AbsTimeToRelTime(double absTime) const
{
  // Timeline view: simple offset from origin
  if (m_timelineViewActive) return absTime - m_timelineOrigin;

  // Multi-item active: absolute timeline, simple offset
  if (m_multiItemActive || m_segments.size() <= 1) {
    return absTime - m_itemPosition;
  }

  // Legacy concatenated multi-item: find which segment the absolute time falls into
  for (size_t i = 0; i < m_segments.size(); i++) {
    const auto& seg = m_segments[i];
    if (absTime >= seg.position && absTime < seg.position + seg.duration) {
      double timeInSeg = absTime - seg.position;
      return seg.relativeOffset + timeInSeg;
    }
    if (i + 1 < m_segments.size()) {
      double segEnd = seg.position + seg.duration;
      if (absTime >= segEnd && absTime < m_segments[i + 1].position) {
        return seg.relativeOffset + seg.duration;
      }
    }
  }

  if (!m_segments.empty() && absTime < m_segments[0].position) return 0.0;
  if (!m_segments.empty()) {
    const auto& last = m_segments.back();
    if (absTime >= last.position + last.duration)
      return last.relativeOffset + last.duration;
  }
  return 0.0;
}

double WaveformView::RelTimeToAbsTime(double relTime) const
{
  // Timeline view: simple offset from origin
  if (m_timelineViewActive) return m_timelineOrigin + relTime;

  // Multi-item active: absolute timeline, simple offset
  if (m_multiItemActive || m_segments.size() <= 1) {
    return m_itemPosition + relTime;
  }

  // Concatenated segments: map through segment boundaries
  for (size_t i = 0; i < m_segments.size(); i++) {
    const auto& seg = m_segments[i];
    double segEnd = seg.relativeOffset + seg.duration;

    if (relTime >= seg.relativeOffset && relTime < segEnd) {
      double timeInSeg = relTime - seg.relativeOffset;
      return seg.position + timeInSeg;
    }

    // At exact boundary: map to start of next segment (not end of current)
    if (i + 1 < m_segments.size() && std::abs(relTime - segEnd) < 0.0001) {
      return m_segments[i + 1].position;
    }
  }

  if (!m_segments.empty()) {
    const auto& last = m_segments.back();
    return last.position + last.duration;
  }
  return m_itemPosition + relTime;
}

// --- Peaks from cached audio data (pure math, no API calls) ---

// --- Phase 2c: retained single-item buffer across deselect/reselect ---------

void WaveformView::RetainCurrentAudio()
{
  // Only a loaded single-item ITEM view is worth keeping (segment/multi/
  // standalone buffers are rebuilt from their own plans).
  if (m_standaloneMode || m_multiItemActive || m_segments.size() > 1 ||
      !m_take || m_audioSampleCount <= 0 || m_audioData.empty()) return;
  DropRetainedAudio();
  m_retained.take = m_take;
  m_retained.accessor = m_liveAccessor;   // ownership moves to the cache
  m_liveAccessor = nullptr;
  m_retained.data = std::move(m_audioData);
  m_audioData.clear();
  m_retained.frames = m_audioSampleCount;
  m_retained.rate = m_sampleRate;
  m_retained.nch = m_numChannels;
  m_retained.srcCh = m_srcChannels;
  m_retained.duration = m_itemDuration;
  m_retained.offset = m_takeOffset;
  m_retained.playrate = m_takePlayrate;
  m_retained.chanMode = TakeChanMode(m_take);
  DBG("[SneakPeak] RetainCurrentAudio: %d frames kept for take %p\n", m_retained.frames, (void*)m_take);
}

bool WaveformView::ReuseRetainedAudio()
{
  if (!m_retained.take || m_retained.take != m_take || m_retained.frames <= 0) return false;
  if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)m_take, "MediaItem_Take*")) return false;
  if (fabs(m_retained.duration - m_itemDuration) > 1e-9 ||
      fabs(m_retained.offset - m_takeOffset) > 1e-9 ||
      fabs(m_retained.playrate - m_takePlayrate) > 1e-9 ||
      m_retained.chanMode != TakeChanMode(m_take)) return false;
  if (m_retained.accessor && g_AudioAccessorStateChanged &&
      AccessorChanged(m_retained.accessor)) return false;

  m_audioData = std::move(m_retained.data);
  m_audioSampleCount = m_retained.frames;
  m_sampleRate = m_retained.rate;
  m_numChannels = m_retained.nch;
  m_srcChannels = m_retained.srcCh;
  m_liveAccessor = m_retained.accessor;   // ownership back to the view
  m_retained.accessor = nullptr;
  m_retained = RetainedAudio();
  m_peaksValid = false;
  DBG("[SneakPeak] ReuseRetainedAudio: %d frames reinstalled\n", m_audioSampleCount);
  return true;
}

void WaveformView::DropRetainedAudio()
{
  if (m_retained.accessor && g_DestroyAudioAccessor) { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); g_DestroyAudioAccessor(m_retained.accessor); }
  m_retained = RetainedAudio();
}

void WaveformView::ReleaseTakeAccessors()
{
  if (m_liveAccessor && g_DestroyAudioAccessor) { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); g_DestroyAudioAccessor(m_liveAccessor); }
  m_liveAccessor = nullptr;
  DropRetainedAudio();
}

void WaveformView::RecreateLiveAccessor()
{
  if (m_liveAccessor || !m_take || m_standaloneMode || !g_CreateTakeAudioAccessor) return;
  if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)m_take, "MediaItem_Take*")) return;
  { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); m_liveAccessor = g_CreateTakeAudioAccessor(m_take); }
}
