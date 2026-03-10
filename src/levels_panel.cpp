// levels_panel.cpp — RMS/Peak level meter
#include "levels_panel.h"
#include "theme.h"
#include <cmath>
#include <algorithm>

void LevelsPanel::Update(const std::vector<double>& audio, int startFrame,
                         int endFrame, int sampleRate, int nch, double itemVol, bool playing,
                         const bool* channelActive)
{
  // When not playing, decay all meters to silence
  if (!playing) {
    if (m_wasPlaying || m_rmsL > -60.0 || m_rmsR > -60.0) {
      m_rmsL = std::max(-60.0, m_rmsL - DECAY_RATE);
      m_rmsR = std::max(-60.0, m_rmsR - DECAY_RATE);
      m_peakL = std::max(-60.0, m_peakL - DECAY_RATE * 0.5);
      m_peakR = std::max(-60.0, m_peakR - DECAY_RATE * 0.5);
      m_peakHoldL = std::max(-60.0, m_peakHoldL - DECAY_RATE * 0.3);
      m_peakHoldR = std::max(-60.0, m_peakHoldR - DECAY_RATE * 0.3);
    }
    m_wasPlaying = false;
    return;
  }
  m_wasPlaying = true;

  double targetRmsL = -60.0, targetRmsR = -60.0;
  double targetPeakL = -60.0, targetPeakR = -60.0;

  if (nch < 1 || sampleRate <= 0 || audio.empty()) {
    m_rmsL = m_rmsR = m_peakL = m_peakR = -60.0;
    return;
  }

  int totalFrames = static_cast<int>(audio.size()) / nch;
  startFrame = std::max(0, startFrame);
  endFrame = std::min(totalFrames, endFrame);
  if (startFrame >= endFrame) {
    m_rmsL = std::max(-60.0, m_rmsL - DECAY_RATE);
    m_rmsR = std::max(-60.0, m_rmsR - DECAY_RATE);
    m_peakL = std::max(-60.0, m_peakL - DECAY_RATE * 0.5);
    m_peakR = std::max(-60.0, m_peakR - DECAY_RATE * 0.5);
    return;
  }

  int frames = endFrame - startFrame;
  double sumL = 0.0, sumR = 0.0;
  double pkL = 0.0, pkR = 0.0;

  int startIdx = startFrame * nch;
  int endIdx = std::min(endFrame * nch, (int)audio.size());

  for (int i = startIdx; i < endIdx; i += nch) {
    double sL = audio[i] * itemVol;
    double aL = std::abs(sL);
    sumL += sL * sL;
    if (aL > pkL) pkL = aL;

    if (nch >= 2) {
      double sR = audio[i + 1] * itemVol;
      double aR = std::abs(sR);
      sumR += sR * sR;
      if (aR > pkR) pkR = aR;
    }
  }

  double rmsLinL = std::sqrt(sumL / frames);
  targetRmsL = (rmsLinL > 1e-10) ? 20.0 * std::log10(rmsLinL) : -60.0;
  targetPeakL = (pkL > 1e-10) ? 20.0 * std::log10(pkL) : -60.0;

  if (nch >= 2) {
    double rmsLinR = std::sqrt(sumR / frames);
    targetRmsR = (rmsLinR > 1e-10) ? 20.0 * std::log10(rmsLinR) : -60.0;
    targetPeakR = (pkR > 1e-10) ? 20.0 * std::log10(pkR) : -60.0;
  } else {
    targetRmsR = targetRmsL;
    targetPeakR = targetPeakL;
  }

  // Zero out muted channels
  if (channelActive) {
    if (!channelActive[0]) { targetRmsL = -60.0; targetPeakL = -60.0; }
    if (nch >= 2 && !channelActive[1]) { targetRmsR = -60.0; targetPeakR = -60.0; }
  }

  // Clamp
  targetRmsL = std::max(-60.0, std::min(0.0, targetRmsL));
  targetRmsR = std::max(-60.0, std::min(0.0, targetRmsR));
  targetPeakL = std::max(-60.0, std::min(0.0, targetPeakL));
  targetPeakR = std::max(-60.0, std::min(0.0, targetPeakR));

  // Smoothing: fast attack, slow decay
  auto smooth = [](double current, double target, double decay) -> double {
    if (target > current) return target; // instant attack
    return std::max(target, current - decay); // slow decay
  };

  m_rmsL = smooth(m_rmsL, targetRmsL, DECAY_RATE);
  m_rmsR = smooth(m_rmsR, targetRmsR, DECAY_RATE);
  m_peakL = smooth(m_peakL, targetPeakL, DECAY_RATE * 0.5);
  m_peakR = smooth(m_peakR, targetPeakR, DECAY_RATE * 0.5);

  // Peak hold — tracks instantaneous peak, holds briefly then follows
  if (targetPeakL >= m_peakHoldL) { m_peakHoldL = targetPeakL; m_peakHoldCountL = 0; }
  else if (++m_peakHoldCountL > PEAK_HOLD_TICKS) { m_peakHoldL = std::max(targetPeakL, m_peakHoldL - DECAY_RATE * 0.5); }

  if (targetPeakR >= m_peakHoldR) { m_peakHoldR = targetPeakR; m_peakHoldCountR = 0; }
  else if (++m_peakHoldCountR > PEAK_HOLD_TICKS) { m_peakHoldR = std::max(targetPeakR, m_peakHoldR - DECAY_RATE * 0.5); }
}

static int DbToX(double db, int barLeft, int barWidth)
{
  double frac = (db + 60.0) / 60.0;
  frac = std::max(0.0, std::min(1.0, frac));
  return barLeft + static_cast<int>(frac * barWidth);
}

static COLORREF MeterColor(double db)
{
  if (db > -6.0) return RGB(220, 50, 50);
  if (db > -18.0) return RGB(220, 200, 50);
  return RGB(50, 200, 80);
}

void LevelsPanel::Draw(HDC hdc, RECT rect, int nch)
{
  int numCh = (nch >= 2) ? 2 : 1;
  int scaleH = 12; // height for dB scale labels
  int barsH = rect.bottom - rect.top - scaleH;
  int gap = 2;
  int barH = (numCh == 2) ? (barsH - gap) / 2 : barsH;

  int labelW = 14;
  int barLeft = rect.left + labelW;
  int barRight = rect.right - 2;
  int barWidth = barRight - barLeft;
  if (barWidth < 10) return;

  // Dark background
  HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
  FillRect(hdc, &rect, bg);
  DeleteObject(bg);

  SetBkMode(hdc, TRANSPARENT);

  double rms[2] = { m_rmsL, m_rmsR };
  double peak[2] = { m_peakHoldL, m_peakHoldR };
  const char* labels[2] = { "L", "R" };
  if (numCh == 1) labels[0] = "M";

  for (int ch = 0; ch < numCh; ch++) {
    int yTop = rect.top + ch * (barH + gap);
    int yBot = yTop + barH;

    // Channel label
    SetTextColor(hdc, RGB(140, 140, 140));
    RECT lblRect = { rect.left, yTop, barLeft - 1, yBot };
    DrawText(hdc, labels[ch], -1, &lblRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Bar background
    RECT barBg = { barLeft, yTop, barRight, yBot };
    HBRUSH barBgBrush = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hdc, &barBg, barBgBrush);
    DeleteObject(barBgBrush);

    // RMS bar — gradient segments
    int rmsX = DbToX(rms[ch], barLeft, barWidth);
    if (rmsX > barLeft) {
      int greenEnd = std::min(DbToX(-18.0, barLeft, barWidth), rmsX);
      if (greenEnd > barLeft) {
        RECT seg = { barLeft, yTop, greenEnd, yBot };
        HBRUSH br = CreateSolidBrush(RGB(40, 160, 60));
        FillRect(hdc, &seg, br);
        DeleteObject(br);
      }
      int yelStart = DbToX(-18.0, barLeft, barWidth);
      if (rmsX > yelStart) {
        int yelEnd = std::min(DbToX(-6.0, barLeft, barWidth), rmsX);
        if (yelEnd > yelStart) {
          RECT seg = { yelStart, yTop, yelEnd, yBot };
          HBRUSH br = CreateSolidBrush(RGB(190, 170, 30));
          FillRect(hdc, &seg, br);
          DeleteObject(br);
        }
      }
      int redStart = DbToX(-6.0, barLeft, barWidth);
      if (rmsX > redStart) {
        RECT seg = { redStart, yTop, rmsX, yBot };
        HBRUSH br = CreateSolidBrush(RGB(200, 40, 40));
        FillRect(hdc, &seg, br);
        DeleteObject(br);
      }
    }

    // Peak hold indicator
    int peakX = DbToX(peak[ch], barLeft, barWidth);
    if (peakX > barLeft + 2) {
      COLORREF peakCol = MeterColor(peak[ch]);
      HPEN peakPen = CreatePen(PS_SOLID, 2, peakCol);
      HPEN prevPen = (HPEN)SelectObject(hdc, peakPen);
      MoveToEx(hdc, peakX, yTop, nullptr);
      LineTo(hdc, peakX, yBot);
      SelectObject(hdc, prevPen);
      DeleteObject(peakPen);
    }

    // dB tick marks on bars
    static const double ticks[] = { -48, -36, -24, -18, -12, -6, -3, 0 };
    HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
    HPEN prevTick = (HPEN)SelectObject(hdc, tickPen);
    for (double db : ticks) {
      int tx = DbToX(db, barLeft, barWidth);
      MoveToEx(hdc, tx, yTop, nullptr);
      LineTo(hdc, tx, yBot);
    }
    SelectObject(hdc, prevTick);
    DeleteObject(tickPen);
  }

  // dB scale labels row below bars
  int scaleY = rect.bottom - scaleH;
  SetTextColor(hdc, RGB(130, 130, 130));
  // "dB" label
  RECT dbLbl = { rect.left, scaleY, barLeft - 1, rect.bottom };
  DrawText(hdc, "dB", -1, &dbLbl, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  static const double scaleMarks[] = { -54, -48, -42, -36, -30, -24, -18, -12, -6, -3, 0 };
  HPEN sPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
  HPEN prevScale = (HPEN)SelectObject(hdc, sPen);
  for (double db : scaleMarks) {
    int x = DbToX(db, barLeft, barWidth);
    char buf[8];
    snprintf(buf, sizeof(buf), "%.0f", db);
    // Small tick line from bar bottom to scale
    MoveToEx(hdc, x, scaleY, nullptr);
    LineTo(hdc, x, scaleY + 3);

    RECT tr = { x - 14, scaleY + 2, x + 14, rect.bottom };
    DrawText(hdc, buf, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  SelectObject(hdc, prevScale);
  DeleteObject(sPen);
}
