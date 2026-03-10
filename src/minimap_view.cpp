// minimap_view.cpp — Miniature overview of entire item waveform
#include "minimap_view.h"
#include "waveform_view.h"
#include "theme.h"
#include "config.h"
#include <algorithm>
#include <cmath>

void MinimapView::SetRect(int x, int y, int w, int h)
{
  m_rect = { x, y, x + w, y + h };
}

void MinimapView::ComputePeaks(const WaveformView& wv)
{
  int w = m_rect.right - m_rect.left;
  if (w <= 0) return;

  const auto& audioData = wv.GetAudioData();
  int nch = wv.GetNumChannels();
  int totalFrames = wv.GetAudioSampleCount();
  if (audioData.empty() || nch <= 0 || totalFrames <= 0) {
    m_peaksValid = false;
    return;
  }

  if (m_peaksValid && m_cachedWidth == w) return;

  m_peakMax.resize((size_t)w);
  m_peakMin.resize((size_t)w);

  double framesPerPixel = (double)totalFrames / (double)w;
  for (int col = 0; col < w; col++) {
    int sampleStart = (int)((double)col * framesPerPixel);
    int sampleEnd = (int)(((double)col + 1.0) * framesPerPixel);
    sampleStart = std::max(0, std::min(totalFrames - 1, sampleStart));
    sampleEnd = std::max(sampleStart + 1, std::min(totalFrames, sampleEnd));

    double maxVal = -2.0, minVal = 2.0;
    int step = 1;
    int span = sampleEnd - sampleStart;
    if (span > 512) step = span / 256;

    for (int s = sampleStart; s < sampleEnd; s += step) {
      // Mix all channels
      double v = 0.0;
      for (int ch = 0; ch < nch; ch++)
        v += audioData[(size_t)s * nch + ch];
      v /= (double)nch;
      if (v > maxVal) maxVal = v;
      if (v < minVal) minVal = v;
    }

    if (maxVal < -1.5) { maxVal = 0.0; minVal = 0.0; }
    m_peakMax[col] = maxVal;
    m_peakMin[col] = minVal;
  }

  m_peaksValid = true;
  m_cachedWidth = w;
}

void MinimapView::Paint(HDC hdc, const WaveformView& wv)
{
  int w = m_rect.right - m_rect.left;
  int h = m_rect.bottom - m_rect.top;
  if (w <= 0 || h <= 0) return;

  // Background
  HBRUSH bgBrush = CreateSolidBrush(RGB(15, 15, 15));
  FillRect(hdc, &m_rect, bgBrush);
  DeleteObject(bgBrush);

  if (!wv.HasItem()) return;

  ComputePeaks(wv);
  if (!m_peaksValid) return;

  // Draw waveform
  int centerY = m_rect.top + h / 2;
  float halfH = (float)(h / 2);

  HPEN wavePen = CreatePen(PS_SOLID, 1, ColorDarken(g_theme.waveform, 0.3f));
  HPEN oldPen = (HPEN)SelectObject(hdc, wavePen);

  for (int col = 0; col < w; col++) {
    double maxVal = std::max(-1.0, std::min(1.0, m_peakMax[col]));
    double minVal = std::max(-1.0, std::min(1.0, m_peakMin[col]));
    int yMax = centerY - (int)(maxVal * halfH);
    int yMin = centerY - (int)(minVal * halfH);
    if (yMax > yMin) std::swap(yMax, yMin);
    int x = m_rect.left + col;
    MoveToEx(hdc, x, yMax, nullptr);
    LineTo(hdc, x, yMin + 1);
  }

  SelectObject(hdc, oldPen);
  DeleteObject(wavePen);

  // Draw view bounds overlay
  double itemDur = wv.GetItemDuration();
  if (itemDur > 0.0) {
    double viewStart = wv.GetViewStart();
    double viewEnd = viewStart + wv.GetViewDuration();

    int vx1 = m_rect.left + (int)((viewStart / itemDur) * (double)w);
    int vx2 = m_rect.left + (int)((viewEnd / itemDur) * (double)w);
    vx1 = std::max((int)m_rect.left, std::min((int)m_rect.right, vx1));
    vx2 = std::max((int)m_rect.left, std::min((int)m_rect.right, vx2));

    // Semi-transparent: draw dimmed areas outside view
    HBRUSH dimBrush = CreateSolidBrush(RGB(0, 0, 0));
    if (vx1 > m_rect.left) {
      RECT leftDim = { m_rect.left, m_rect.top, vx1, m_rect.bottom };
      FillRect(hdc, &leftDim, dimBrush);
    }
    if (vx2 < m_rect.right) {
      RECT rightDim = { vx2, m_rect.top, m_rect.right, m_rect.bottom };
      FillRect(hdc, &rightDim, dimBrush);
    }
    DeleteObject(dimBrush);

    // View boundary lines
    HPEN boundPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    HPEN oldP = (HPEN)SelectObject(hdc, boundPen);
    MoveToEx(hdc, vx1, m_rect.top, nullptr);
    LineTo(hdc, vx1, m_rect.bottom);
    MoveToEx(hdc, vx2, m_rect.top, nullptr);
    LineTo(hdc, vx2, m_rect.bottom);
    SelectObject(hdc, oldP);
    DeleteObject(boundPen);
  }
}

double MinimapView::XToTime(int x, double itemDuration) const
{
  int w = m_rect.right - m_rect.left;
  if (w <= 0) return 0.0;
  double ratio = (double)(x - m_rect.left) / (double)w;
  return std::max(0.0, std::min(itemDuration, ratio * itemDuration));
}
