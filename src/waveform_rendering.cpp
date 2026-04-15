// ============================================================================
// waveform_rendering.cpp — Waveform peak computation and drawing
//
// UpdatePeaks, Paint, DrawWaveformChannel, DrawDbScale, DrawDbGridLines,
// DrawCenterLine, DrawClipIndicators, DrawItemBoundaries, DrawSelection,
// DrawTimeGrid, DrawCursor, DrawFadeBackground, DrawFadeEnvelope,
// DrawStandaloneFadeHandles, UpdateFadeCache, GetActiveFadeParams,
// SetFadeDragInfo, GetChanMode, ClickChannelButton.
//
// Part of the WaveformView class — methods defined here, class in waveform_view.h.
// ============================================================================

#include "waveform_view.h"
#include "audio_ops.h"
#include "theme.h"
#include "config.h"
#include "globals.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>


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
    int waveR = m_rect.right - DB_SCALE_WIDTH;
    int x1 = std::max((int)m_rect.left, std::min(waveR, TimeToX(selStart)));
    int x2 = std::max((int)m_rect.left, std::min(waveR, TimeToX(selEnd)));
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

  // Draw gap regions between segments (timeline view + multi-item MIX mode)
  if ((m_timelineViewActive || m_multiItemActive) && m_segments.size() >= 2) {
    int waveL = m_rect.left;
    int waveR = m_rect.right - DB_SCALE_WIDTH;
    OwnedBrush gapBrush(RGB(20, 20, 20));
    for (size_t i = 0; i + 1 < m_segments.size(); i++) {
      double gapStart = m_segments[i].relativeOffset + m_segments[i].duration;
      double gapEnd = m_segments[i + 1].relativeOffset;
      if (gapEnd > gapStart) {
        int x1 = std::max(waveL, TimeToX(gapStart));
        int x2 = std::min(waveR, TimeToX(gapEnd));
        if (x2 > x1) {
          RECT gapRect = { x1, m_rect.top, x2, m_rect.bottom };
          gapBrush.Fill(hdc, &gapRect);
        }
      }
    }
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
  if ((m_multiItemActive || m_trackViewActive || m_segments.size() > 1) && m_showJoinLines) DrawItemBoundaries(hdc);

  // Selection edges and cursor
  if (hasSel) DrawSelection(hdc);
  DrawCursor(hdc);
  DrawClipIndicators(hdc);
  DrawVolumeEnvelope(hdc);
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

  // Apply item volume (D_VOL), fades, and take volume envelope
  double itemVol = m_fadeCache.itemVol;
  auto fp = GetActiveFadeParams();
  double fadeInLen = fp.fadeInLen, fadeOutLen = fp.fadeOutLen;
  int fadeInShape = fp.fadeInShape, fadeOutShape = fp.fadeOutShape;
  double fadeInDir = fp.fadeInDir, fadeOutDir = fp.fadeOutDir;

  // Take volume envelope (for waveform visual feedback)
  // Uses GetEnvelopeAtTime helper for per-segment support in timeline/SET
  bool useEnvGain = m_envShowVolume && !m_standaloneMode && m_take &&
      !m_multiItemActive && g_Envelope_Evaluate && g_ScaleFromEnvelopeMode;


  // Pens: normal (green) and selected (dark green), plus RMS (darker variants)
  // Dim channel if muted (inactive)
  bool dimmed = (m_numChannels > 1 && !m_channelActive[channel]);
  COLORREF normColor = dimmed ? RGB(40, 50, 40) : g_theme.waveform;
  COLORREF selColor = dimmed ? RGB(50, 60, 50) : g_theme.waveformSel;
  COLORREF rmsNormColor = dimmed ? RGB(30, 40, 30) : g_theme.waveformRms;
  COLORREF rmsSelColor = dimmed ? RGB(40, 50, 40) : g_theme.waveformRmsSel;
  COLORREF clipColor = RGB(220, 50, 50);
  HPEN normalPen = CreatePen(PS_SOLID, 1, normColor);
  HPEN selPen = CreatePen(PS_SOLID, 1, selColor);
  HPEN clipPen = CreatePen(PS_SOLID, 1, clipColor);
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
      fadeGain *= ApplyFadeShape(colTime / fadeInLen, fadeInShape, -fadeInDir);
    if (fadeOutLen > 0.0 && colTime > m_itemDuration - fadeOutLen)
      fadeGain *= ApplyFadeShape((m_itemDuration - colTime) / fadeOutLen, fadeOutShape, fadeOutDir);
    if (fadeGain < 0.0) fadeGain = 0.0;

    // Gain preview (applies to selection region only, all modes)
    double sgain = 1.0;
    if (m_standaloneGain != 1.0) {
      if (m_standaloneGainStart < 0.0) {
        sgain = m_standaloneGain; // no selection = full range
      } else if (colTime >= m_standaloneGainStart && colTime < m_standaloneGainEnd) {
        sgain = m_standaloneGain;
      }
    }

    // Take volume envelope gain (per-segment via helper)
    double envGain = 1.0;
    if (useEnvGain) {
      auto ei = GetEnvelopeAtTime(colTime);
      if (ei.env) {
        double rawVal = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
        g_Envelope_Evaluate(ei.env, ei.envTime, (double)m_sampleRate, 0, &rawVal, &d1, &d2, &d3);
        envGain = g_ScaleFromEnvelopeMode(ei.scalingMode, rawVal);
      }
    }

    double vol = itemVol * fadeGain * sgain * envGain;
    double rawMax = m_peakMax[idx] * vol;
    double rawMin = m_peakMin[idx] * vol;
    bool clipping = (rawMax > 1.0 || rawMin < -1.0);

    // Allow drawing beyond 0dB (don't clamp to 1.0) but clamp to channel bounds
    int yMax = centerY - (int)(rawMax * (double)halfH);
    int yMin = centerY - (int)(rawMin * (double)halfH);

    yMax = std::max(yTop, std::min(yTop + height - 1, yMax));
    yMin = std::max(yTop, std::min(yTop + height - 1, yMin));

    if (yMax > yMin) std::swap(yMax, yMin);

    if (clipping) {
      // Split column: green for normal range, red for clipping portion
      // Red = top 30% of peak (proportional to severity)
      int greenTop = yMax, greenBot = yMin;

      if (rawMax > 1.0) {
        double redBottom = rawMax * 0.7;
        int yRedBot = centerY - (int)(redBottom * (double)halfH);
        yRedBot = std::max(yTop, std::min(yTop + height - 1, yRedBot));
        // Red: peak to 70% mark
        SelectObject(hdc, clipPen);
        MoveToEx(hdc, x, yMax, nullptr);
        LineTo(hdc, x, yRedBot + 1);
        greenTop = yRedBot + 1; // green starts below red
      }
      if (rawMin < -1.0) {
        double redTop = rawMin * 0.7;
        int yRedTop = centerY - (int)(redTop * (double)halfH);
        yRedTop = std::max(yTop, std::min(yTop + height - 1, yRedTop));
        SelectObject(hdc, clipPen);
        MoveToEx(hdc, x, yRedTop, nullptr);
        LineTo(hdc, x, yMin + 1);
        greenBot = yRedTop - 1; // green ends above red
      }
      // Green: remaining middle portion
      bool inSel = hasSel && x >= selX1 && x < selX2;
      SelectObject(hdc, inSel ? selPen : normalPen);
      if (greenTop <= greenBot) {
        MoveToEx(hdc, x, greenTop, nullptr);
        LineTo(hdc, x, greenBot + 1);
      }
    } else {
      MoveToEx(hdc, x, yMax, nullptr);
      LineTo(hdc, x, yMin + 1);
    }
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
      fadeGain *= ApplyFadeShape(colTime / fadeInLen, fadeInShape, -fadeInDir);
    if (fadeOutLen > 0.0 && colTime > m_itemDuration - fadeOutLen)
      fadeGain *= ApplyFadeShape((m_itemDuration - colTime) / fadeOutLen, fadeOutShape, fadeOutDir);
    if (fadeGain < 0.0) fadeGain = 0.0;

    double sgain2 = 1.0;
    if (m_standaloneGain != 1.0) {
      if (m_standaloneGainStart < 0.0) sgain2 = m_standaloneGain;
      else if (colTime >= m_standaloneGainStart && colTime < m_standaloneGainEnd) sgain2 = m_standaloneGain;
    }
    double envGain2 = 1.0;
    if (useEnvGain) {
      auto ei = GetEnvelopeAtTime(colTime);
      if (ei.env) {
        double rawVal = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
        g_Envelope_Evaluate(ei.env, ei.envTime, (double)m_sampleRate, 0, &rawVal, &d1, &d2, &d3);
        envGain2 = g_ScaleFromEnvelopeMode(ei.scalingMode, rawVal);
      }
    }
    double vol = itemVol * fadeGain * sgain2 * envGain2;
    double rmsVal = m_peakRMS[idx] * vol;

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
  DeleteObject(clipPen);
  DeleteObject(rmsNormPen);
  DeleteObject(rmsSelPen);
}

void WaveformView::DrawCenterLine(HDC hdc, int yCenter)
{
  OwnedPen pen(PS_SOLID, 1, g_theme.centerLine);
  DCPenScope scope(hdc, pen);
  MoveToEx(hdc, m_rect.left, yCenter, nullptr);
  LineTo(hdc, m_rect.right - DB_SCALE_WIDTH, yCenter);
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

  // Concat/Track view boundaries
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

  int waveRight = m_rect.right - DB_SCALE_WIDTH;
  int x1 = std::max((int)m_rect.left, std::min(waveRight, TimeToX(selStart)));
  int x2 = std::max((int)m_rect.left, std::min(waveRight, TimeToX(selEnd)));
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

void WaveformView::UpdateFadeCacheMulti()
{
  m_fadeCache = {};
  m_fadeCache.itemVol = m_batchGainOffset;
  if (m_segments.empty()) return;
  MediaItem* first = m_segments.front().item;
  if (first && (!g_ValidatePtr2 || g_ValidatePtr2(nullptr, first, "MediaItem*"))) {
    m_fadeCache.fadeInLen = g_GetMediaItemInfo_Value(first, "D_FADEINLEN");
    m_fadeCache.fadeInShape = (int)g_GetMediaItemInfo_Value(first, "C_FADEINSHAPE");
    m_fadeCache.fadeInDir = g_GetMediaItemInfo_Value(first, "D_FADEINDIR");
  }
  MediaItem* last = m_segments.back().item;
  if (last && (!g_ValidatePtr2 || g_ValidatePtr2(nullptr, last, "MediaItem*"))) {
    m_fadeCache.fadeOutLen = g_GetMediaItemInfo_Value(last, "D_FADEOUTLEN");
    m_fadeCache.fadeOutShape = (int)g_GetMediaItemInfo_Value(last, "C_FADEOUTSHAPE");
    m_fadeCache.fadeOutDir = g_GetMediaItemInfo_Value(last, "D_FADEOUTDIR");
  }
}

void WaveformView::UpdateFadeCacheSingle()
{
  if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, m_item, "MediaItem*")) return;
  m_fadeCache.itemVol = g_GetMediaItemInfo_Value(m_item, "D_VOL");
  if (g_GetSetMediaItemTakeInfo && m_take && (!g_ValidatePtr2 || g_ValidatePtr2(nullptr, m_take, "MediaItem_Take*"))) {
    double* pTakeVol = (double*)g_GetSetMediaItemTakeInfo(m_take, "D_VOL", nullptr);
    if (pTakeVol) m_fadeCache.itemVol *= *pTakeVol;
  }
  m_fadeCache.fadeInLen = g_GetMediaItemInfo_Value(m_item, "D_FADEINLEN");
  m_fadeCache.fadeOutLen = g_GetMediaItemInfo_Value(m_item, "D_FADEOUTLEN");
  m_fadeCache.fadeInShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEINSHAPE");
  m_fadeCache.fadeOutShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEOUTSHAPE");
  m_fadeCache.fadeInDir = g_GetMediaItemInfo_Value(m_item, "D_FADEINDIR");
  m_fadeCache.fadeOutDir = g_GetMediaItemInfo_Value(m_item, "D_FADEOUTDIR");
}

bool WaveformView::CompareFadeParams(const FadeCache& a, const FadeCache& b) const
{
  return a.fadeInLen != b.fadeInLen || a.fadeOutLen != b.fadeOutLen
      || a.fadeInShape != b.fadeInShape || a.fadeOutShape != b.fadeOutShape
      || a.fadeInDir != b.fadeInDir || a.fadeOutDir != b.fadeOutDir;
}

bool WaveformView::UpdateFadeCache()
{
  if (!m_item || !g_GetMediaItemInfo_Value || m_standaloneMode) {
    m_fadeCache = {};
    m_fadeCache.itemVol = 1.0;
    return false;
  }

  FadeCache old = m_fadeCache;

  if (m_segments.size() > 1 || m_trackViewActive || m_timelineViewActive)
    UpdateFadeCacheMulti();
  else
    UpdateFadeCacheSingle();

  bool volChanged = (old.itemVol != m_fadeCache.itemVol);
  if (volChanged) m_peaksValid = false;
  return volChanged || CompareFadeParams(old, m_fadeCache);
}

WaveformView::FadeParams WaveformView::GetActiveFadeParams() const
{
  FadeParams fp;
  if (m_standaloneMode) {
    fp.fadeInLen = m_standaloneFade.fadeInLen;
    fp.fadeOutLen = m_standaloneFade.fadeOutLen;
    fp.fadeInShape = m_standaloneFade.fadeInShape;
    fp.fadeOutShape = m_standaloneFade.fadeOutShape;
    fp.fadeInDir = m_standaloneFade.fadeInDir;
    fp.fadeOutDir = m_standaloneFade.fadeOutDir;
  } else {
    fp.fadeInLen = m_fadeCache.fadeInLen;
    fp.fadeOutLen = m_fadeCache.fadeOutLen;
    fp.fadeInShape = m_fadeCache.fadeInShape;
    fp.fadeOutShape = m_fadeCache.fadeOutShape;
    fp.fadeInDir = m_fadeCache.fadeInDir;
    fp.fadeOutDir = m_fadeCache.fadeOutDir;
  }
  return fp;
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
  if (!m_standaloneMode && !m_item) return;
  auto fp = GetActiveFadeParams();
  double fadeInLen = fp.fadeInLen, fadeOutLen = fp.fadeOutLen;
  int fadeInShape = fp.fadeInShape, fadeOutShape = fp.fadeOutShape;
  double fadeInDir = fp.fadeInDir, fadeOutDir = fp.fadeOutDir;
  if (fadeInLen < FADE_MIN_LEN && fadeOutLen < FADE_MIN_LEN) return;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;
  int yTop = m_rect.top;
  int yBot = m_rect.bottom;
  int yRange = yBot - yTop;

  // Tint only the area outside the fade curve (where gain < 1.0)
  HPEN tintPen = CreatePen(PS_SOLID, 1, RGB(30, 25, 45));
  HPEN oldPen = (HPEN)SelectObject(hdc, tintPen);

  if (fadeInLen >= FADE_MIN_LEN) {
    int x0 = std::max(waveL, TimeToX(0.0));
    int x1 = std::min(waveR, TimeToX(fadeInLen));
    for (int px = x0; px <= x1; px++) {
      double t = (x1 > x0) ? (double)(px - x0) / (double)(x1 - x0) : 1.0;
      double gain = ApplyFadeShape(t, fadeInShape, -fadeInDir);
      // Curve Y position (gain=1 at top, gain=0 at bottom)
      int curveY = yBot - (int)(gain * yRange);
      // Fill from top down to the curve — the attenuated zone
      if (curveY > yTop) {
        MoveToEx(hdc, px, yTop, nullptr);
        LineTo(hdc, px, curveY);
      }
    }
  }
  if (fadeOutLen >= FADE_MIN_LEN) {
    double foStart = m_itemDuration - fadeOutLen;
    int x0 = std::max(waveL, TimeToX(foStart));
    int x1 = std::min(waveR, TimeToX(m_itemDuration));
    for (int px = x0; px <= x1; px++) {
      double t = (x1 > x0) ? (double)(px - x0) / (double)(x1 - x0) : 0.0;
      double gain = ApplyFadeShape(1.0 - t, fadeOutShape, fadeOutDir);
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
  if (!m_standaloneMode && !m_item) return;
  auto fp = GetActiveFadeParams();
  double fadeInLen = fp.fadeInLen, fadeOutLen = fp.fadeOutLen;
  int fadeInShape = fp.fadeInShape, fadeOutShape = fp.fadeOutShape;
  double fadeInDir = fp.fadeInDir, fadeOutDir = fp.fadeOutDir;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;

  // Draw once across full waveform height
  int yFull = m_rect.top + 2;           // gain = 1.0
  int yZero = m_rect.bottom - 2;        // gain = 0.0
  int yRange = yZero - yFull;

  // Fade envelope curves
  COLORREF envColor = RGB(255, 200, 50);
  OwnedPen envPen(PS_SOLID, 2, envColor);
  DCPenScope penScope(hdc, envPen);
  OwnedBrush handleBrush(envColor);

  // Fade In curve + handle
  {
    int x0 = std::max(waveL, TimeToX(0.0));
    int x1 = std::min(waveR, TimeToX(fadeInLen));
    if (fadeInLen >= FADE_MIN_LEN && x1 > x0) {
      double g0 = ApplyFadeShape(0.0, fadeInShape, -fadeInDir);
      MoveToEx(hdc, x0, yZero - (int)(g0 * yRange), nullptr);
      for (int px = x0 + 1; px <= x1; px++) {
        double t = (double)(px - x0) / (double)(x1 - x0);
        double gain = ApplyFadeShape(t, fadeInShape, -fadeInDir);
        LineTo(hdc, px, yZero - (int)(gain * yRange));
      }
    }
    int hx = (fadeInLen >= FADE_MIN_LEN) ? x1 : x0;
    RECT handle = { hx - FADE_HANDLE_HALF_SIZE, yFull - FADE_HANDLE_HALF_SIZE, hx + FADE_HANDLE_HALF_SIZE, yFull + FADE_HANDLE_HALF_SIZE };
    handleBrush.Fill(hdc, &handle);
  }

  // Fade Out curve + handle
  {
    double foStart = m_itemDuration - fadeOutLen;
    int x0 = std::max(waveL, TimeToX(foStart));
    int x1 = std::min(waveR, TimeToX(m_itemDuration));
    if (fadeOutLen >= FADE_MIN_LEN && x1 > x0) {
      MoveToEx(hdc, x0, yFull, nullptr);
      for (int px = x0; px <= x1; px++) {
        double t = (double)(px - x0) / (double)(x1 - x0);
        double gain = ApplyFadeShape(1.0 - t, fadeOutShape, fadeOutDir);
        LineTo(hdc, px, yZero - (int)(gain * yRange));
      }
    }
    int hx = (fadeOutLen >= FADE_MIN_LEN) ? x0 : x1;
    RECT handle = { hx - FADE_HANDLE_HALF_SIZE, yFull - FADE_HANDLE_HALF_SIZE, hx + FADE_HANDLE_HALF_SIZE, yFull + FADE_HANDLE_HALF_SIZE };
    handleBrush.Fill(hdc, &handle);
  }

  // Draw curvature label during fade drag
  if (m_fadeDragType != 0) {
    static const char* shapeNames[] = {
      "Linear", "Fast Start", "Slow Start",
      "Fast Steep", "Slow Steep", "S-Curve", "S-Curve Steep"
    };
    int shapeIdx = std::max(0, std::min(6, m_fadeDragShape));
    double dir = (m_fadeDragType == 1) ? fadeInDir : fadeOutDir;
    char label[64];
    snprintf(label, sizeof(label), "%s  curve: %+.0f%%", shapeNames[shapeIdx], dir * 100.0);

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

WaveformView::EnvSegmentInfo WaveformView::GetEnvelopeAtTime(double viewTime) const
{
  EnvSegmentInfo info;
  if (!g_GetTakeEnvelopeByName || !g_GetEnvelopeScalingMode) return info;

  // Multi-segment modes: find which segment contains this time
  if ((m_timelineViewActive || m_trackViewActive) && m_segments.size() > 1) {
    for (int i = 0; i < (int)m_segments.size(); i++) {
      const auto& seg = m_segments[i];
      if (viewTime >= seg.relativeOffset && viewTime < seg.relativeOffset + seg.duration) {
        if (!seg.take) return info;
        info.take = seg.take;
        info.segmentIdx = i;
        info.envTime = viewTime - seg.relativeOffset; // time relative to take start
        info.env = g_GetTakeEnvelopeByName(seg.take, "Volume");
        if (info.env) info.scalingMode = g_GetEnvelopeScalingMode(info.env);
        return info;
      }
    }
    return info; // gap region
  }

  // Single-item mode
  if (m_take) {
    info.take = m_take;
    info.segmentIdx = -1;
    info.envTime = viewTime;
    info.env = g_GetTakeEnvelopeByName(m_take, "Volume");
    if (info.env) info.scalingMode = g_GetEnvelopeScalingMode(info.env);
  }
  return info;
}

// Envelope Y mapping: top = +6dB (gain 2.0), bottom = -inf (gain 0.0)
// This puts 0dB (gain 1.0) at 50% height, matching REAPER's arrange view.
static constexpr double ENV_MAX_GAIN = 1.33; // ~+2.5dB at top, 0dB at 75% height

int WaveformView::EnvYToGainY(double gain) const
{
  int yTop = m_rect.top + 2;
  int yBot = m_rect.bottom - 2;
  int yRange = yBot - yTop;
  int y = yBot - (int)((gain / ENV_MAX_GAIN) * (double)yRange);
  return std::max((int)m_rect.top, std::min((int)m_rect.bottom, y));
}

double WaveformView::EnvPixelToGain(int y) const
{
  int yTop = m_rect.top + 2;
  int yBot = m_rect.bottom - 2;
  int yRange = yBot - yTop;
  if (yRange < 1) return 1.0;
  double gain = ((double)(yBot - y) / (double)yRange) * ENV_MAX_GAIN;
  return std::max(0.0, gain);
}

int WaveformView::HitTestEnvelopePoint(int x, int y, int hitRadius) const
{
  if (!m_item || !m_take || m_standaloneMode || !m_envShowVolume) return -1;
  if (m_multiItemActive) return -1; // multi-item: skip for now
  if (!g_CountEnvelopePoints || !g_GetEnvelopePoint || !g_ScaleFromEnvelopeMode) return -1;

  int bestIdx = -1;
  int bestDist = hitRadius * hitRadius + 1;

  // Helper: test points from a single envelope
  auto testEnv = [&](TrackEnvelope* env, int scalingMode, double segRelOffset) {
    int count = g_CountEnvelopePoints(env);
    for (int i = 0; i < count; i++) {
      double ptTime = 0.0, ptValue = 0.0, ptTension = 0.0;
      int ptShape = 0;
      bool ptSelected = false;
      if (!g_GetEnvelopePoint(env, i, &ptTime, &ptValue, &ptShape, &ptTension, &ptSelected))
        continue;
      double gain = g_ScaleFromEnvelopeMode(scalingMode, ptValue);
      int px = TimeToX(ptTime + segRelOffset);
      int py = EnvYToGainY(gain);
      int dx = x - px;
      int dy = y - py;
      int dist = dx * dx + dy * dy;
      if (dist < bestDist) {
        bestDist = dist;
        bestIdx = i;
      }
    }
  };

  if ((m_timelineViewActive || m_trackViewActive) && m_segments.size() > 1 && g_GetTakeEnvelopeByName) {
    // For now, only test the segment under the mouse (via GetEnvelopeAtTime)
    double clickTime = XToTime(x);
    auto ei = GetEnvelopeAtTime(clickTime);
    if (ei.env) {
      const auto& seg = m_segments[ei.segmentIdx];
      testEnv(ei.env, ei.scalingMode, seg.relativeOffset);
    }
  } else if (m_take && g_GetTakeEnvelopeByName) {
    TrackEnvelope* env = g_GetTakeEnvelopeByName(m_take, "Volume");
    if (env) {
      int sm = g_GetEnvelopeScalingMode ? g_GetEnvelopeScalingMode(env) : 0;
      testEnv(env, sm, 0.0);
    }
  }
  return bestIdx;
}

void WaveformView::DrawVolumeEnvelope(HDC hdc)
{
  if (!m_item || !m_take || m_standaloneMode) return;
  if (!m_envShowVolume) return;
  if (m_multiItemActive) return; // multi-item: multiple tracks, skip for now
  if (!g_Envelope_Evaluate || !g_ScaleFromEnvelopeMode) return;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;
  int w = waveR - waveL;
  if (w < 2) return;

  int yRange = (m_rect.bottom - 2) - (m_rect.top + 2);
  if (yRange < 1) return;

  // Draw envelope curve using per-segment helper
  {
    OwnedPen envPen(PS_SOLID, 2, g_theme.volumeEnvelope);
    DCPenScope penScope(hdc, envPen);

    double timeStep = m_viewDuration / (double)w;
    double colTime = m_viewStartTime;
    bool first = true;

    for (int col = 0; col < w; col++, colTime += timeStep) {
      auto ei = GetEnvelopeAtTime(colTime);
      if (!ei.env) { first = true; continue; } // gap - break line

      double rawValue = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
      g_Envelope_Evaluate(ei.env, ei.envTime, (double)m_sampleRate, 0,
                          &rawValue, &d1, &d2, &d3);
      double gain = g_ScaleFromEnvelopeMode(ei.scalingMode, rawValue);
      int y = EnvYToGainY(gain);
      int x = waveL + col;
      if (first) { MoveToEx(hdc, x, y, nullptr); first = false; }
      else LineTo(hdc, x, y);
    }
  }

  // Draw envelope points as small filled circles (all segments)
  if (!g_CountEnvelopePoints || !g_GetEnvelopePoint) return;

  OwnedBrush ptBrush(g_theme.volumeEnvelope);
  OwnedBrush ptBrushSel(RGB(255, 255, 255));  // selected points: white fill
  OwnedPen ptOutline(PS_SOLID, 1, RGB(255, 255, 255));
  OwnedPen ptOutlineSel(PS_SOLID, 1, g_theme.volumeEnvelope); // selected: cyan outline

  auto drawPointsForEnv = [&](TrackEnvelope* env, int scalingMode, double segRelOffset) {
    int count = g_CountEnvelopePoints(env);
    // Dense envelopes (>50 points, e.g. from Apply Dynamics): small 2px dots
    // Sparse envelopes (manual editing): normal 4px circles
    bool dense = (count > 50);
    int normalR = dense ? 2 : 4;
    int skipDist = dense ? 2 : 3;
    int lastPx = -100;
    for (int i = 0; i < count; i++) {
      double ptTime = 0.0, ptValue = 0.0, ptTension = 0.0;
      int ptShape = 0;
      bool ptSelected = false;
      if (!g_GetEnvelopePoint(env, i, &ptTime, &ptValue, &ptShape, &ptTension, &ptSelected))
        continue;

      int px = TimeToX(ptTime + segRelOffset);
      if (px < waveL - 4 || px > waveR + 4) continue;
      if (!ptSelected && abs(px - lastPx) < skipDist) continue;
      lastPx = px;

      double gain = g_ScaleFromEnvelopeMode(scalingMode, ptValue);
      int py = EnvYToGainY(gain);

      int rr = ptSelected ? 5 : normalR;
      HPEN oldPen = (HPEN)SelectObject(hdc, ptSelected ? (HPEN)ptOutlineSel : (HPEN)ptOutline);
      HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, ptSelected ? (HBRUSH)ptBrushSel : (HBRUSH)ptBrush);
      Ellipse(hdc, px - rr, py - rr, px + rr, py + rr);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldBrush);
    }
  };

  if ((m_timelineViewActive || m_trackViewActive) && m_segments.size() > 1) {
    for (const auto& seg : m_segments) {
      if (!seg.take || !g_GetTakeEnvelopeByName) continue;
      TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
      if (!env) continue;
      int sm = g_GetEnvelopeScalingMode ? g_GetEnvelopeScalingMode(env) : 0;
      drawPointsForEnv(env, sm, seg.relativeOffset);
    }
  } else if (m_take) {
    TrackEnvelope* env = g_GetTakeEnvelopeByName ? g_GetTakeEnvelopeByName(m_take, "Volume") : nullptr;
    if (env) {
      int sm = g_GetEnvelopeScalingMode ? g_GetEnvelopeScalingMode(env) : 0;
      drawPointsForEnv(env, sm, 0.0);
    }
  }
}

void WaveformView::DrawStandaloneFadeHandles(HDC hdc)
{
  if (!m_standaloneMode) return;

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;
  int yTop = m_rect.top + 2;

  OwnedBrush hb(RGB(255, 200, 50));

  // Fade-in handle
  int fiX = waveL;
  if (m_standaloneFade.fadeInLen >= FADE_MIN_LEN)
    fiX = std::min(waveR, TimeToX(m_standaloneFade.fadeInLen));
  RECT fiHandle = { fiX - FADE_HANDLE_HALF_SIZE, yTop - FADE_HANDLE_HALF_SIZE, fiX + FADE_HANDLE_HALF_SIZE, yTop + FADE_HANDLE_HALF_SIZE };
  hb.Fill(hdc, &fiHandle);

  // Fade-out handle
  int foX = waveR;
  if (m_standaloneFade.fadeOutLen >= FADE_MIN_LEN)
    foX = std::max(waveL, TimeToX(m_itemDuration - m_standaloneFade.fadeOutLen));
  RECT foHandle = { foX - FADE_HANDLE_HALF_SIZE, yTop - FADE_HANDLE_HALF_SIZE, foX + FADE_HANDLE_HALF_SIZE, yTop + FADE_HANDLE_HALF_SIZE };
  hb.Fill(hdc, &foHandle);
}
