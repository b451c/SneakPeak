// multi_item_view.cpp — Multi-item mix/layered view implementation
#include "multi_item_view.h"
#include "waveform_view.h"  // for WaveformSelection
#include "reaper_plugin.h"
#include "config.h"
#include "theme.h"
#include "debug.h"
#include <algorithm>
#include <cmath>
#include <cstring>

void MultiItemView::ScaleLayerAudio(double factor)
{
  if (factor == 1.0) return;
  for (auto& layer : m_layers) {
    for (size_t i = 0; i < layer.audio.size(); i++)
      layer.audio[i] *= factor;
    layer.itemVol *= factor; // sync with new D_VOL to prevent CheckVolumeChanged reload
  }
  m_peaksValid = false;
}

void MultiItemView::ScaleLayerAudioRange(double factor, double startTime, double endTime, int sampleRate)
{
  if (factor == 1.0 || startTime >= endTime || sampleRate <= 0) return;
  double absStart = m_timelineStart + startTime;
  double absEnd = m_timelineStart + endTime;
  for (auto& layer : m_layers) {
    double layerStart = layer.position;
    double layerEnd = layer.position + layer.duration;
    if (layerEnd <= absStart || layerStart >= absEnd) continue;
    double overlapStart = std::max(layerStart, absStart);
    double overlapEnd = std::min(layerEnd, absEnd);
    int f0 = (int)((overlapStart - layerStart) * sampleRate);
    int f1 = (int)((overlapEnd - layerStart) * sampleRate);
    f0 = std::max(0, std::min(f0, layer.audioFrameCount));
    f1 = std::max(f0, std::min(f1, layer.audioFrameCount));
    int nch = (int)layer.audio.size() / std::max(1, layer.audioFrameCount);
    for (int f = f0; f < f1; f++) {
      for (int ch = 0; ch < nch; ch++)
        layer.audio[(size_t)f * nch + ch] *= factor;
    }
    // Sync itemVol with current D_VOL to prevent CheckVolumeChanged reload
    if (layer.item && g_GetMediaItemInfo_Value) {
      double vol = g_GetMediaItemInfo_Value(layer.item, "D_VOL");
      if (g_GetSetMediaItemTakeInfo && layer.take) {
        double* pv = (double*)g_GetSetMediaItemTakeInfo(layer.take, "D_VOL", nullptr);
        if (pv) vol *= *pv;
      }
      if (vol > 0.0) layer.itemVol = vol;
    }
  }
  m_peaksValid = false;
}

void MultiItemView::Clear()
{
  m_layers.clear();
  m_laneZoom.clear();
  m_laneNames.clear();
  m_timelineStart = 0.0;
  m_timelineEnd = 0.0;
  m_peaksValid = false;
  m_cachedViewStart = 0.0;
  m_cachedViewDur = 0.0;
  m_cachedWidth = 0;
}

bool MultiItemView::LoadItems(const std::vector<MediaItem*>& items,
                              int& outChannels, int& outSampleRate)
{
  // Lanes: a rebuild (split/delete/reselect of the same tracks) keeps each track's zoom
  std::vector<std::pair<MediaTrack*, float>> keptZoom;
  for (const auto& layer : m_layers)
    if (layer.track && layer.trackColorIndex < (int)m_laneZoom.size())
      keptZoom.emplace_back(layer.track, m_laneZoom[layer.trackColorIndex]);
  Clear();

  if (items.size() < 2) return false;
  if (!g_GetActiveTake || !g_GetMediaItemInfo_Value || !g_GetMediaItemTake_Source) return false;
  if (!g_CreateTakeAudioAccessor || !g_GetAudioAccessorSamples || !g_DestroyAudioAccessor) return false;

  // Determine sample rate and channel count from first item
  MediaItem_Take* firstTake = g_GetActiveTake(items[0]);
  if (!firstTake) return false;
  PCM_source* src0 = g_GetMediaItemTake_Source(firstTake);
  if (!src0) return false;

  m_sampleRate = (int)src0->GetSampleRate();
  int maxChannels = src0->GetNumChannels();
  if (maxChannels < 1) maxChannels = 1;
  if (maxChannels > 2) maxChannels = 2;

  // Compute timeline bounds and load per-layer audio
  m_timelineStart = 1e30;
  m_timelineEnd = -1e30;

  for (MediaItem* item : items) {
    MediaItem_Take* take = g_GetActiveTake(item);
    if (!take) continue;

    double pos = g_GetMediaItemInfo_Value(item, "D_POSITION");
    double dur = g_GetMediaItemInfo_Value(item, "D_LENGTH");
    if (dur <= 0.0) continue;

    if (pos < m_timelineStart) m_timelineStart = pos;
    if (pos + dur > m_timelineEnd) m_timelineEnd = pos + dur;

    ItemLayer layer;
    layer.item = item;
    layer.take = take;
    layer.position = pos;
    layer.duration = dur;
    layer.itemVol = g_GetMediaItemInfo_Value(item, "D_VOL");
    // Take volume (the handle in arrange view)
    if (g_GetSetMediaItemTakeInfo) {
      double* pTakeVol = (double*)g_GetSetMediaItemTakeInfo(take, "D_VOL", nullptr);
      if (pTakeVol) layer.itemVol *= *pTakeVol;
    }
    if (layer.itemVol <= 0.0) layer.itemVol = 1.0;

    PCM_source* src = g_GetMediaItemTake_Source(take);
    if (src) {
      layer.numChannels = src->GetNumChannels();
      if (layer.numChannels < 1) layer.numChannels = 1;
      if (layer.numChannels > 2) layer.numChannels = 2;
      if (layer.numChannels > maxChannels) maxChannels = layer.numChannels;
    }

    m_layers.push_back(std::move(layer));
  }

  if (m_layers.empty() || m_timelineEnd <= m_timelineStart) {
    Clear();
    return false;
  }

  // Determine if we need to downsample (cap per-layer to keep memory/CPU sane)
  static const int MAX_FRAMES_PER_LAYER = 5000000; // ~1.7 min stereo 48kHz
  double maxLayerDur = 0.0;
  for (const auto& layer : m_layers)
    if (layer.duration > maxLayerDur) maxLayerDur = layer.duration;

  int readRate = m_sampleRate;
  int maxFrames = (int)(maxLayerDur * (double)m_sampleRate);
  if (maxFrames > MAX_FRAMES_PER_LAYER) {
    int ratio = (maxFrames + MAX_FRAMES_PER_LAYER - 1) / MAX_FRAMES_PER_LAYER;
    readRate = m_sampleRate / ratio;
    if (readRate < 8000) readRate = 8000;
    m_sampleRate = readRate;
    DBG("[MultiItem] Downsampling all layers: ratio=%d readRate=%d\n", ratio, readRate);
  }

  // Phase 2a: plan every layer; the samples arrive from SneakPeak's background
  // loader (one job per layer, D_VOL baked on install). Until then the layer
  // draws from .reapeaks (ComputeLayerPeaksFromSDK).
  m_channels = maxChannels;
  for (auto& layer : m_layers) {
    int frames = (int)(layer.duration * (double)readRate);
    if (frames <= 0) continue;
    layer.plannedFrames = frames;
    layer.audioFrameCount = 0;
    layer.audio.clear();
    layer.audioStartFrame = (int)((layer.position - m_timelineStart) * (double)m_sampleRate);
    DBG("[MultiItem] Layer planned: pos=%.3f dur=%.3f vol=%.3f frames=%d startFrame=%d srcCh=%d\n",
        layer.position, layer.duration, layer.itemVol, frames, layer.audioStartFrame,
        layer.numChannels);
  }

  // Assign color indices: per-item sequential; per-track in arrange order (the
  // items arrive sorted by position - lanes must follow the track list instead)
  {
    std::vector<MediaTrack*> tracks;
    for (auto& layer : m_layers) {
      layer.track = g_GetMediaItem_Track ? g_GetMediaItem_Track(layer.item) : nullptr;
      if (std::find(tracks.begin(), tracks.end(), layer.track) == tracks.end())
        tracks.push_back(layer.track);
    }
    std::sort(tracks.begin(), tracks.end(), [](MediaTrack* a, MediaTrack* b) {
      return TrackNumber(a) < TrackNumber(b);
    });
    for (MediaTrack* tr : tracks) {
      float zoom = 1.0f;
      for (const auto& kz : keptZoom) if (kz.first == tr) zoom = kz.second;
      m_laneZoom.push_back(zoom);
      m_laneNames.push_back(TrackLabel(tr));
    }
    for (int i = 0; i < (int)m_layers.size(); i++) {
      m_layers[i].colorIndex = i;
      m_layers[i].trackColorIndex =
        (int)(std::find(tracks.begin(), tracks.end(), m_layers[i].track) - tracks.begin());
    }
  }

  outChannels = maxChannels;
  outSampleRate = m_sampleRate;

  DBG("[MultiItem] Loaded %d layers, timeline=%.3f-%.3f (%.3fs), sr=%d nch=%d\n",
      (int)m_layers.size(), m_timelineStart, m_timelineEnd,
      m_timelineEnd - m_timelineStart, m_sampleRate, maxChannels);
  return true;
}

// --- Peak computation ---

double MultiItemView::GetLayerSample(const ItemLayer& layer, int timelineFrame, int ch, int nch) const
{
  int layerFrame = timelineFrame - layer.audioStartFrame;
  if (layerFrame < 0 || layerFrame >= layer.audioFrameCount) return 0.0;
  size_t idx = (size_t)layerFrame * nch + ch;
  if (idx >= layer.audio.size()) return 0.0;
  return layer.audio[idx];
}

void MultiItemView::GetMixedAudio(int startFrame, int endFrame, int numChannels,
                                  std::vector<double>& out) const
{
  if (m_layers.empty() || startFrame >= endFrame) {
    out.clear();
    return;
  }
  int frames = endFrame - startFrame;
  int nch = std::max(1, std::min(2, numChannels));
  out.assign((size_t)frames * nch, 0.0);

  for (const auto& layer : m_layers) {
    // Compute overlap between [startFrame, endFrame) and layer's frame range
    int layerEnd = layer.audioStartFrame + layer.audioFrameCount;
    int from = std::max(startFrame, layer.audioStartFrame);
    int to = std::min(endFrame, layerEnd);
    if (from >= to) continue;

    for (int f = from; f < to; f++) {
      int layerF = f - layer.audioStartFrame;
      int outIdx = (f - startFrame) * nch;
      for (int ch = 0; ch < nch; ch++) {
        size_t srcIdx = (size_t)layerF * nch + ch;
        if (srcIdx < layer.audio.size())
          out[outIdx + ch] += layer.audio[srcIdx];
      }
    }
  }
}

bool MultiItemView::CheckVolumeChanged() const
{
  for (const auto& layer : m_layers) {
    if (!layer.item || !g_GetMediaItemInfo_Value) continue;
    double vol = g_GetMediaItemInfo_Value(layer.item, "D_VOL");
    if (g_GetSetMediaItemTakeInfo && layer.take) {
      double* pTakeVol = (double*)g_GetSetMediaItemTakeInfo(layer.take, "D_VOL", nullptr);
      if (pTakeVol) vol *= *pTakeVol;
    }
    if (vol <= 0.0) vol = 1.0;
    // Compare with baked volume (tolerance for float rounding)
    if (std::abs(vol - layer.itemVol) > 1e-6) return true;
  }
  return false;
}

void MultiItemView::UpdatePeaks(double viewStart, double viewDur, int width, int numChannels,
                                std::vector<double>& peakMax, std::vector<double>& peakMin,
                                std::vector<double>& peakRMS)
{
  if (m_peaksValid && m_cachedViewStart == viewStart &&
      m_cachedViewDur == viewDur && m_cachedWidth == width) {
    return;
  }

  if (m_mode == MultiItemMode::MIX && AllLayersLoaded()) {
    ComputeMixPeaks(viewStart, viewDur, width, numChannels, peakMax, peakMin, peakRMS);
  } else {
    // LAYERED modes: compute per-layer peaks, then derive mix from layer peaks (fast)
    ComputeLayeredPeaks(viewStart, viewDur, width, numChannels);

    // Build approximate mix peaks from per-layer peaks (sum of peaks, no re-scan of audio)
    int nch = numChannels;
    size_t total = (size_t)(width * nch);
    peakMax.assign(total, 0.0);
    peakMin.assign(total, 0.0);
    peakRMS.assign(total, 0.0);
    for (const auto& layer : m_layers) {
      if (layer.peakMax.size() < total) continue;
      for (size_t i = 0; i < total; i++) {
        peakMax[i] += layer.peakMax[i];
        peakMin[i] += layer.peakMin[i];
        double r = layer.peakRMS[i];
        peakRMS[i] += r * r; // sum of squares for RMS
      }
    }
    for (size_t i = 0; i < total; i++) {
      peakRMS[i] = sqrt(peakRMS[i]);
    }
  }

  m_peaksValid = true;
  m_cachedViewStart = viewStart;
  m_cachedViewDur = viewDur;
  m_cachedWidth = width;
}

void MultiItemView::ComputeMixPeaks(double viewStart, double viewDur, int width, int numChannels,
                                    std::vector<double>& peakMax, std::vector<double>& peakMin,
                                    std::vector<double>& peakRMS)
{
  int nch = numChannels;
  peakMax.resize((size_t)(width * nch));
  peakMin.resize((size_t)(width * nch));
  peakRMS.resize((size_t)(width * nch));

  double timePerPixel = viewDur / (double)width;
  int numLayers = (int)m_layers.size();

  for (int col = 0; col < width; col++) {
    double colRelTime = viewStart + (double)col * timePerPixel;
    int sampleStart = (int)(colRelTime * (double)m_sampleRate);
    int sampleEnd = (int)((colRelTime + timePerPixel) * (double)m_sampleRate);
    if (sampleStart < 0) sampleStart = 0;
    if (sampleEnd < sampleStart + 1) sampleEnd = sampleStart + 1;

    int span = sampleEnd - sampleStart;
    int step = 1;
    if (span > 2048) step = span / 1024;

    for (int ch = 0; ch < nch; ch++) {
      double maxVal = -2.0;
      double minVal = 2.0;
      double sumSq = 0.0;
      int count = 0;

      for (int s = sampleStart; s < sampleEnd; s += step) {
        double sum = 0.0;
        for (int li = 0; li < numLayers; li++) {
          const auto& layer = m_layers[li];
          int lf = s - layer.audioStartFrame;
          if (lf >= 0 && lf < layer.audioFrameCount) {
            size_t ai = (size_t)lf * nch + ch;
            if (ai < layer.audio.size()) sum += layer.audio[ai];
          }
        }
        if (sum > maxVal) maxVal = sum;
        if (sum < minVal) minVal = sum;
        sumSq += sum * sum;
        count++;
      }

      if (maxVal < -1.5) { maxVal = 0.0; minVal = 0.0; }

      size_t idx = (size_t)(col * nch + ch);
      peakMax[idx] = maxVal;
      peakMin[idx] = minVal;
      peakRMS[idx] = (count > 0) ? sqrt(sumSq / (double)count) : 0.0;
    }
  }
}

void MultiItemView::ComputeLayeredPeaks(double viewStart, double viewDur, int width, int numChannels)
{
  int nch = numChannels;
  double timePerPixel = viewDur / (double)width;
  double sr = (double)m_sampleRate;

  for (auto& layer : m_layers) {
    size_t total = (size_t)(width * nch);
    layer.peakMax.assign(total, 0.0);
    layer.peakMin.assign(total, 0.0);
    layer.peakRMS.assign(total, 0.0);

    if (layer.audioFrameCount <= 0) {   // still loading: REAPER's own peaks
      ComputeLayerPeaksFromSDK(layer, viewStart, viewDur, width, nch);
      continue;
    }

    // Compute column range where this layer has audio — skip everything outside
    double layerRelStart = (double)layer.audioStartFrame / sr;
    double layerRelEnd = layerRelStart + (double)layer.audioFrameCount / sr;
    int colStart = (int)((layerRelStart - viewStart) / timePerPixel);
    int colEnd = (int)((layerRelEnd - viewStart) / timePerPixel) + 1;
    colStart = std::max(0, colStart);
    colEnd = std::min(width, colEnd);
    if (colStart >= colEnd) continue; // layer not visible

    for (int col = colStart; col < colEnd; col++) {
      double colRelTime = viewStart + (double)col * timePerPixel;
      int sampleStart = (int)(colRelTime * sr);
      int sampleEnd = (int)((colRelTime + timePerPixel) * sr);
      if (sampleStart < 0) sampleStart = 0;
      if (sampleEnd < sampleStart + 1) sampleEnd = sampleStart + 1;

      // Clamp to layer's audio range
      int layerSampleStart = sampleStart - layer.audioStartFrame;
      int layerSampleEnd = sampleEnd - layer.audioStartFrame;
      if (layerSampleStart < 0) layerSampleStart = 0;
      if (layerSampleEnd > layer.audioFrameCount) layerSampleEnd = layer.audioFrameCount;
      if (layerSampleStart >= layerSampleEnd) continue;

      int span = layerSampleEnd - layerSampleStart;
      int step = 1;
      if (span > 2048) step = span / 1024;

      for (int ch = 0; ch < nch; ch++) {
        double maxVal = -2.0;
        double minVal = 2.0;
        double sumSq = 0.0;
        int count = 0;

        for (int lf = layerSampleStart; lf < layerSampleEnd; lf += step) {
          size_t ai = (size_t)lf * nch + ch;
          double v = layer.audio[ai]; // no bounds check needed — clamped above
          if (v > maxVal) maxVal = v;
          if (v < minVal) minVal = v;
          sumSq += v * v;
          count++;
        }

        if (maxVal < -1.5) { maxVal = 0.0; minVal = 0.0; }

        size_t idx = (size_t)(col * nch + ch);
        layer.peakMax[idx] = maxVal;
        layer.peakMin[idx] = minVal;
        layer.peakRMS[idx] = (count > 0) ? sqrt(sumSq / (double)count) : 0.0;
      }
    }
  }
}

// --- LAYERED drawing ---

// Blend color with background for pseudo-alpha (GDI has no native alpha)
COLORREF MultiItemView::BlendColor(COLORREF fg, COLORREF bg, float alpha)
{
  int r = (int)(GetRValue(fg) * alpha + GetRValue(bg) * (1.0f - alpha));
  int g = (int)(GetGValue(fg) * alpha + GetGValue(bg) * (1.0f - alpha));
  int b = (int)(GetBValue(fg) * alpha + GetBValue(bg) * (1.0f - alpha));
  return RGB(std::max(0, std::min(255, r)),
             std::max(0, std::min(255, g)),
             std::max(0, std::min(255, b)));
}

void MultiItemView::DrawLayers(HDC hdc, RECT rect, int numChannels,
                               double viewStart, double viewDur, float verticalZoom,
                               const WaveformSelection& selection, double gainOffset)
{
  if (m_mode == MultiItemMode::LANES) {   // one band per track (multi_item_lanes.cpp)
    DrawLanes(hdc, rect, numChannels, viewStart, viewDur, verticalZoom, selection, gainOffset);
    return;
  }
  int w = rect.right - rect.left - SP(DB_SCALE_WIDTH);
  if (w < 1) w = 1;
  int nch = numChannels;
  if (nch < 1) return;

  int totalH = rect.bottom - rect.top;
  int chH = (nch <= 1) ? totalH : (totalH - SP(CHANNEL_SEPARATOR_HEIGHT) * (nch - 1)) / nch;

  // Selection range in pixels
  bool hasSel = selection.active && selection.startTime != selection.endTime;
  int selX1 = 0, selX2 = 0;
  if (hasSel) {
    double s = std::min(selection.startTime, selection.endTime);
    double e = std::max(selection.startTime, selection.endTime);
    double pixPerSec = (w > 0 && viewDur > 0.0) ? (double)w / viewDur : 1.0;
    selX1 = rect.left + (int)((s - viewStart) * pixPerSec);
    selX2 = rect.left + (int)((e - viewStart) * pixPerSec);
  }

  COLORREF bgColor = g_theme.waveformBg;

  // Build draw order: for LAYERED_TRACKS, group by track so same-track layers
  // are drawn together (earlier tracks underneath, later tracks on top)
  std::vector<int> drawOrder(m_layers.size());
  for (int i = 0; i < (int)m_layers.size(); i++) drawOrder[i] = i;
  if (m_mode == MultiItemMode::LAYERED_TRACKS) {
    std::sort(drawOrder.begin(), drawOrder.end(), [this](int a, int b) {
      return m_layers[a].trackColorIndex < m_layers[b].trackColorIndex;
    });
  }

  // Pre-create all pens (avoid per-layer CreatePen/DeleteObject)
  struct LayerPens { HPEN peak, rms, peakSel, rmsSel; };
  std::vector<LayerPens> allPens(m_layers.size());
  for (int di = 0; di < (int)drawOrder.size(); di++) {
    int li = drawOrder[di];
    const auto& layer = m_layers[li];
    int ci = (m_mode == MultiItemMode::LAYERED_TRACKS) ? layer.trackColorIndex : layer.colorIndex;
    COLORREF base = kLayerColors[ci % kNumLayerColors];
    allPens[li].peak    = CreatePen(PS_SOLID, 1, BlendColor(base, bgColor, 0.7f));
    allPens[li].rms     = CreatePen(PS_SOLID, 1, BlendColor(base, bgColor, 0.9f));
    allPens[li].peakSel = CreatePen(PS_SOLID, 1, BlendColor(base, g_theme.waveformSelBg, 0.7f));
    allPens[li].rmsSel  = CreatePen(PS_SOLID, 1, BlendColor(base, g_theme.waveformSelBg, 0.9f));
  }

  double sr = (double)m_sampleRate;
  double timePerPixel = viewDur / (double)w;

  for (int di = 0; di < (int)drawOrder.size(); di++) {
    int layerIdx = drawOrder[di];
    const auto& layer = m_layers[layerIdx];
    if (layer.peakMax.empty()) continue;

    const auto& pens = allPens[layerIdx];

    // Compute visible column range for this layer
    double layerRelStart = (double)layer.audioStartFrame / sr;
    double layerRelEnd = layerRelStart + (double)layer.audioFrameCount / sr;
    int colStart = (int)((layerRelStart - viewStart) / timePerPixel);
    int colEnd = (int)((layerRelEnd - viewStart) / timePerPixel) + 1;
    colStart = std::max(0, colStart);
    colEnd = std::min(w, colEnd);
    if (colStart >= colEnd) continue;

    for (int ch = 0; ch < nch; ch++) {
      int chTop = rect.top + ch * (chH + SP(CHANNEL_SEPARATOR_HEIGHT));
      int centerY = chTop + chH / 2;
      float halfH = (float)(chH / 2) * verticalZoom;

      // Single pass: peak + RMS together per column (halves iteration count)
      HPEN curPen = pens.peak;
      HPEN oldPen = (HPEN)SelectObject(hdc, curPen);

      for (int col = colStart; col < colEnd; col++) {
        size_t idx = (size_t)(col * nch + ch);
        if (idx >= layer.peakMax.size()) break;

        int x = rect.left + col;
        bool inSel = hasSel && x >= selX1 && x < selX2;

        // Peak line
        HPEN wantPen = inSel ? pens.peakSel : pens.peak;
        if (wantPen != curPen) { SelectObject(hdc, wantPen); curPen = wantPen; }

        double maxVal = std::max(-1.0, std::min(1.0, layer.peakMax[idx] * gainOffset));
        double minVal = std::max(-1.0, std::min(1.0, layer.peakMin[idx] * gainOffset));

        int yMax = centerY - (int)(maxVal * (double)halfH);
        int yMin = centerY - (int)(minVal * (double)halfH);
        yMax = std::max(chTop, std::min(chTop + chH - 1, yMax));
        yMin = std::max(chTop, std::min(chTop + chH - 1, yMin));
        if (yMax > yMin) std::swap(yMax, yMin);

        MoveToEx(hdc, x, yMax, nullptr);
        LineTo(hdc, x, yMin + 1);

        // RMS line (overdraw on same column — merged pass)
        wantPen = inSel ? pens.rmsSel : pens.rms;
        if (wantPen != curPen) { SelectObject(hdc, wantPen); curPen = wantPen; }

        double rmsVal = std::min(1.0, layer.peakRMS[idx] * gainOffset);
        int yRmsTop = centerY - (int)(rmsVal * (double)halfH);
        int yRmsBot = centerY + (int)(rmsVal * (double)halfH);
        yRmsTop = std::max(chTop, std::min(chTop + chH - 1, yRmsTop));
        yRmsBot = std::max(chTop, std::min(chTop + chH - 1, yRmsBot));

        MoveToEx(hdc, x, yRmsTop, nullptr);
        LineTo(hdc, x, yRmsBot + 1);
      }

      SelectObject(hdc, oldPen);
    }
  }

  // Cleanup all pens
  for (int li = 0; li < (int)m_layers.size(); li++) {
    DeleteObject(allPens[li].peak);
    DeleteObject(allPens[li].rms);
    DeleteObject(allPens[li].peakSel);
    DeleteObject(allPens[li].rmsSel);
  }
}

// --- Phase 2a: background loading support ---

// One .reapeaks fetch for the layer's visible columns (mono/stereo mapped to the
// common channel count, D_VOL applied like the baked buffer, RMS never faked).
void MultiItemView::ComputeLayerPeaksFromSDK(ItemLayer& layer, double viewStart, double viewDur,
                                             int width, int numChannels)
{
  if (!g_GetMediaItemTake_Peaks || !layer.take || width <= 0 || viewDur <= 0.0) return;
  int nch = numChannels;
  double step = viewDur / (double)width;
  double layerRelStart = layer.position - m_timelineStart;
  double layerRelEnd = layerRelStart + layer.duration;
  int c0 = (int)ceil((layerRelStart - viewStart) / step);
  int c1 = (int)floor((layerRelEnd - viewStart) / step);
  if (c0 < 0) c0 = 0;
  if (c1 > width) c1 = width;
  int n = c1 - c0;
  if (n <= 0) return;

  int srcNch = 1;
  if (g_GetMediaItemTake_Source) {
    PCM_source* src = g_GetMediaItemTake_Source(layer.take);
    if (src) srcNch = std::max(1, std::min(2, src->GetNumChannels()));
  }
  double starttime = layer.position + (viewStart + (double)c0 * step - layerRelStart);
  std::vector<double> buf((size_t)srcNch * (size_t)n * 2, 0.0);
  int ret = g_GetMediaItemTake_Peaks(layer.take, (double)width / viewDur, starttime, srcNch, n, 0, buf.data());
  int actual = ret & 0xFFFFF;
  int mode = (ret >> 20) & 0xF;
  if (actual <= 0 || (mode != 0 && mode != 1)) return;

  double vol = layer.itemVol > 0.0 ? layer.itemVol : 1.0;
  auto sampleAt = [&](int s, int ch, bool minSide) -> double {
    size_t minOff = (mode == 0 && minSide) ? (size_t)srcNch * (size_t)actual : 0;
    if (srcNch == 2 && nch == 1) {
      return (buf[minOff + (size_t)s * 2] + buf[minOff + (size_t)s * 2 + 1]) * 0.5;
    }
    int sc = (srcNch == 1) ? 0 : ch;
    return buf[minOff + (size_t)s * srcNch + sc];
  };
  for (int col = c0; col < c1; col++) {
    int local = col - c0;
    for (int ch = 0; ch < nch; ch++) {
      double mx, mn;
      if (mode == 0) {
        int sidx = (local < actual) ? local : actual - 1;
        mx = sampleAt(sidx, ch, false);
        mn = sampleAt(sidx, ch, true);
      } else {
        double spp = (double)actual / (double)n;
        int s0 = (int)(local * spp), s1 = (int)((local + 1) * spp);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > actual) s1 = actual;
        mx = -2.0; mn = 2.0;
        for (int sidx = s0; sidx < s1; sidx++) {
          double v = sampleAt(sidx, ch, false);
          if (v > mx) mx = v;
          if (v < mn) mn = v;
        }
        if (mx < -1.5) { mx = 0.0; mn = 0.0; }
      }
      size_t idx = (size_t)(col * nch + ch);
      layer.peakMax[idx] = mx * vol;
      layer.peakMin[idx] = mn * vol;
    }
  }
}

void MultiItemView::InstallLayerAudio(size_t idx, std::vector<double>&& audio, int frames, double bakedVol)
{
  if (idx >= m_layers.size()) return;
  ItemLayer& layer = m_layers[idx];
  layer.audio = std::move(audio);
  layer.audioFrameCount = frames;
  layer.numChannels = m_channels;   // upmixed to the common channel count
  if (bakedVol > 0.0) layer.itemVol = bakedVol;
  m_peaksValid = false;
}

bool MultiItemView::AllLayersLoaded() const
{
  for (const auto& layer : m_layers)
    if (layer.plannedFrames > 0 && layer.audioFrameCount <= 0) return false;
  return true;
}

void MultiItemView::DropAudio()
{
  for (auto& layer : m_layers) {
    layer.audio.clear();
    layer.audio.shrink_to_fit();
    layer.audioFrameCount = 0;
  }
  m_peaksValid = false;
}
