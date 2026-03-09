// waveform_view.cpp — Waveform rendering using GDI
// Audio data is loaded once into memory, peaks computed from cache (zero API calls per paint)
#include "waveform_view.h"
#include "reaper_plugin.h"
#include "theme.h"
#include "debug.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

WaveformView::WaveformView() {}
WaveformView::~WaveformView() {}

// Apply REAPER fade shape curve to a linear 0..1 ratio
// shape: 0=linear, 1=fast start, 2=slow start, 3=fast start steep,
//        4=slow start steep, 5=S-curve, 6=S-curve steep
static double ApplyFadeShape(double t, int shape)
{
  t = std::max(0.0, std::min(1.0, t));
  switch (shape) {
    default:
    case 0: return t;                                       // linear
    case 1: return sqrt(t);                                 // fast start
    case 2: return t * t;                                   // slow start
    case 3: return pow(t, 0.25);                            // fast start steep
    case 4: return t * t * t * t;                           // slow start steep
    case 5: return 0.5 - 0.5 * cos(M_PI * t);              // S-curve
    case 6: { double s = 0.5 - 0.5 * cos(M_PI * t);       // S-curve steep
              return s * s * (3.0 - 2.0 * s); }
  }
}

void WaveformView::SetItem(MediaItem* item)
{
  if (m_item == item) return;

  m_item = item;
  m_take = nullptr;
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

  // Apply take channel mode (mono downmix) after loading
  if (g_GetSetMediaItemTakeInfo && m_take && srcChannels == 2) {
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

  DBG("[EditView] SetItem: pos=%.3f dur=%.3f offset=%.3f ch=%d sr=%d samples=%d\n",
      m_itemPosition, m_itemDuration, m_takeOffset, m_numChannels, m_sampleRate, m_audioSampleCount);
}

void WaveformView::ClearItem()
{
  m_item = nullptr;
  m_take = nullptr;
  m_peaksValid = false;
  m_selection = {};
  m_numChannels = 0;
  m_itemDuration = 0.0;
  m_audioData.clear();
  m_audioSampleCount = 0;
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

  int totalFrames = (int)(m_itemDuration * (double)m_sampleRate) + 1;
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
    DBG("[EditView] Downsampling: ratio=%d readRate=%d readFrames=%d\n", ratio, readRate, readFrames);
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

  DBG("[EditView] Loaded %d frames (%d ch) into %.1f MB\n",
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
}
void WaveformView::ClearSelection() { m_selection = {}; m_selecting = false; }

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

// --- Peaks from cached audio data (pure math, no API calls) ---

void WaveformView::UpdatePeaks()
{
  int w = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  if (w < 1) w = m_rect.right - m_rect.left;
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

      // For very zoomed out views, subsample to keep it fast
      // Use conservative step to avoid missing peaks
      int step = 1;
      int span = sampleEnd - sampleStart;
      if (span > 8192) step = span / 4096;

      for (int s = sampleStart; s < sampleEnd; s += step) {
        double v = m_audioData[(size_t)s * nch + ch];
        if (v > maxVal) maxVal = v;
        if (v < minVal) minVal = v;
      }

      if (maxVal < -1.5) { maxVal = 0.0; minVal = 0.0; }

      size_t idx = (size_t)(col * nch + ch);
      m_peakMax[idx] = maxVal;
      m_peakMin[idx] = minVal;
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

  if (!m_item || !m_take) {
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

  // Draw order: center lines → dB grid lines → waveform → dB scale column → selection → cursor
  // Grid lines go UNDER the waveform so they're subtly visible through gaps
  for (int ch = 0; ch < m_numChannels; ch++) {
    int chTop = GetChannelTop(ch);
    int chH = GetChannelHeight();
    DrawCenterLine(hdc, chTop + chH / 2);
    DrawDbGridLines(hdc, ch, chTop, chH);
    DrawWaveformChannel(hdc, ch, chTop, chH);
  }

  // dB scale column (on top of waveform, right edge)
  for (int ch = 0; ch < m_numChannels; ch++) {
    DrawDbScale(hdc, ch, GetChannelTop(ch), GetChannelHeight());
  }

  // Selection edges and cursor
  if (hasSel) DrawSelection(hdc);
  DrawCursor(hdc);
  DrawFadeEnvelope(hdc);

  // Channel separator on top of everything
  if (m_numChannels == 2) {
    int sepY = GetChannelTop(1) - 1;
    HPEN sepPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HPEN oldPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, m_rect.left, sepY, nullptr);
    LineTo(hdc, m_rect.right, sepY);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);
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

  // Apply item volume (D_VOL) and fades so waveform reflects changes
  double itemVol = 1.0;
  double fadeInLen = 0.0, fadeOutLen = 0.0;
  int fadeInShape = 0, fadeOutShape = 0;
  if (m_item && g_GetMediaItemInfo_Value) {
    itemVol = g_GetMediaItemInfo_Value(m_item, "D_VOL");
    fadeInLen = g_GetMediaItemInfo_Value(m_item, "D_FADEINLEN");
    fadeOutLen = g_GetMediaItemInfo_Value(m_item, "D_FADEOUTLEN");
    fadeInShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEINSHAPE");
    fadeOutShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEOUTSHAPE");
  }

  // Two pens: normal (green) and selected (dark green)
  HPEN normalPen = CreatePen(PS_SOLID, 1, g_theme.waveform);
  HPEN selPen = CreatePen(PS_SOLID, 1, g_theme.waveformSel);

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

  for (int col = 0; col < w; col++) {
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
    double colTime = XToTime(x);
    double fadeGain = 1.0;
    if (fadeInLen > 0.0 && colTime < fadeInLen)
      fadeGain *= ApplyFadeShape(colTime / fadeInLen, fadeInShape);
    if (fadeOutLen > 0.0 && colTime > m_itemDuration - fadeOutLen)
      fadeGain *= ApplyFadeShape((m_itemDuration - colTime) / fadeOutLen, fadeOutShape);
    if (fadeGain < 0.0) fadeGain = 0.0;

    double vol = itemVol * fadeGain;
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

  SelectObject(hdc, oldPen);
  DeleteObject(normalPen);
  DeleteObject(selPen);
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

// Horizontal dB grid lines — drawn UNDER waveform (visible through gaps)
void WaveformView::DrawDbGridLines(HDC hdc, int channel, int yTop, int height)
{
  if (height < 40) return;

  int centerY = yTop + height / 2;
  float halfH = (float)(height / 2) * m_verticalZoom;
  int waveRight = m_rect.right - DB_SCALE_WIDTH;

  // Subtle grid line color
  HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(35, 55, 40));
  HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);

  static const double dbValues[] = { -3, -6, -9, -15, -21 };
  for (double db : dbValues) {
    double linear = pow(10.0, db / 20.0);
    int yOff = (int)(linear * (double)halfH);
    int y1 = centerY - yOff;
    int y2 = centerY + yOff;

    if (y1 >= yTop && y1 < yTop + height) {
      MoveToEx(hdc, m_rect.left, y1, nullptr);
      LineTo(hdc, waveRight, y1);
    }
    if (y2 >= yTop && y2 < yTop + height && y2 != y1) {
      MoveToEx(hdc, m_rect.left, y2, nullptr);
      LineTo(hdc, waveRight, y2);
    }
  }

  SelectObject(hdc, oldPen);
  DeleteObject(gridPen);
}

// dB scale column with labels — drawn ON TOP of waveform (right edge)
void WaveformView::DrawDbScale(HDC hdc, int channel, int yTop, int height)
{
  if (height < 40) return;

  int centerY = yTop + height / 2;
  float halfH = (float)(height / 2) * m_verticalZoom;
  int scaleLeft = m_rect.right - DB_SCALE_WIDTH;

  // Column background — dark gray, distinct from black waveform area
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

  // Larger font for readability
  HFONT font = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
  HFONT oldFont = (HFONT)SelectObject(hdc, font);

  // "dB" header
  SetTextColor(hdc, g_theme.dbScaleText);
  RECT hdrRect = { scaleLeft + 2, yTop + 1, m_rect.right - 2, yTop + 14 };
  DrawText(hdc, "dB", -1, &hdrRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Tick + label for each dB level (both halves)
  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));

  static const double dbValues[] = { -3, -6, -9, -15, -21 };
  for (double db : dbValues) {
    double linear = pow(10.0, db / 20.0);
    int yOff = (int)(linear * (double)halfH);
    int y1 = centerY - yOff;
    int y2 = centerY + yOff;

    char label[8];
    snprintf(label, sizeof(label), "%d", (int)db);

    if (y1 >= yTop + 14 && y1 < yTop + height - 14) {
      oldPen = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, y1, nullptr);
      LineTo(hdc, scaleLeft + 5, y1);
      SelectObject(hdc, oldPen);

      SetTextColor(hdc, g_theme.dbScaleText);
      RECT tr = { scaleLeft + 5, y1 - 7, m_rect.right - 2, y1 + 7 };
      DrawText(hdc, label, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (y2 >= yTop + 14 && y2 < yTop + height - 14 && y2 != y1) {
      oldPen = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, y2, nullptr);
      LineTo(hdc, scaleLeft + 5, y2);
      SelectObject(hdc, oldPen);

      SetTextColor(hdc, g_theme.dbScaleText);
      RECT tr = { scaleLeft + 5, y2 - 7, m_rect.right - 2, y2 + 7 };
      DrawText(hdc, label, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }

  DeleteObject(tickPen);

  // Channel number in green
  if (m_numChannels > 1) {
    HFONT chFont = CreateFont(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
    HFONT prevFont = (HFONT)SelectObject(hdc, chFont);
    char chLabel[4];
    snprintf(chLabel, sizeof(chLabel), "%d", channel + 1);
    SetTextColor(hdc, RGB(0, 200, 80));
    RECT chRect = { scaleLeft, yTop + height - 18, m_rect.right - 3, yTop + height - 2 };
    DrawText(hdc, chLabel, -1, &chRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, prevFont);
    DeleteObject(chFont);
  }

  SelectObject(hdc, oldFont);
  DeleteObject(font);
}

void WaveformView::DrawCursor(HDC hdc)
{
  bool isPlaying = false;
  if (g_GetPlayState) {
    int playState = g_GetPlayState();
    isPlaying = (playState & 1) != 0;
  }

  if (!isPlaying) {
    // Stopped: solid red line at edit cursor position
    int cx = TimeToX(m_cursorTime);
    if (cx >= m_rect.left && cx <= m_rect.right) {
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
    if (cx >= m_rect.left && cx <= m_rect.right) {
      HPEN dashPen = CreatePen(PS_SOLID, 1, g_theme.editCursor);
      HPEN oldPen = (HPEN)SelectObject(hdc, dashPen);
      for (int dy = m_rect.top; dy < m_rect.bottom; dy += 6) {
        MoveToEx(hdc, cx, dy, nullptr);
        LineTo(hdc, cx, std::min(dy + 3, (int)m_rect.bottom));
      }
      SelectObject(hdc, oldPen);
      DeleteObject(dashPen);
    }

    // 2) Playhead — solid red line moving with playback
    if (g_GetPlayPosition2) {
      double relPos = g_GetPlayPosition2() - m_itemPosition;
      int px = TimeToX(relPos);
      if (px >= m_rect.left && px <= m_rect.right) {
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

void WaveformView::DrawFadeEnvelope(HDC hdc)
{
  if (!m_item || !g_GetMediaItemInfo_Value) return;

  double fadeInLen = g_GetMediaItemInfo_Value(m_item, "D_FADEINLEN");
  double fadeOutLen = g_GetMediaItemInfo_Value(m_item, "D_FADEOUTLEN");
  if (fadeInLen < 0.001 && fadeOutLen < 0.001) return;

  int fadeInShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEINSHAPE");
  int fadeOutShape = (int)g_GetMediaItemInfo_Value(m_item, "C_FADEOUTSHAPE");

  int waveL = m_rect.left;
  int waveR = m_rect.right - DB_SCALE_WIDTH;

  // Draw once across full waveform height (not per channel)
  int yFull = m_rect.top + 2;           // gain = 1.0
  int yZero = m_rect.bottom - 2;        // gain = 0.0
  int yRange = yZero - yFull;

  // Subtle tint following fade curve shape (area where gain < 1.0)
  // Draw every-other-pixel dark dots in the "attenuated" region above/below the curve
  HPEN dimPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
  HPEN oldPen = (HPEN)SelectObject(hdc, dimPen);
  if (fadeInLen >= 0.001) {
    int x0 = std::max(waveL, TimeToX(0.0));
    int x1 = std::min(waveR, TimeToX(fadeInLen));
    for (int px = x0; px <= x1; px++) {
      double t = (x1 > x0) ? (double)(px - x0) / (double)(x1 - x0) : 1.0;
      double gain = ApplyFadeShape(t, fadeInShape);
      int curveY = yZero - (int)(gain * yRange);
      // Tint from top down to curve (the "cut" area)
      for (int y = yFull; y < curveY; y += 3) {
        MoveToEx(hdc, px, y, nullptr);
        LineTo(hdc, px + 1, y);
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
      int curveY = yZero - (int)(gain * yRange);
      for (int y = yFull; y < curveY; y += 3) {
        MoveToEx(hdc, px, y, nullptr);
        LineTo(hdc, px + 1, y);
      }
    }
  }
  SelectObject(hdc, oldPen);
  DeleteObject(dimPen);

  // Fade envelope curves
  COLORREF envColor = RGB(255, 200, 50);
  HPEN envPen = CreatePen(PS_SOLID, 2, envColor);
  oldPen = (HPEN)SelectObject(hdc, envPen);

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
}
