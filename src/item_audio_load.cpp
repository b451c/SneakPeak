// ============================================================================
// item_audio_load.cpp — Background ITEM audio load for SneakPeak
//
// The working sample buffer of every ITEM view (single item, timeline/SET
// segments, multi-item layers) decodes here in OnTimer slices and installs on
// completion; RequireItemAudio() is the gate every sample consumer goes
// through. Also pumps REAPER's .reapeaks builder when a source has no peaks.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_stream.h"
#include <mutex>
#include "debug.h"
#include "reaper_plugin.h"

#include <cstdio>
#include <algorithm>

// ============================================================================
// Background ITEM audio load (INC-PK1 + phase 2a, design_sdk_peaks_hybrid.md)
// Every ITEM view - single item, timeline/SET segments, multi-item layers -
// paints from REAPER's .reapeaks the moment it is (re)built; the sample
// buffer decodes here in OnTimer slices, one job per take, and installs on
// completion (whole buffer for shared views, per layer for multi-item).
// Anything that needs raw samples gates on RequireItemAudio().
// 8g (design_lazy_buffer.md): a view whose buffer would be downsampled is LAZY -
// it decodes only when a consumer asks (`wanted`) - and no view ever allocates
// more than WaveformView::kMaxBufferBytes (refused before any reserve).
// ============================================================================

void SneakPeak::StartItemAudioLoad(bool wanted)
{
  if (m_destructiveJob.active) return;   // F5: the file is being rewritten - the finish reloads
  AbortItemAudioLoad();
  m_itemLoadOverCap = false;
  if (m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return;
  const unsigned gen = m_waveform.GetLoadGeneration();
  if (m_waveform.IsItemAudioLoaded()) return;
  if (!wanted && m_waveform.ItemBufferIsLazy()) return;

  ItemAudioLoad& L = m_itemLoad;
  L.generation = gen;

  if (m_waveform.IsMultiItemActive()) {
    const MultiItemView& mv = m_waveform.GetMultiItemView();
    L.multi = true;
    L.readRate = mv.GetSampleRate();
    L.nch = std::max(1, mv.GetChannels());
    const auto& layers = mv.GetLayers();
    for (size_t i = 0; i < layers.size(); i++) {
      const auto& ly = layers[i];
      if (ly.plannedFrames <= 0 || ly.audioFrameCount > 0 || !ly.take) continue;
      ItemAudioJob j;
      j.take = ly.take; j.item = ly.item; j.frames = ly.plannedFrames;
      j.srcNch = std::max(1, std::min(L.nch, ly.numChannels));
      j.layerIdx = (int)i;
      L.jobs.push_back(std::move(j));
      L.totalFrames += ly.plannedFrames;
    }
  } else if (m_waveform.GetSegments().size() > 1) {
    // Timeline / SET: one shared buffer laid out by the segment plan (gaps = silence).
    L.readRate = m_waveform.GetSampleRate();
    L.nch = std::max(1, m_waveform.GetNumChannels());
    int total = m_waveform.GetPlannedFrames();
    for (const auto& seg : m_waveform.GetSegments()) {
      if (!seg.take || seg.audioFrameCount <= 0) continue;
      ItemAudioJob j;
      j.take = seg.take; j.item = seg.item; j.dstFrame = seg.audioStartFrame;
      j.frames = seg.audioFrameCount; j.srcNch = L.nch;
      L.jobs.push_back(std::move(j));
      L.totalFrames += seg.audioFrameCount;
      if (seg.audioStartFrame + seg.audioFrameCount > total) total = seg.audioStartFrame + seg.audioFrameCount;
    }
    L.totalFrames = total;
  } else if (m_waveform.GetTake()) {
    int readRate = 0, readFrames = 0;
    if (m_waveform.ComputeItemLoadPlan(readRate, readFrames)) {
      L.single = true;
      L.readRate = readRate;
      L.nch = std::max(1, m_waveform.GetSrcChannels());
      ItemAudioJob j;
      j.take = m_waveform.GetTake(); j.item = m_waveform.GetItem();
      j.frames = readFrames; j.srcNch = L.nch;
      L.jobs.push_back(std::move(j));
      L.totalFrames = readFrames;
    }
  }

  if (L.jobs.empty()) {
    m_itemLoadFailedGen = gen;   // nothing to load for this view - don't spin
    L = ItemAudioLoad();
    return;
  }
  if ((int64_t)L.totalFrames * (int64_t)L.nch * (int64_t)sizeof(double) > WaveformView::kMaxBufferBytes) {
    DBG("[SneakPeak] StartItemAudioLoad: %d frames x %d ch over the buffer cap - refused\n",
        L.totalFrames, L.nch);
    m_itemLoadFailedGen = gen;   // over the cap: no allocation, RequireItemAudio says why
    m_itemLoadOverCap = true;
    L = ItemAudioLoad();
    return;
  }
  if (!L.multi) L.samples.reserve((size_t)L.totalFrames * (size_t)L.nch);
  L.active = true;
  DBG("[SneakPeak] StartItemAudioLoad: %d jobs, %d frames @ %d Hz, %d ch (multi=%d)\n",
      (int)L.jobs.size(), L.totalFrames, L.readRate, L.nch, (int)L.multi);
}

void SneakPeak::AbortItemAudioLoad()
{
  for (auto& j : m_itemLoad.jobs)
    if (j.accessor && g_DestroyAudioAccessor) { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); g_DestroyAudioAccessor(j.accessor); }
  bool wasActive = m_itemLoad.active;
  m_itemLoad = ItemAudioLoad();
  if (wasActive && m_hwnd) UpdateTitle();
}

void SneakPeak::StepItemAudioLoad()
{
  ItemAudioLoad& L = m_itemLoad;
  if (!L.active) return;

  // The view moved on (any reload bumps the generation) or a take died.
  if (m_waveform.IsStandaloneMode() || L.generation != m_waveform.GetLoadGeneration()) {
    AbortItemAudioLoad();
    return;
  }

  // ~15 ms decode budget per tick keeps the UI fluid at TIMER_INTERVAL_MS; a
  // single accessor call covers at most ~1/8 s of audio so one call cannot
  // blow the budget on a slow codec.
  const int chunkMax = std::max(1024, std::min(16384, L.readRate / 8));
  DWORD t0 = GetTickCount();
  while (L.jobIdx < L.jobs.size() && GetTickCount() - t0 < 15) {
    ItemAudioJob& j = L.jobs[L.jobIdx];
    if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)j.take, "MediaItem_Take*")) {
      AbortItemAudioLoad();
      return;
    }
    if (!j.accessor) {
      { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); j.accessor = g_CreateTakeAudioAccessor(j.take); }
      if (!j.accessor) { L.doneFrames += j.frames; L.jobIdx++; L.framesRead = 0; continue; }
      if (L.multi) j.staging.assign((size_t)j.frames * (size_t)L.nch, 0.0);
    }

    int n = std::min(chunkMax, j.frames - L.framesRead);
    double t = (double)L.framesRead / (double)L.readRate;
    // The shared buffer grows chunk by chunk inside its reserved capacity: an
    // upfront zero-fill of the whole thing (460 MB for an hour of stereo at
    // the 8 kHz floor) cost a 0.15 s main-thread stall on select; growing it
    // here touches only this chunk's pages per tick (gaps of skipped jobs
    // are zero-filled by the same resize).
    if (!L.multi) {
      const size_t need = (size_t)(j.dstFrame + L.framesRead + n) * (size_t)L.nch;
      if (L.samples.size() < need) L.samples.resize(need, 0.0);
    }
    double* dst = L.multi ? j.staging.data() + (size_t)L.framesRead * L.nch
                          : L.samples.data() + (size_t)(j.dstFrame + L.framesRead) * L.nch;
    if (j.srcNch < L.nch) {
      // mono source in a stereo layer set: read mono, duplicate (legacy parity)
      std::vector<double> tmp((size_t)n * (size_t)j.srcNch, 0.0);
      int ret;
      { std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
        ret = g_GetAudioAccessorSamples(j.accessor, L.readRate, j.srcNch, t, n, tmp.data()); }
      if (ret > 0)
        for (int f = 0; f < n; f++)
          for (int ch = 0; ch < L.nch; ch++)
            dst[(size_t)f * L.nch + ch] = tmp[(size_t)f * j.srcNch];
      if (ret <= 0) n = j.frames - L.framesRead;   // keep zeros for the rest
    } else {
      int ret;
      { std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
        ret = g_GetAudioAccessorSamples(j.accessor, L.readRate, L.nch, t, n, dst); }
      if (ret <= 0) n = j.frames - L.framesRead;
    }
    L.framesRead += n;
    L.doneFrames += n;

    if (L.framesRead >= j.frames) {
      // Job done: bake D_VOL (timeline/SET/multi parity with the legacy loaders),
      // install a multi-item layer right away, release the accessor.
      if (!L.single) {
        double vol = ItemTakeVolume(j.item, j.take);
        double* base = L.multi ? j.staging.data() : L.samples.data() + (size_t)j.dstFrame * L.nch;
        if (vol != 1.0)
          for (size_t i = 0, cnt = (size_t)j.frames * (size_t)L.nch; i < cnt; i++) base[i] *= vol;
        if (L.multi) {
          m_waveform.GetMultiItemViewMut().InstallLayerAudio((size_t)j.layerIdx, std::move(j.staging),
                                                             j.frames, vol);
          m_waveform.Invalidate();
        }
      }
      if (g_DestroyAudioAccessor) { std::lock_guard<std::mutex> lk(AudioStream::ApiLock()); g_DestroyAudioAccessor(j.accessor); }
      j.accessor = nullptr;
      L.jobIdx++;
      L.framesRead = 0;
    }
  }

  if (L.jobIdx >= L.jobs.size()) {
    FinishItemAudioLoad();
    return;
  }

  if (m_hwnd && L.totalFrames > 0) {
    int pct = (int)(100.0 * (double)L.doneFrames / (double)L.totalFrames);
    if (pct != L.lastPct) {       // title writes are not free - only on change
      L.lastPct = pct;
      char title[256];
      snprintf(title, sizeof(title), "SneakPeak: Loading item audio... %d%%", pct);
      SetWindowText(m_hwnd, title);
    }
  }
}

void SneakPeak::FinishItemAudioLoad()
{
  ItemAudioLoad& L = m_itemLoad;
  if (L.single) {
    // Fold I_CHANMODE mono modes exactly like the legacy synchronous path.
    MediaItem_Take* take = L.jobs.empty() ? nullptr : L.jobs[0].take;
    const int chanMode = TakeChanMode(take);
    const int outNch = FoldedChannels(L.nch, chanMode);
    if (outNch != L.nch) {   // audio_stream.h helpers: the stream folds identically
      FoldChanMode(L.samples.data(), L.totalFrames, chanMode);
      L.samples.resize((size_t)L.totalFrames);
      L.samples.shrink_to_fit();
    }
    m_waveform.InstallItemAudio(std::move(L.samples), L.totalFrames, L.readRate, outNch);
  } else if (!L.multi) {
    L.samples.resize((size_t)L.totalFrames * (size_t)L.nch, 0.0);   // trailing skipped jobs
    m_waveform.InstallItemAudio(std::move(L.samples), L.totalFrames, L.readRate, L.nch);
  }
  AbortItemAudioLoad(); // releases accessors, clears state, restores title

  // Full-fidelity consumers wake up: RMS/flat-top recompute on next paint,
  // minimap upgrades from SDK peaks (Dynamics streams its own trace, 8f).
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  if (m_spectralVisible) { m_spectral.ClearSpectrum(); m_spectral.Invalidate(); }
  if (m_oneShotPanel.IsVisible()) m_osPreviewDirty = true;   // 8g: the preview waited for this
  if (m_limiterPanel.IsVisible()) InvalidateLimiterPreview(); // idem: the GR band + readouts
  if (m_hwnd) Invalidate();
}

bool SneakPeak::ItemAudioReady() const
{
  return m_waveform.IsItemAudioLoaded();
}

// Gate for sample-dependent user actions: starts the load of a lazy view (8g),
// then an honest toast instead of a multi-second freeze (never a synchronous
// fallback load). The caller is re-invoked by the user or wakes on the install
// hook in FinishItemAudioLoad.
bool SneakPeak::RequireItemAudio(const char* what)
{
  if (ItemAudioReady()) return true;
  if (!m_itemLoad.active) StartItemAudioLoad(true);
  char buf[160];
  if (m_itemLoad.active && m_itemLoad.totalFrames > 0)
    snprintf(buf, sizeof(buf), "%s needs the item audio - loading (%d%%)", what,
             (int)(100.0 * (double)m_itemLoad.doneFrames / (double)m_itemLoad.totalFrames));
  else if (m_itemLoadOverCap) {
    const int nch = std::max(1, m_waveform.GetNumChannels());
    const int rate = std::max(1, m_waveform.GetSampleRate());
    const int maxMin = (int)(WaveformView::kMaxBufferBytes / (nch * (int64_t)sizeof(double)) / rate / 60);
    snprintf(buf, sizeof(buf), "Item too long for %s (about %d min max at this rate)", what, maxMin);
  } else
    snprintf(buf, sizeof(buf), "%s needs the item audio - still loading", what);
  ShowToast(buf);
  return false;
}

// .reapeaks not built yet for this source (fresh import with peaks disabled,
// cleared peak cache): pump REAPER's builder so the SDK display can appear.
void SneakPeak::StepSdkPeaksBuild()
{
  if (!m_waveform.SdkPeaksPending() || m_waveform.IsStandaloneMode() ||
      !m_waveform.GetTake() || !g_PCM_Source_BuildPeaks || !g_GetMediaItemTake_Source) {
    m_sdkPeaksBuildStage = -1;
    return;
  }
  PCM_source* src = g_GetMediaItemTake_Source(m_waveform.GetTake());
  if (!src) { m_sdkPeaksBuildStage = -1; return; }

  if (m_sdkPeaksBuildStage < 0) {
    if (g_PCM_Source_BuildPeaks(src, 0) == 0) {
      // Nothing to build - peaks exist; retry the fetch on next paint.
      Invalidate();
      return;
    }
    m_sdkPeaksBuildStage = 0;
    return;
  }

  int left = g_PCM_Source_BuildPeaks(src, 1);
  if (left == 0) {
    g_PCM_Source_BuildPeaks(src, 2);
    m_sdkPeaksBuildStage = -1;
    Invalidate();
  }
}
