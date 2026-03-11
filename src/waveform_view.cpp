// waveform_view.cpp — Waveform rendering using GDI
// Audio data is loaded once into memory, peaks computed from cache (zero API calls per paint)
#include "waveform_view.h"
#include "audio_engine.h"
#include "audio_ops.h"
#include "reaper_plugin.h"
#include "theme.h"
#include "debug.h"
#include <cstring>
#include <cstdio>

// ApplyFadeShape is now shared via audio_ops.h

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

  m_standaloneMode = false;
  m_standaloneFilePath.clear();
  m_standaloneGain = 1.0;
  m_standaloneGainStart = -1.0;
  m_standaloneGainEnd = -1.0;
  m_item = item;
  m_take = nullptr;
  m_segments.clear();
  m_peaksValid = false;
  m_selection = {};
  m_audioData.clear();
  m_audioSampleCount = 0;

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

  int srcChannels = 1;
  if (g_GetMediaItemTake_Source) {
    PCM_source* src = g_GetMediaItemTake_Source(m_take);
    if (src) {
      srcChannels = src->GetNumChannels();
      m_sampleRate = (int)src->GetSampleRate();
      if (srcChannels < 1) srcChannels = 1;
      if (srcChannels > 2) srcChannels = 2;
    }
  }
  m_numChannels = srcChannels;

  m_viewStartTime = 0.0;
  m_viewDuration = m_itemDuration;
  m_cursorTime = 0.0;

  // Load all audio samples at source channel count
  LoadAudioData();

  // Create live accessor for external change detection
  if (m_take && g_CreateTakeAudioAccessor) {
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

  DBG("[SneakPeak] SetItem: pos=%.3f dur=%.3f offset=%.3f ch=%d sr=%d samples=%d\n",
      m_itemPosition, m_itemDuration, m_takeOffset, m_numChannels, m_sampleRate, m_audioSampleCount);
}

void WaveformView::SetItems(const std::vector<MediaItem*>& items)
{
  if (items.size() <= 1) {
    if (!items.empty()) SetItem(items[0]);
    return;
  }

  m_standaloneMode = false;
  m_standaloneFilePath.clear();
  m_standaloneGain = 1.0;
  m_standaloneGainStart = -1.0;
  m_standaloneGainEnd = -1.0;
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

    // Build segments for compatibility (AbsTimeToRelTime etc.)
    for (const auto& layer : m_multiItem.GetLayers()) {
      ItemSegment seg;
      seg.item = layer.item;
      seg.take = layer.take;
      seg.position = layer.position;
      seg.duration = layer.duration;
      seg.relativeOffset = layer.position - m_itemPosition;
      seg.audioStartFrame = layer.audioStartFrame;
      seg.audioFrameCount = layer.audioFrameCount;
      m_segments.push_back(seg);
    }

    DBG("[SneakPeak] SetItems (multi-item): %d layers, timeline=%.3f-%.3f dur=%.3f\n",
        (int)m_multiItem.GetLayers().size(), m_itemPosition,
        m_itemPosition + m_itemDuration, m_itemDuration);
    return;
  }

  // Fallback: old concatenation code (shouldn't normally get here)
  DBG("[SneakPeak] SetItems: MultiItemView failed, falling back to concat\n");
  m_multiItem.Clear();

  if (!g_GetActiveTake || !g_GetMediaItemInfo_Value || !g_GetMediaItemTake_Source) return;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return;

  m_take = g_GetActiveTake(items[0]);
  if (!m_take) { m_item = nullptr; return; }
  PCM_source* src0 = g_GetMediaItemTake_Source(m_take);
  if (!src0) { m_item = nullptr; m_take = nullptr; return; }
  m_sampleRate = (int)src0->GetSampleRate();
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
    seg.audioStartFrame = m_audioSampleCount;

    int frames = (int)(effectiveDur * (double)m_sampleRate);
    if (frames <= 0) continue;

    AudioAccessor* accessor = g_CreateTakeAudioAccessor(take);
    if (!accessor) continue;

    size_t prevSize = m_audioData.size();
    m_audioData.resize(prevSize + (size_t)frames * m_numChannels, 0.0);

    static const int CHUNK_FRAMES = 65536;
    int framesLoaded = 0;
    while (framesLoaded < frames) {
      int chunk = std::min(CHUNK_FRAMES, frames - framesLoaded);
      double chunkTime = (double)framesLoaded / (double)m_sampleRate;
      g_GetAudioAccessorSamples(accessor, m_sampleRate, m_numChannels,
                                chunkTime, chunk,
                                m_audioData.data() + prevSize + (size_t)framesLoaded * m_numChannels);
      framesLoaded += chunk;
    }
    g_DestroyAudioAccessor(accessor);

    double itemVol = g_GetMediaItemInfo_Value(item, "D_VOL");
    // Take volume (the handle in arrange view)
    if (g_GetSetMediaItemTakeInfo && take) {
      double* pTakeVol = (double*)g_GetSetMediaItemTakeInfo(take, "D_VOL", nullptr);
      if (pTakeVol) itemVol *= *pTakeVol;
    }
    if (itemVol != 1.0 && itemVol > 0.0) {
      size_t sampleCount = (size_t)frames * m_numChannels;
      for (size_t s = 0; s < sampleCount; s++)
        m_audioData[prevSize + s] *= itemVol;
    }

    seg.audioFrameCount = frames;
    m_audioSampleCount += frames;
    totalDuration += effectiveDur;
    m_segments.push_back(seg);
  }

  m_itemDuration = totalDuration;
  m_takeOffset = 0.0;
  m_viewStartTime = 0.0;
  m_viewDuration = totalDuration;
  m_cursorTime = 0.0;
}

void WaveformView::ClearItem()
{
  if (m_liveAccessor && g_DestroyAudioAccessor) {
    g_DestroyAudioAccessor(m_liveAccessor);
    m_liveAccessor = nullptr;
  }
  m_item = nullptr;
  m_take = nullptr;
  m_segments.clear();
  m_multiItemActive = false;
  m_multiItem.Clear();
  m_batchGainOffset = 1.0;
  m_peaksValid = false;
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
  return g_AudioAccessorStateChanged(m_liveAccessor);
}

void WaveformView::ReloadAfterExternalChange()
{
  if (m_liveAccessor && g_AudioAccessorValidateState)
    g_AudioAccessorValidateState(m_liveAccessor);
  LoadAudioData();
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
void WaveformView::LoadAudioData()
{
  m_audioData.clear();
  m_audioSampleCount = 0;

  if (!m_take || m_itemDuration <= 0.0) return;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return;

  int nch = m_numChannels;
  if (nch < 1) nch = 1;

  int totalFrames = (int)(m_itemDuration * (double)m_sampleRate);
  // Safety cap: 30 minutes of stereo 96kHz = ~346M samples, ~2.6GB
  // For practical use, cap at 10M frames (~3.5 min stereo 48kHz)
  // Beyond that, we'll downsample on load
  static const int MAX_FRAMES = 10000000;

  int readRate = m_sampleRate;
  int readFrames = totalFrames;

  // If too many frames, read at lower rate to keep memory sane
  if (totalFrames > MAX_FRAMES) {
    // Downsample ratio
    int ratio = (totalFrames + MAX_FRAMES - 1) / MAX_FRAMES;
    readRate = m_sampleRate / ratio;
    if (readRate < 8000) readRate = 8000;
    readFrames = (int)(m_itemDuration * (double)readRate) + 1;
    m_sampleRate = readRate; // update so peak calculations use correct rate
    DBG("[SneakPeak] Downsampling: ratio=%d readRate=%d readFrames=%d\n", ratio, readRate, readFrames);
  }

  AudioAccessor* accessor = g_CreateTakeAudioAccessor(m_take);
  if (!accessor) return;

  m_audioData.resize((size_t)readFrames * nch, 0.0);

  // Read in chunks of 64k frames to avoid huge single API calls
  static const int CHUNK_FRAMES = 65536;
  int framesRead = 0;

  while (framesRead < readFrames) {
    int framesThisChunk = std::min(CHUNK_FRAMES, readFrames - framesRead);
    double chunkTime = (double)framesRead / (double)readRate;

    int ret = g_GetAudioAccessorSamples(accessor, readRate, nch,
                                         chunkTime, framesThisChunk,
                                         m_audioData.data() + (size_t)framesRead * nch);
    if (ret <= 0) {
      // Zero out remaining
      size_t offset = (size_t)framesRead * nch;
      std::fill(m_audioData.begin() + offset, m_audioData.end(), 0.0);
      break;
    }

    framesRead += framesThisChunk;
  }

  m_audioSampleCount = readFrames;
  g_DestroyAudioAccessor(accessor);

  DBG("[SneakPeak] Loaded %d frames (%d ch) into %.1f MB\n",
      m_audioSampleCount, nch, (double)(m_audioData.size() * sizeof(double)) / (1024.0 * 1024.0));
}

void WaveformView::ReloadAudio()
{
  m_peaksValid = false;
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
  return m_rect.top + channel * (chH + CHANNEL_SEPARATOR_HEIGHT);
}

int WaveformView::GetChannelHeight() const
{
  int totalH = m_rect.bottom - m_rect.top;
  if (m_numChannels <= 1) return totalH;
  return (totalH - CHANNEL_SEPARATOR_HEIGHT * (m_numChannels - 1)) / m_numChannels;
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

  // Clamp to item bounds — no empty space beyond item
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
  double minStart = 0.0;
  double maxStart = std::max(0.0, m_itemDuration - m_viewDuration);
  m_viewStartTime = std::max(minStart, std::min(maxStart, m_viewStartTime));
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
  if (m_snapToZero && m_audioSampleCount > 0 && m_numChannels > 0) {
    m_selection.startTime = SnapToZeroCrossing(m_selection.startTime);
    m_selection.endTime = SnapToZeroCrossing(m_selection.endTime);
  }
}

double WaveformView::SnapToZeroCrossing(double time) const {
  if (m_audioData.empty() || m_audioSampleCount < 2 || m_numChannels <= 0 || m_sampleRate <= 0) return time;

  int frame = (int)(time * (double)m_sampleRate);
  frame = std::max(0, std::min(frame, m_audioSampleCount - 1));

  // Search outward ±ZERO_SNAP_RANGE samples on channel 0 for sign change
  int bestDist = ZERO_SNAP_RANGE + 1;
  int bestFrame = frame;
  int searchRange = std::min(ZERO_SNAP_RANGE, m_audioSampleCount - 1);

  for (int d = 0; d <= searchRange; d++) {
    for (int sign = -1; sign <= 1; sign += 2) {
      int f = frame + sign * d;
      if (f < 0 || f >= m_audioSampleCount - 1) continue;
      double s0 = m_audioData[(size_t)f * m_numChannels];
      double s1 = m_audioData[(size_t)(f + 1) * m_numChannels];
      if (s0 * s1 <= 0.0) {  // zero crossing (or exact zero)
        if (d < bestDist) {
          bestDist = d;
          bestFrame = f;
        }
      }
    }
    if (bestDist <= d) break;  // found closest
  }

  return (double)bestFrame / (double)m_sampleRate;
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
  int w = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  if (w < 1) w = m_rect.right - m_rect.left;
  if (w <= 0) return m_viewStartTime;
  return m_viewStartTime + ((double)(x - m_rect.left) / (double)w) * m_viewDuration;
}

int WaveformView::TimeToX(double time) const {
  int w = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  if (w < 1) w = m_rect.right - m_rect.left;
  if (m_viewDuration <= 0.0) return m_rect.left;
  return m_rect.left + (int)(((time - m_viewStartTime) / m_viewDuration) * (double)w);
}

double WaveformView::AbsTimeToRelTime(double absTime) const
{
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
  // Multi-item active: absolute timeline, simple offset
  if (m_multiItemActive || m_segments.size() <= 1) {
    return m_itemPosition + relTime;
  }

  // Legacy concatenated multi-item
  for (const auto& seg : m_segments) {
    if (relTime >= seg.relativeOffset && relTime < seg.relativeOffset + seg.duration) {
      double timeInSeg = relTime - seg.relativeOffset;
      return seg.position + timeInSeg;
    }
  }

  if (!m_segments.empty()) {
    const auto& last = m_segments.back();
    return last.position + last.duration;
  }
  return m_itemPosition + relTime;
}

// --- Peaks from cached audio data (pure math, no API calls) ---

void WaveformView::UpdatePeaks()
{
  int w = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  if (w < 1) w = m_rect.right - m_rect.left;

  // Multi-item active: delegate peak computation
  if (m_multiItemActive) {
    if (w <= 0) { m_peaksValid = false; return; }
    m_multiItem.UpdatePeaks(m_viewStartTime, m_viewDuration, w, m_numChannels,
                            m_peakMax, m_peakMin, m_peakRMS);
    // Build clip column list
    double vol = m_fadeCache.itemVol;
    if (vol <= 0.0) vol = 1.0;
    m_clipColumns.clear();
    int nch = m_numChannels;
    for (int col = 0; col < w; col++) {
      for (int ch = 0; ch < nch; ch++) {
        size_t idx = (size_t)(col * nch + ch);
        if (idx < m_peakMax.size() && (fabs(m_peakMax[idx] * vol) >= 1.0 || fabs(m_peakMin[idx] * vol) >= 1.0)) {
          m_clipColumns.push_back(col);
          break;
        }
      }
    }
    m_peaksValid = true;
    m_peaksCachedStart = m_viewStartTime;
    m_peaksCachedDuration = m_viewDuration;
    m_peaksCachedWidth = w;
    return;
  }

  if (w <= 0 || m_audioSampleCount <= 0) {
    m_peaksValid = false;
    return;
  }

  if (m_peaksValid && m_peaksCachedStart == m_viewStartTime &&
      m_peaksCachedDuration == m_viewDuration && m_peaksCachedWidth == w) {
    return;
  }

  int nch = m_numChannels;
  if (nch < 1) nch = 1;

  m_peakMax.resize((size_t)(w * nch));
  m_peakMin.resize((size_t)(w * nch));
  m_peakRMS.resize((size_t)(w * nch));

  double timePerPixel = m_viewDuration / (double)w;
  for (int col = 0; col < w; col++) {
    double colTime = m_viewStartTime + (double)col * timePerPixel;
    int sampleStart = (int)(colTime * (double)m_sampleRate);
    int sampleEnd = (int)((colTime + timePerPixel) * (double)m_sampleRate);

    // Clamp to valid range
    sampleStart = std::max(0, std::min(m_audioSampleCount - 1, sampleStart));
    sampleEnd = std::max(sampleStart + 1, std::min(m_audioSampleCount, sampleEnd));

    for (int ch = 0; ch < nch; ch++) {
      double maxVal = -2.0;
      double minVal = 2.0;
      double sumSq = 0.0;
      int count = 0;

      // For very zoomed out views, subsample to keep it fast
      // Use conservative step to avoid missing peaks
      int step = 1;
      int span = sampleEnd - sampleStart;
      if (span > 8192) step = span / 4096;

      for (int s = sampleStart; s < sampleEnd; s += step) {
        double v = m_audioData[(size_t)s * nch + ch];
        if (v > maxVal) maxVal = v;
        if (v < minVal) minVal = v;
        sumSq += v * v;
        count++;
      }

      if (maxVal < -1.5) { maxVal = 0.0; minVal = 0.0; }

      size_t idx = (size_t)(col * nch + ch);
      m_peakMax[idx] = maxVal;
      m_peakMin[idx] = minVal;
      m_peakRMS[idx] = (count > 0) ? sqrt(sumSq / (double)count) : 0.0;
    }
  }

  // Build clip column list — scale peaks by item volume (D_VOL)
  double vol = m_fadeCache.itemVol;
  if (vol <= 0.0) vol = 1.0;
  m_clipColumns.clear();
  for (int col = 0; col < w; col++) {
    for (int ch = 0; ch < nch; ch++) {
      size_t idx = (size_t)(col * nch + ch);
      if (fabs(m_peakMax[idx] * vol) >= 1.0 || fabs(m_peakMin[idx] * vol) >= 1.0) {
        m_clipColumns.push_back(col);
        break;
      }
    }
  }

  m_peaksValid = true;
  m_peaksCachedStart = m_viewStartTime;
  m_peaksCachedDuration = m_viewDuration;
  m_peaksCachedWidth = w;
}

// --- Rendering (GDI) ---

void WaveformView::Paint(HDC hdc)
{
  if (!hdc) return;

  int w = m_rect.right - m_rect.left;
  int h = m_rect.bottom - m_rect.top;
  if (w <= 0 || h <= 0) return;

  if ((!m_item || !m_take) && !m_standaloneMode) {
    HBRUSH bgBrush = CreateSolidBrush(g_theme.waveformBg);
    FillRect(hdc, &m_rect, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_theme.emptyText);
    RECT textRect = m_rect;
    DrawText(hdc, "Select a media item to edit", -1, &textRect,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    return;
  }

  // Audition-style: light teal bg, white selection bg
  HBRUSH bgBrush = CreateSolidBrush(g_theme.waveformBg);
  FillRect(hdc, &m_rect, bgBrush);
  DeleteObject(bgBrush);

  // Tint fade regions with slightly darker background
  DrawFadeBackground(hdc);

  bool hasSel = HasSelection();
  if (hasSel) {
    double selStart = std::min(m_selection.startTime, m_selection.endTime);
    double selEnd = std::max(m_selection.startTime, m_selection.endTime);
    int x1 = std::max(m_rect.left, std::min(m_rect.right, TimeToX(selStart)));
    int x2 = std::max(m_rect.left, std::min(m_rect.right, TimeToX(selEnd)));
    if (x2 > x1) {
      RECT selRect = { x1, m_rect.top, x2, m_rect.bottom };
      HBRUSH selBgBrush = CreateSolidBrush(g_theme.waveformSelBg);
      FillRect(hdc, &selRect, selBgBrush);
      DeleteObject(selBgBrush);
    }
  }

  UpdatePeaks();

  // Draw order: time grid → center lines → dB grid lines → waveform → dB scale column → selection → cursor
  // Grid lines go UNDER the waveform so they're subtly visible through gaps
  DrawTimeGrid(hdc);
  for (int ch = 0; ch < m_numChannels; ch++) {
    int chTop = GetChannelTop(ch);
    int chH = GetChannelHeight();
    DrawCenterLine(hdc, chTop + chH / 2);
    DrawDbGridLines(hdc, ch, chTop, chH);
  }

  // LAYERED mode: draw per-layer waveforms. MIX mode: standard single draw.
  if (m_multiItemActive && m_multiItem.GetMode() != MultiItemMode::MIX) {
    m_multiItem.DrawLayers(hdc, m_rect, m_numChannels,
                           m_viewStartTime, m_viewDuration, m_verticalZoom,
                           m_selection, m_batchGainOffset);
  } else {
    for (int ch = 0; ch < m_numChannels; ch++) {
      DrawWaveformChannel(hdc, ch, GetChannelTop(ch), GetChannelHeight());
    }
  }

  // dB scale column (on top of waveform, right edge)
  for (int ch = 0; ch < m_numChannels; ch++) {
    DrawDbScale(hdc, ch, GetChannelTop(ch), GetChannelHeight());
  }

  // Item boundaries for multi-item view
  if ((m_multiItemActive || m_segments.size() > 1) && m_showJoinLines) DrawItemBoundaries(hdc);

  // Selection edges and cursor
  if (hasSel) DrawSelection(hdc);
  DrawCursor(hdc);
  DrawClipIndicators(hdc);
  DrawFadeEnvelope(hdc);
  DrawStandaloneFadeHandles(hdc);

  // Channel separator on top of everything
  if (m_numChannels == 2) {
    int sepY = GetChannelTop(1) - CHANNEL_SEPARATOR_HEIGHT;
    RECT sepRect = { m_rect.left, sepY, m_rect.right, sepY + CHANNEL_SEPARATOR_HEIGHT };
    HBRUSH sepBrush = CreateSolidBrush(RGB(60, 60, 60));
    FillRect(hdc, &sepRect, sepBrush);
    DeleteObject(sepBrush);
  }
}

void WaveformView::DrawWaveformChannel(HDC hdc, int channel, int yTop, int height)
{
  int w = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  if (w < 1) w = 1;
  int nch = m_numChannels;
  if (nch <= 0 || !m_peaksValid) return;

  int centerY = yTop + height / 2;
  float halfH = (float)(height / 2) * m_verticalZoom;

  // Apply item volume (D_VOL) and fades so waveform reflects changes (from cache)
  double itemVol = m_fadeCache.itemVol;
  double fadeInLen = m_fadeCache.fadeInLen;
  double fadeOutLen = m_fadeCache.fadeOutLen;
  int fadeInShape = m_fadeCache.fadeInShape;
  int fadeOutShape = m_fadeCache.fadeOutShape;


  // Pens: normal (green) and selected (dark green), plus RMS (darker variants)
  // Dim channel if muted (inactive)
  bool dimmed = (m_numChannels > 1 && !m_channelActive[channel]);
  COLORREF normColor = dimmed ? RGB(40, 50, 40) : g_theme.waveform;
  COLORREF selColor = dimmed ? RGB(50, 60, 50) : g_theme.waveformSel;
  COLORREF rmsNormColor = dimmed ? RGB(30, 40, 30) : g_theme.waveformRms;
  COLORREF rmsSelColor = dimmed ? RGB(40, 50, 40) : g_theme.waveformRmsSel;
  HPEN normalPen = CreatePen(PS_SOLID, 1, normColor);
  HPEN selPen = CreatePen(PS_SOLID, 1, selColor);
  HPEN rmsNormPen = CreatePen(PS_SOLID, 1, rmsNormColor);
  HPEN rmsSelPen = CreatePen(PS_SOLID, 1, rmsSelColor);

  // Precompute selection pixel range
  bool hasSel = HasSelection();
  int selX1 = 0, selX2 = 0;
  if (hasSel) {
    double s = std::min(m_selection.startTime, m_selection.endTime);
    double e = std::max(m_selection.startTime, m_selection.endTime);
    selX1 = TimeToX(s);
    selX2 = TimeToX(e);
  }

  HPEN curPen = normalPen;
  HPEN oldPen = (HPEN)SelectObject(hdc, curPen);

  // Pre-compute time stepping to avoid per-column XToTime() calls
  double viewStart = m_viewStartTime;
  double timeStep = (w > 1) ? m_viewDuration / (double)w : 0.0;
  double colTime = viewStart;

  for (int col = 0; col < w; col++, colTime += timeStep) {
    size_t idx = (size_t)(col * nch + channel);
    if (idx >= m_peakMax.size()) break;

    int x = m_rect.left + col;

    // Switch pen based on whether we're in selection
    HPEN wantPen = (hasSel && x >= selX1 && x < selX2) ? selPen : normalPen;
    if (wantPen != curPen) {
      SelectObject(hdc, wantPen);
      curPen = wantPen;
    }

    // Calculate fade envelope at this column's time position
    double fadeGain = 1.0;
    if (fadeInLen > 0.0 && colTime < fadeInLen)
      fadeGain *= ApplyFadeShape(colTime / fadeInLen, fadeInShape);
    if (fadeOutLen > 0.0 && colTime > m_itemDuration - fadeOutLen)
      fadeGain *= ApplyFadeShape((m_itemDuration - colTime) / fadeOutLen, fadeOutShape);
    if (fadeGain < 0.0) fadeGain = 0.0;

    // Standalone gain preview (applies to selection region only)
    double sgain = 1.0;
    if (m_standaloneMode && m_standaloneGain != 1.0) {
      if (m_standaloneGainStart < 0.0) {
        sgain = m_standaloneGain; // no selection = full file
      } else if (colTime >= m_standaloneGainStart && colTime < m_standaloneGainEnd) {
        sgain = m_standaloneGain;
      }
    }

    double vol = itemVol * fadeGain * sgain;
    double maxVal = std::max(-1.0, std::min(1.0, m_peakMax[idx] * vol));
    double minVal = std::max(-1.0, std::min(1.0, m_peakMin[idx] * vol));

    int yMax = centerY - (int)(maxVal * (double)halfH);
    int yMin = centerY - (int)(minVal * (double)halfH);

    yMax = std::max(yTop, std::min(yTop + height - 1, yMax));
    yMin = std::max(yTop, std::min(yTop + height - 1, yMin));

    if (yMax > yMin) std::swap(yMax, yMin);

    MoveToEx(hdc, x, yMax, nullptr);
    LineTo(hdc, x, yMin + 1);
  }

  // Draw RMS overlay (narrower, darker)
  curPen = rmsNormPen;
  SelectObject(hdc, curPen);
  colTime = viewStart;
  for (int col = 0; col < w; col++, colTime += timeStep) {
    size_t idx = (size_t)(col * nch + channel);
    if (idx >= m_peakRMS.size()) break;

    int x = m_rect.left + col;
    HPEN wantPen = (hasSel && x >= selX1 && x < selX2) ? rmsSelPen : rmsNormPen;
    if (wantPen != curPen) {
      SelectObject(hdc, wantPen);
      curPen = wantPen;
    }

    double fadeGain = 1.0;
    if (fadeInLen > 0.0 && colTime < fadeInLen)
      fadeGain *= ApplyFadeShape(colTime / fadeInLen, fadeInShape);
    if (fadeOutLen > 0.0 && colTime > m_itemDuration - fadeOutLen)
      fadeGain *= ApplyFadeShape((m_itemDuration - colTime) / fadeOutLen, fadeOutShape);
    if (fadeGain < 0.0) fadeGain = 0.0;

    double sgain2 = 1.0;
    if (m_standaloneMode && m_standaloneGain != 1.0) {
      if (m_standaloneGainStart < 0.0) sgain2 = m_standaloneGain;
      else if (colTime >= m_standaloneGainStart && colTime < m_standaloneGainEnd) sgain2 = m_standaloneGain;
    }
    double vol = itemVol * fadeGain * sgain2;
    double rmsVal = std::min(1.0, m_peakRMS[idx] * vol);

    int yRmsTop = centerY - (int)(rmsVal * (double)halfH);
    int yRmsBot = centerY + (int)(rmsVal * (double)halfH);
    yRmsTop = std::max(yTop, std::min(yTop + height - 1, yRmsTop));
    yRmsBot = std::max(yTop, std::min(yTop + height - 1, yRmsBot));

    MoveToEx(hdc, x, yRmsTop, nullptr);
    LineTo(hdc, x, yRmsBot + 1);
  }

  SelectObject(hdc, oldPen);
  DeleteObject(normalPen);
  DeleteObject(selPen);
  DeleteObject(rmsNormPen);
  DeleteObject(rmsSelPen);
}

void WaveformView::DrawCenterLine(HDC hdc, int yCenter)
{
  HPEN pen = CreatePen(PS_SOLID, 1, g_theme.centerLine);
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, m_rect.left, yCenter, nullptr);
  LineTo(hdc, m_rect.right - DB_SCALE_WIDTH, yCenter);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

void WaveformView::DrawClipIndicators(HDC hdc)
{
  if (m_clipColumns.empty()) return;

  HPEN pen = CreatePen(PS_SOLID, 1, g_theme.clipIndicator);
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);

  for (int ch = 0; ch < m_numChannels; ch++) {
    int chTop = GetChannelTop(ch);
    int chH = GetChannelHeight();

    for (int col : m_clipColumns) {
      int x = m_rect.left + col;
      // 3px red tick at top
      MoveToEx(hdc, x, chTop, nullptr);
      LineTo(hdc, x, chTop + 3);
      // 3px red tick at bottom
      MoveToEx(hdc, x, chTop + chH - 3, nullptr);
      LineTo(hdc, x, chTop + chH);
    }
  }

  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

void WaveformView::DrawItemBoundaries(HDC hdc)
{
  if (m_multiItemActive) {
    // Draw join lines where items meet or overlap (crossfade midpoint)
    const auto& layers = m_multiItem.GetLayers();
    if (layers.empty()) return;

    // Check all pairs of items for adjacency or overlap
    static const double JOIN_THRESH = 0.5; // max gap to still consider a join (500ms)
    std::vector<double> joinTimes;
    int n = (int)layers.size();
    for (int i = 0; i < n; i++) {
      double endA = (layers[i].position - m_itemPosition) + layers[i].duration;
      for (int j = i + 1; j < n; j++) {
        double startB = layers[j].position - m_itemPosition;
        double endB = startB + layers[j].duration;
        double startA = layers[i].position - m_itemPosition;

        // Check both directions: A.end meets B.start, or B.end meets A.start
        double gap1 = startB - endA;   // positive = gap, negative = overlap
        double gap2 = startA - endB;

        if (gap1 >= -layers[i].duration && gap1 <= JOIN_THRESH) {
          // A ends near/at/overlapping B start
          if (gap1 < 0.0) {
            // Overlap: midpoint of overlap region
            double overlapStart = std::max(startA, startB);
            double overlapEnd = std::min(endA, endB);
            joinTimes.push_back((overlapStart + overlapEnd) * 0.5);
          } else {
            // Small gap or exact match: midpoint of gap
            joinTimes.push_back((endA + startB) * 0.5);
          }
        } else if (gap2 >= -layers[j].duration && gap2 <= JOIN_THRESH) {
          // B ends near/at/overlapping A start
          if (gap2 < 0.0) {
            double overlapStart = std::max(startA, startB);
            double overlapEnd = std::min(endA, endB);
            joinTimes.push_back((overlapStart + overlapEnd) * 0.5);
          } else {
            joinTimes.push_back((endB + startA) * 0.5);
          }
        }
      }
    }

    // Sort and deduplicate within pixel distance
    std::sort(joinTimes.begin(), joinTimes.end());
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int lastDrawnX = -999;
    for (double t : joinTimes) {
      int x = TimeToX(t);
      if (x >= m_rect.left && x < m_rect.right && x != lastDrawnX) {
        MoveToEx(hdc, x, m_rect.top, nullptr);
        LineTo(hdc, x, m_rect.bottom);
        lastDrawnX = x;
      }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    return;
  }

  // Legacy concat boundaries
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);

  for (size_t i = 1; i < m_segments.size(); i++) {
    int x = TimeToX(m_segments[i].relativeOffset);
    if (x >= m_rect.left && x < m_rect.right) {
      MoveToEx(hdc, x, m_rect.top, nullptr);
      LineTo(hdc, x, m_rect.bottom);
    }
  }

  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

void WaveformView::DrawSelection(HDC hdc)
{
  double selStart = std::min(m_selection.startTime, m_selection.endTime);
  double selEnd = std::max(m_selection.startTime, m_selection.endTime);

  int x1 = std::max(m_rect.left, std::min(m_rect.right, TimeToX(selStart)));
  int x2 = std::max(m_rect.left, std::min(m_rect.right, TimeToX(selEnd)));
  if (x2 <= x1) return;

  // Thin 1px edge lines
  HPEN edgePen = CreatePen(PS_SOLID, 1, g_theme.selectionEdge);
  HPEN oldPen = (HPEN)SelectObject(hdc, edgePen);
  MoveToEx(hdc, x1, m_rect.top, nullptr);
  LineTo(hdc, x1, m_rect.bottom);
  MoveToEx(hdc, x2, m_rect.top, nullptr);
  LineTo(hdc, x2, m_rect.bottom);
  SelectObject(hdc, oldPen);
  DeleteObject(edgePen);
}

// Vertical time grid lines — synced with ruler tick interval
void WaveformView::DrawTimeGrid(HDC hdc)
{
  int w = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  if (w < 10 || m_viewDuration <= 0) return;

  int waveRight = m_rect.left + w;
  int yTop = m_rect.top;
  int yBot = m_rect.bottom;

  double pixelsPerSec = (double)w / m_viewDuration;

  // Same interval logic as ruler (DrawRuler in edit_view.cpp)
  static const double intervals[] = {
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
    1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0
  };
  double tickInterval = 300.0;
  for (double iv : intervals) {
    if (iv * pixelsPerSec >= 150.0) { tickInterval = iv; break; }
  }

  // Subtle vertical lines at ruler tick positions (Audition-style)
  HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(35, 55, 40));
  HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);

  double firstTick = floor(m_viewStartTime / tickInterval) * tickInterval;
  for (double t = firstTick; t < m_viewStartTime + m_viewDuration; t += tickInterval) {
    int x = TimeToX(t);
    if (x > m_rect.left && x < waveRight) {
      MoveToEx(hdc, x, yTop, nullptr);
      LineTo(hdc, x, yBot);
    }
  }

  SelectObject(hdc, oldPen);
  DeleteObject(gridPen);
}

// Horizontal dB grid lines — dynamic, matches scale labels
void WaveformView::DrawDbGridLines(HDC hdc, int channel, int yTop, int height)
{
  if (height < 40) return;

  int centerY = yTop + height / 2;
  float halfH = (float)(height / 2) * m_verticalZoom;
  int waveRight = m_rect.right - DB_SCALE_WIDTH;

  HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(35, 55, 40));
  HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);

  // Sparse key dB values only (Audition-style: few lines, not dense)
  static const double keyDb[] = { -48, -36, -24, -18, -12, -6, -3, 0, 3, 6, 12 };

  int lastY_top = centerY;
  int lastY_bot = centerY;

  for (double db : keyDb) {
    double linear = pow(10.0, db / 20.0);
    int yOff = (int)(linear * (double)halfH);
    if (yOff < 1) continue;

    int y1 = centerY - yOff;
    int y2 = centerY + yOff;

    // Top half — min 30px spacing
    if (y1 >= yTop && y1 < yTop + height && lastY_top - y1 >= 30) {
      MoveToEx(hdc, m_rect.left, y1, nullptr);
      LineTo(hdc, waveRight, y1);
      lastY_top = y1;
    }
    // Bottom half
    if (y2 >= yTop && y2 < yTop + height && y2 != y1 && y2 - lastY_bot >= 30) {
      MoveToEx(hdc, m_rect.left, y2, nullptr);
      LineTo(hdc, waveRight, y2);
      lastY_bot = y2;
    }
  }

  SelectObject(hdc, oldPen);
  DeleteObject(gridPen);
}

// dB scale column with labels — dynamic Audition-style, adapts to vertical zoom
void WaveformView::DrawDbScale(HDC hdc, int channel, int yTop, int height)
{
  if (height < 40) return;

  int centerY = yTop + height / 2;
  float halfH = (float)(height / 2) * m_verticalZoom;
  int scaleLeft = m_rect.right - DB_SCALE_WIDTH;

  // Column background
  RECT colRect = { scaleLeft, yTop, m_rect.right, yTop + height };
  HBRUSH colBrush = CreateSolidBrush(RGB(25, 25, 25));
  FillRect(hdc, &colRect, colBrush);
  DeleteObject(colBrush);

  // Left border
  HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, scaleLeft, yTop, nullptr);
  LineTo(hdc, scaleLeft, yTop + height);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(hdc, TRANSPARENT);

  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

  // "dB" header
  SetTextColor(hdc, g_theme.dbScaleText);
  RECT hdrRect = { scaleLeft + 2, yTop + 1, m_rect.right - 2, yTop + 13 };
  DrawText(hdc, "dB", -1, &hdrRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Dynamic dB labels — Audition-style, adapts to vertical zoom
  // Center = -∞ (silence), edges = 0 dB at zoom 1.0
  // When zoomed in (verticalZoom > 1), more negative dB values become visible
  // When zoomed out (verticalZoom < 1), 0 dB line moves toward center → show positive dB labels

  // Candidate dB values from center outward (-60 → +12)
  // Iterate from most negative (nearest center) to positive (furthest from center)
  static const double allDb[] = {
    -60, -55, -50, -45, -40, -36, -33, -30, -27, -24,
    -21, -20, -18, -16, -15, -14, -12, -10,
    -9, -8, -7, -6, -5, -4, -3, -2, -1, 0,
    3, 6, 9, 12
  };
  static const int numDb = sizeof(allDb) / sizeof(allDb[0]);

  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));

  // -∞ at center line
  if (centerY > yTop + 4 && centerY < yTop + height - 4) {
    oldPen = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, scaleLeft + 1, centerY, nullptr);
    LineTo(hdc, scaleLeft + 5, centerY);
    SelectObject(hdc, oldPen);

    SetTextColor(hdc, g_theme.dbScaleText);
    RECT tr = { scaleLeft + 5, centerY - 6, m_rect.right - 2, centerY + 6 };
    DrawText(hdc, "-\xE2\x88\x9E", -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Top half: iterate from center outward (y decreasing upward)
  int lastY_top = centerY;
  for (int i = 0; i < numDb; i++) {
    double db = allDb[i];
    double linear = pow(10.0, db / 20.0);
    int yOff = (int)(linear * (double)halfH);
    if (yOff < 1) continue;
    int y = centerY - yOff;
    if (y > yTop + height - 4 || y < yTop + 2) continue;
    if (lastY_top - y < 13) continue;

    oldPen = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, scaleLeft + 1, y, nullptr);
    LineTo(hdc, scaleLeft + 5, y);
    SelectObject(hdc, oldPen);

    char label[8];
    snprintf(label, sizeof(label), "%d", (int)db);
    SetTextColor(hdc, g_theme.dbScaleText);
    RECT tr = { scaleLeft + 5, y - 6, m_rect.right - 2, y + 6 };
    DrawText(hdc, label, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    lastY_top = y;
  }

  // Bottom half: iterate from center outward (y increasing downward)
  int lastY_bot = centerY;
  for (int i = 0; i < numDb; i++) {
    double db = allDb[i];
    double linear = pow(10.0, db / 20.0);
    int yOff = (int)(linear * (double)halfH);
    if (yOff < 1) continue;
    int y = centerY + yOff;
    if (y < yTop + 4 || y > yTop + height - 2) continue;
    if (y - lastY_bot < 13) continue;

    oldPen = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, scaleLeft + 1, y, nullptr);
    LineTo(hdc, scaleLeft + 5, y);
    SelectObject(hdc, oldPen);

    char label[8];
    snprintf(label, sizeof(label), "%d", (int)db);
    SetTextColor(hdc, g_theme.dbScaleText);
    RECT tr = { scaleLeft + 5, y - 6, m_rect.right - 2, y + 6 };
    DrawText(hdc, label, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    lastY_bot = y;
  }

  DeleteObject(tickPen);

  // Channel button — centered vertically, clickable for solo
  if (m_numChannels > 1) {
    char chLabel[4];
    snprintf(chLabel, sizeof(chLabel), "%d", channel + 1);

    int btnW = 18, btnH = 16;
    int btnX = scaleLeft + (DB_SCALE_WIDTH - btnW) / 2;
    int btnY = centerY - btnH / 2;
    RECT btnRect = { btnX, btnY, btnX + btnW, btnY + btnH };

    // Active = green (pressed), Inactive = gray (muted)
    bool active = m_channelActive[channel];
    HBRUSH btnBg = CreateSolidBrush(active ? RGB(0, 160, 60) : RGB(50, 50, 50));
    FillRect(hdc, &btnRect, btnBg);
    DeleteObject(btnBg);

    // Border
    HPEN btnBorder = CreatePen(PS_SOLID, 1, active ? RGB(0, 200, 80) : RGB(80, 80, 80));
    HPEN prevPen = (HPEN)SelectObject(hdc, btnBorder);
    MoveToEx(hdc, btnRect.left, btnRect.top, nullptr);
    LineTo(hdc, btnRect.right - 1, btnRect.top);
    LineTo(hdc, btnRect.right - 1, btnRect.bottom - 1);
    LineTo(hdc, btnRect.left, btnRect.bottom - 1);
    LineTo(hdc, btnRect.left, btnRect.top);
    SelectObject(hdc, prevPen);
    DeleteObject(btnBorder);

    // Label
    HFONT prevFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
    SetTextColor(hdc, active ? RGB(255, 255, 255) : RGB(100, 100, 100));
    DrawText(hdc, chLabel, -1, &btnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, prevFont);
  }

  SelectObject(hdc, oldFont);
}

void WaveformView::DrawCursor(HDC hdc)
{
  bool isPlaying = false;
  if (g_GetPlayState) {
    int playState = g_GetPlayState();
    isPlaying = (playState & 1) != 0;
  }

  // Limit cursor drawing to waveform area (exclude dB scale)
  int waveRight = m_rect.right - DB_SCALE_WIDTH;

  if (!isPlaying) {
    // Stopped: solid red line at edit cursor position
    int cx = TimeToX(m_cursorTime);
    if (cx >= m_rect.left && cx <= waveRight) {
      HPEN curPen = CreatePen(PS_SOLID, 2, g_theme.editCursor);
      HPEN oldPen = (HPEN)SelectObject(hdc, curPen);
      MoveToEx(hdc, cx, m_rect.top, nullptr);
      LineTo(hdc, cx, m_rect.bottom);
      SelectObject(hdc, oldPen);
      DeleteObject(curPen);
    }
  } else {
    // Playing: dashed line at edit cursor (moveable), solid line at playhead

    // 1) Edit cursor — dashed red line (manual dash since SWELL lacks PS_DOT)
    int cx = TimeToX(m_cursorTime);
    if (cx >= m_rect.left && cx <= waveRight) {
      HPEN dashPen = CreatePen(PS_SOLID, 1, g_theme.editCursor);
      HPEN oldPen = (HPEN)SelectObject(hdc, dashPen);
      for (int dy = m_rect.top; dy < m_rect.bottom; dy += 6) {
        MoveToEx(hdc, cx, dy, nullptr);
        LineTo(hdc, cx, std::min(dy + 3, (int)m_rect.bottom));
      }
      SelectObject(hdc, oldPen);
      DeleteObject(dashPen);
    }

    // 2) Playhead — solid red line moving with playback, clamped to item bounds
    if (g_GetPlayPosition2) {
      double absPos = g_GetPlayPosition2();
      double relPos = AbsTimeToRelTime(absPos);
      // Don't draw playhead outside item bounds
      if (relPos < 0.0 || relPos > m_itemDuration) relPos = -1.0;
      int px = TimeToX(relPos);
      if (relPos >= 0.0 && px >= m_rect.left && px <= waveRight) {
        HPEN playPen = CreatePen(PS_SOLID, 2, g_theme.playhead);
        HPEN oldPen = (HPEN)SelectObject(hdc, playPen);
        MoveToEx(hdc, px, m_rect.top, nullptr);
        LineTo(hdc, px, m_rect.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(playPen);
      }
    }
  }
}

bool WaveformView::UpdateFadeCache()
{
  if (!m_item || !g_GetMediaItemInfo_Value || m_standaloneMode) {
    m_fadeCache = {};
    m_fadeCache.itemVol = 1.0;
    return false;
  }

  // Multi-item: D_VOL is already baked into audio data per-segment in SetItems(),
  // so don't apply it again in draw. Batch gain offset reflects knob adjustment.
  // Show first item's fade-in and last item's fade-out.
  if (m_segments.size() > 1) {
    double oldVol = m_fadeCache.itemVol;
    m_fadeCache = {};
    m_fadeCache.itemVol = m_batchGainOffset;
    // Fade-in from first item
    MediaItem* first = m_segments.front().item;
    if (first) {
      m_fadeCache.fadeInLen = g_GetMediaItemInfo_Value(first, "D_FADEINLEN");
      m_fadeCache.fadeInShape = (int)g_GetMediaItemInfo_Value(first, "C_FADEINSHAPE");
    }
    // Fade-out from last item
    MediaItem* last = m_segments.back().item;
    if (last) {
      m_fadeCache.fadeOutLen = g_GetMediaItemInfo_Value(last, "D_FADEOUTLEN");
      m_fadeCache.fadeOutShape = (int)g_GetMediaItemInfo_Value(last, "C_FADEOUTSHAPE");
    }
    return oldVol != m_fadeCache.itemVol;
  }

  double oldVol = m_fadeCache.itemVol;
  m_fadeCache.itemVol = g_GetMediaItemInfo_Value(m_item, "D_VOL");
  // Take volume (the handle in arrange view controls take D_VOL, not item D_VOL)
  if (g_GetSetMediaItemTakeInfo && m_take) {
    double* pTakeVol = (double*)g_GetSetMediaItemTakeInfo(m_take, "D_VOL", nullptr);
    if (pTakeVol) m_fadeCache.itemVol *= *pTakeVol;
  }
  m_fadeCache.fadeInLen = g_GetMediaItemInfo_Value(m_item, "D_FADEINLEN");
  m_fadeCache.fadeOutLen = g_GetMediaItemInfo_Value(m_item, "D_FADEOUTLEN");
  m_fadeCache.fadeInShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEINSHAPE");
  m_fadeCache.fadeOutShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEOUTSHAPE");

  // If volume changed, invalidate peaks so clip indicators update
  bool changed = (oldVol != m_fadeCache.itemVol);
  if (changed) {
    m_peaksValid = false;
  }
  return changed;
}

void WaveformView::SetFadeDragInfo(int dragType, int shape)
{
  m_fadeDragType = dragType;
  m_fadeDragShape = shape;
}

int WaveformView::GetChanMode() const
{
  if (m_numChannels < 2) return 0;
  bool L = m_channelActive[0], R = m_channelActive[1];
  if (L && R) return 0;   // stereo
  if (L && !R) return 3;  // left only
  if (!L && R) return 4;  // right only
  return 0; // both off shouldn't happen
}

bool WaveformView::ClickChannelButton(int x, int y)
{
  if (m_numChannels < 2) return false;

  int scaleLeft = m_rect.right - DB_SCALE_WIDTH;
  if (x < scaleLeft || x > m_rect.right) return false;

  int btnW = 18, btnH = 16;
  int btnX = scaleLeft + (DB_SCALE_WIDTH - btnW) / 2;

  for (int ch = 0; ch < m_numChannels; ch++) {
    int chTop = GetChannelTop(ch);
    int chH = GetChannelHeight();
    int centerY = chTop + chH / 2;
    int btnY = centerY - btnH / 2;

    if (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH) {
      // Toggle this channel — but don't allow both off
      bool newState = !m_channelActive[ch];
      int other = 1 - ch;
      if (!newState && !m_channelActive[other])
        return false; // would mute both — disallow
      m_channelActive[ch] = newState;
      return true;
    }
  }
  return false;
}

void WaveformView::DrawFadeBackground(HDC hdc)
{
  double fadeInLen, fadeOutLen;
  int fadeInShape, fadeOutShape;

  if (m_standaloneMode) {
    fadeInLen = m_standaloneFade.fadeInLen;
    fadeOutLen = m_standaloneFade.fadeOutLen;
    fadeInShape = m_standaloneFade.fadeInShape;
    fadeOutShape = m_standaloneFade.fadeOutShape;
  } else if (m_item) {
    fadeInLen = m_fadeCache.fadeInLen;
    fadeOutLen = m_fadeCache.fadeOutLen;
    fadeInShape = m_fadeCache.fadeInShape;
    fadeOutShape = m_fadeCache.fadeOutShape;
  } else {
    return;
  }
  if (fadeInLen < 0.001 && fadeOutLen < 0.001) return;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;
  int yTop = m_rect.top;
  int yBot = m_rect.bottom;
  int yRange = yBot - yTop;

  // Tint only the area outside the fade curve (where gain < 1.0)
  HPEN tintPen = CreatePen(PS_SOLID, 1, RGB(30, 25, 45));
  HPEN oldPen = (HPEN)SelectObject(hdc, tintPen);

  if (fadeInLen >= 0.001) {
    int x0 = std::max(waveL, TimeToX(0.0));
    int x1 = std::min(waveR, TimeToX(fadeInLen));
    for (int px = x0; px <= x1; px++) {
      double t = (x1 > x0) ? (double)(px - x0) / (double)(x1 - x0) : 1.0;
      double gain = ApplyFadeShape(t, fadeInShape);
      // Curve Y position (gain=1 at top, gain=0 at bottom)
      int curveY = yBot - (int)(gain * yRange);
      // Fill from top down to the curve — the attenuated zone
      if (curveY > yTop) {
        MoveToEx(hdc, px, yTop, nullptr);
        LineTo(hdc, px, curveY);
      }
    }
  }
  if (fadeOutLen >= 0.001) {
    double foStart = m_itemDuration - fadeOutLen;
    int x0 = std::max(waveL, TimeToX(foStart));
    int x1 = std::min(waveR, TimeToX(m_itemDuration));
    for (int px = x0; px <= x1; px++) {
      double t = (x1 > x0) ? (double)(px - x0) / (double)(x1 - x0) : 0.0;
      double gain = ApplyFadeShape(1.0 - t, fadeOutShape);
      int curveY = yBot - (int)(gain * yRange);
      if (curveY > yTop) {
        MoveToEx(hdc, px, yTop, nullptr);
        LineTo(hdc, px, curveY);
      }
    }
  }

  SelectObject(hdc, oldPen);
  DeleteObject(tintPen);
}

void WaveformView::DrawFadeEnvelope(HDC hdc)
{
  double fadeInLen, fadeOutLen;
  int fadeInShape, fadeOutShape;

  if (m_standaloneMode) {
    fadeInLen = m_standaloneFade.fadeInLen;
    fadeOutLen = m_standaloneFade.fadeOutLen;
    fadeInShape = m_standaloneFade.fadeInShape;
    fadeOutShape = m_standaloneFade.fadeOutShape;
  } else if (m_item) {
    fadeInLen = m_fadeCache.fadeInLen;
    fadeOutLen = m_fadeCache.fadeOutLen;
    fadeInShape = m_fadeCache.fadeInShape;
    fadeOutShape = m_fadeCache.fadeOutShape;
  } else {
    return;
  }
  if (fadeInLen < 0.001 && fadeOutLen < 0.001) return;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;

  // Draw once across full waveform height (not per channel)
  int yFull = m_rect.top + 2;           // gain = 1.0
  int yZero = m_rect.bottom - 2;        // gain = 0.0
  int yRange = yZero - yFull;

  // Fade envelope curves
  COLORREF envColor = RGB(255, 200, 50);
  HPEN envPen = CreatePen(PS_SOLID, 2, envColor);
  HPEN oldPen = (HPEN)SelectObject(hdc, envPen);

  // Fade In curve
  if (fadeInLen >= 0.001) {
    int x0 = std::max(waveL, TimeToX(0.0));
    int x1 = std::min(waveR, TimeToX(fadeInLen));
    if (x1 > x0) {
      double g0 = ApplyFadeShape(0.0, fadeInShape);
      MoveToEx(hdc, x0, yZero - (int)(g0 * yRange), nullptr);
      for (int px = x0 + 1; px <= x1; px++) {
        double t = (double)(px - x0) / (double)(x1 - x0);
        double gain = ApplyFadeShape(t, fadeInShape);
        LineTo(hdc, px, yZero - (int)(gain * yRange));
      }
    }
    // Handle at fade-in end
    HBRUSH hb = CreateSolidBrush(envColor);
    RECT handle = { x1 - 4, yFull - 4, x1 + 4, yFull + 4 };
    FillRect(hdc, &handle, hb);
    DeleteObject(hb);
  }

  // Fade Out curve
  if (fadeOutLen >= 0.001) {
    double foStart = m_itemDuration - fadeOutLen;
    int x0 = std::max(waveL, TimeToX(foStart));
    int x1 = std::min(waveR, TimeToX(m_itemDuration));
    if (x1 > x0) {
      MoveToEx(hdc, x0, yFull, nullptr);
      for (int px = x0; px <= x1; px++) {
        double t = (double)(px - x0) / (double)(x1 - x0);
        double gain = ApplyFadeShape(1.0 - t, fadeOutShape);
        LineTo(hdc, px, yZero - (int)(gain * yRange));
      }
    }
    // Handle at fade-out start
    HBRUSH hb = CreateSolidBrush(envColor);
    RECT handle = { x0 - 4, yFull - 4, x0 + 4, yFull + 4 };
    FillRect(hdc, &handle, hb);
    DeleteObject(hb);
  }

  SelectObject(hdc, oldPen);
  DeleteObject(envPen);

  // Draw shape label during fade drag
  if (m_fadeDragType != 0) {
    static const char* shapeNames[] = {
      "Linear", "Fast Start", "Slow Start",
      "Fast Steep", "Slow Steep", "S-Curve", "S-Curve Steep"
    };
    int shapeIdx = std::max(0, std::min(6, m_fadeDragShape));
    char label[64];
    snprintf(label, sizeof(label), "%d: %s", shapeIdx, shapeNames[shapeIdx]);

    // Position label near the relevant handle
    int labelX;
    if (m_fadeDragType == 1) { // fade in — label near right handle
      labelX = std::min(waveR, TimeToX(fadeInLen)) + 8;
    } else { // fade out — label near left handle
      double foStart = m_itemDuration - fadeOutLen;
      labelX = std::max(waveL, TimeToX(foStart)) - 100;
    }
    int labelY = yFull + 4;

    // Draw label with semi-transparent bg
    HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold14);
    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, RGB(30, 30, 30));
    SetTextColor(hdc, envColor);
    RECT labelRect = { labelX, labelY, labelX + 150, labelY + 16 };
    DrawText(hdc, label, -1, &labelRect, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, oldFont);
  }
}

void WaveformView::DrawStandaloneFadeHandles(HDC hdc)
{
  if (!m_standaloneMode) return;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;
  int yTop = m_rect.top + 2;

  COLORREF handleColor = RGB(255, 200, 50);
  HBRUSH hb = CreateSolidBrush(handleColor);

  // Fade-in handle: at top-left (or at fade-in end if active)
  int fiX = waveL;
  if (m_standaloneFade.fadeInLen >= 0.001)
    fiX = std::min(waveR, TimeToX(m_standaloneFade.fadeInLen));
  RECT fiHandle = { fiX - 5, yTop - 5, fiX + 5, yTop + 5 };
  FillRect(hdc, &fiHandle, hb);

  // Fade-out handle: at top-right (or at fade-out start if active)
  int foX = waveR;
  if (m_standaloneFade.fadeOutLen >= 0.001)
    foX = std::max(waveL, TimeToX(m_itemDuration - m_standaloneFade.fadeOutLen));
  RECT foHandle = { foX - 5, yTop - 5, foX + 5, yTop + 5 };
  FillRect(hdc, &foHandle, hb);

  DeleteObject(hb);
}
