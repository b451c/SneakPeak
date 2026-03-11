// levels_panel.cpp — Peak/RMS/VU level meter
#include "levels_panel.h"
#include "theme.h"
#include <cmath>
#include <algorithm>

LevelsPanel::~LevelsPanel()
{
  DestroyGdiCache();
}

void LevelsPanel::CreateGdiCache()
{
  DestroyGdiCache();

  m_gdi.bgBrush    = CreateSolidBrush(RGB(20, 20, 20));
  m_gdi.barBgBrush = CreateSolidBrush(RGB(30, 30, 30));
  m_gdi.tickPen    = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
  m_gdi.scalePen   = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));

  switch (m_mode) {
    case MeterMode::PEAK:
      m_gdi.greenBrush  = CreateSolidBrush(RGB(50, 210, 90));
      m_gdi.yellowBrush = CreateSolidBrush(RGB(210, 190, 40));
      m_gdi.redBrush    = CreateSolidBrush(RGB(220, 50, 50));
      break;
    case MeterMode::VU:
      m_gdi.greenBrush  = CreateSolidBrush(RGB(30, 150, 70));
      m_gdi.yellowBrush = CreateSolidBrush(RGB(180, 160, 30));
      m_gdi.redBrush    = CreateSolidBrush(RGB(190, 40, 40));
      break;
    case MeterMode::RMS:
    default:
      m_gdi.greenBrush  = CreateSolidBrush(RGB(40, 160, 60));
      m_gdi.yellowBrush = CreateSolidBrush(RGB(190, 170, 30));
      m_gdi.redBrush    = CreateSolidBrush(RGB(200, 40, 40));
      break;
  }

  m_gdi.peakGreen  = CreatePen(PS_SOLID, 2, RGB(50, 200, 80));
  m_gdi.peakYellow = CreatePen(PS_SOLID, 2, RGB(220, 200, 50));
  m_gdi.peakRed    = CreatePen(PS_SOLID, 2, RGB(220, 50, 50));

  m_gdi.mode = m_mode;
  m_gdi.valid = true;
}

void LevelsPanel::DestroyGdiCache()
{
  if (m_gdi.bgBrush)    { DeleteObject(m_gdi.bgBrush);    m_gdi.bgBrush = nullptr; }
  if (m_gdi.barBgBrush) { DeleteObject(m_gdi.barBgBrush); m_gdi.barBgBrush = nullptr; }
  if (m_gdi.greenBrush) { DeleteObject(m_gdi.greenBrush); m_gdi.greenBrush = nullptr; }
  if (m_gdi.yellowBrush){ DeleteObject(m_gdi.yellowBrush);m_gdi.yellowBrush = nullptr; }
  if (m_gdi.redBrush)   { DeleteObject(m_gdi.redBrush);   m_gdi.redBrush = nullptr; }
  if (m_gdi.tickPen)    { DeleteObject(m_gdi.tickPen);    m_gdi.tickPen = nullptr; }
  if (m_gdi.scalePen)   { DeleteObject(m_gdi.scalePen);   m_gdi.scalePen = nullptr; }
  if (m_gdi.peakGreen)  { DeleteObject(m_gdi.peakGreen);  m_gdi.peakGreen = nullptr; }
  if (m_gdi.peakYellow) { DeleteObject(m_gdi.peakYellow); m_gdi.peakYellow = nullptr; }
  if (m_gdi.peakRed)    { DeleteObject(m_gdi.peakRed);    m_gdi.peakRed = nullptr; }
  m_gdi.valid = false;
}

const char* LevelsPanel::GetModeLabel() const
{
  switch (m_mode) {
    case MeterMode::PEAK: return "PPM";
    case MeterMode::RMS:  return "RMS";
    case MeterMode::VU:   return "VU";
  }
  return "RMS";
}

int LevelsPanel::GetIntegrationHalfWindow(int sampleRate) const
{
  switch (m_mode) {
    case MeterMode::PEAK: return sampleRate / 200;      // 5ms each side = 10ms (fast transients)
    case MeterMode::RMS:  return sampleRate * 3 / 20;   // 150ms each side = 300ms (AES/EBU)
    case MeterMode::VU:   return sampleRate * 3 / 20;   // 150ms each side = 300ms (VU standard)
  }
  return sampleRate * 3 / 20;
}

void LevelsPanel::UpdateFromTrackPeak(double peakLinL, double peakLinR, bool playing, int nch)
{
  double peakDecay = 0.3, peakHoldDecay = 0.3, decayRate = 0.5;

  if (!playing) {
    if (m_wasPlaying || m_barL > -60.0 || m_barR > -60.0) {
      m_barL = std::max(-60.0, m_barL - decayRate);
      m_barR = std::max(-60.0, m_barR - decayRate);
      m_peakL = std::max(-60.0, m_peakL - peakDecay);
      m_peakR = std::max(-60.0, m_peakR - peakDecay);
      m_peakHoldL = std::max(-60.0, m_peakHoldL - peakHoldDecay);
      m_peakHoldR = std::max(-60.0, m_peakHoldR - peakHoldDecay);
    }
    m_wasPlaying = false;
    return;
  }
  m_wasPlaying = true;

  double dbL = (peakLinL > 1e-10) ? 20.0 * std::log10(peakLinL) : -60.0;
  double dbR = (nch >= 2 && peakLinR > 1e-10) ? 20.0 * std::log10(peakLinR) : dbL;
  dbL = std::max(-60.0, std::min(0.0, dbL));
  dbR = std::max(-60.0, std::min(0.0, dbR));

  // Instant attack for bars (Track_GetPeakInfo already smoothed by REAPER)
  m_barL = dbL;
  m_barR = dbR;

  // Peak indicator
  if (dbL > m_peakL) m_peakL = dbL;
  else m_peakL = std::max(dbL, m_peakL - peakDecay);
  if (dbR > m_peakR) m_peakR = dbR;
  else m_peakR = std::max(dbR, m_peakR - peakDecay);

  // Peak hold
  if (dbL >= m_peakHoldL) { m_peakHoldL = dbL; m_peakHoldCountL = 0; }
  else if (++m_peakHoldCountL > PEAK_HOLD_TICKS) { m_peakHoldL = std::max(dbL, m_peakHoldL - peakHoldDecay); }
  if (dbR >= m_peakHoldR) { m_peakHoldR = dbR; m_peakHoldCountR = 0; }
  else if (++m_peakHoldCountR > PEAK_HOLD_TICKS) { m_peakHoldR = std::max(dbR, m_peakHoldR - peakHoldDecay); }
}

void LevelsPanel::Update(const std::vector<double>& audio, int startFrame,
                         int endFrame, int sampleRate, int nch, double itemVol, bool playing,
                         const bool* channelActive)
{
  // Mode-dependent ballistics (dB per tick at ~33ms)
  double attackRate, decayRate, peakDecay, peakHoldDecay;
  switch (m_mode) {
    case MeterMode::PEAK:
      attackRate = 999.0;
      decayRate = 0.5;
      peakDecay = 0.3;
      peakHoldDecay = 0.3;
      break;
    case MeterMode::VU:
      attackRate = 2.2;
      decayRate = 2.2;
      peakDecay = 0.3;
      peakHoldDecay = 0.3;
      break;
    case MeterMode::RMS:
    default:
      attackRate = 999.0;
      decayRate = 3.0;
      peakDecay = 1.5;
      peakHoldDecay = 1.5;
      break;
  }

  // When not playing, decay all meters to silence
  if (!playing) {
    if (m_wasPlaying || m_barL > -60.0 || m_barR > -60.0) {
      m_barL = std::max(-60.0, m_barL - decayRate);
      m_barR = std::max(-60.0, m_barR - decayRate);
      m_peakL = std::max(-60.0, m_peakL - peakDecay);
      m_peakR = std::max(-60.0, m_peakR - peakDecay);
      m_peakHoldL = std::max(-60.0, m_peakHoldL - peakHoldDecay);
      m_peakHoldR = std::max(-60.0, m_peakHoldR - peakHoldDecay);
    }
    m_wasPlaying = false;
    return;
  }
  m_wasPlaying = true;

  double targetBarL = -60.0, targetBarR = -60.0;
  double targetPeakL = -60.0, targetPeakR = -60.0;

  if (nch < 1 || sampleRate <= 0 || audio.empty()) {
    m_barL = m_barR = m_peakL = m_peakR = -60.0;
    return;
  }

  int totalFrames = static_cast<int>(audio.size()) / nch;
  startFrame = std::max(0, startFrame);
  endFrame = std::min(totalFrames, endFrame);
  if (startFrame >= endFrame) {
    m_barL = std::max(-60.0, m_barL - decayRate);
    m_barR = std::max(-60.0, m_barR - decayRate);
    m_peakL = std::max(-60.0, m_peakL - peakDecay);
    m_peakR = std::max(-60.0, m_peakR - peakDecay);
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

  // Compute target values based on mode
  double rmsLinL = std::sqrt(sumL / frames);
  double rmsLinR = (nch >= 2) ? std::sqrt(sumR / frames) : rmsLinL;

  switch (m_mode) {
    case MeterMode::PEAK:
      // Bar shows true peak
      targetBarL = (pkL > 1e-10) ? 20.0 * std::log10(pkL) : -60.0;
      targetBarR = (nch >= 2) ? ((pkR > 1e-10) ? 20.0 * std::log10(pkR) : -60.0) : targetBarL;
      break;
    case MeterMode::RMS:
    case MeterMode::VU:
    default:
      // Bar shows RMS
      targetBarL = (rmsLinL > 1e-10) ? 20.0 * std::log10(rmsLinL) : -60.0;
      targetBarR = (rmsLinR > 1e-10) ? 20.0 * std::log10(rmsLinR) : -60.0;
      break;
  }

  // Peak hold always tracks true peak
  targetPeakL = (pkL > 1e-10) ? 20.0 * std::log10(pkL) : -60.0;
  targetPeakR = (nch >= 2) ? ((pkR > 1e-10) ? 20.0 * std::log10(pkR) : -60.0) : targetPeakL;

  // Zero out muted channels
  if (channelActive) {
    if (!channelActive[0]) { targetBarL = -60.0; targetPeakL = -60.0; }
    if (nch >= 2 && !channelActive[1]) { targetBarR = -60.0; targetPeakR = -60.0; }
  }

  // Clamp
  targetBarL = std::max(-60.0, std::min(0.0, targetBarL));
  targetBarR = std::max(-60.0, std::min(0.0, targetBarR));
  targetPeakL = std::max(-60.0, std::min(0.0, targetPeakL));
  targetPeakR = std::max(-60.0, std::min(0.0, targetPeakR));

  // Smoothing per mode
  if (m_mode == MeterMode::VU) {
    // VU: same attack and decay rate (sluggish)
    auto vuSmooth = [attackRate, decayRate](double current, double target) -> double {
      if (target > current) return std::min(target, current + attackRate);
      return std::max(target, current - decayRate);
    };
    m_barL = vuSmooth(m_barL, targetBarL);
    m_barR = vuSmooth(m_barR, targetBarR);
  } else {
    // Peak & RMS: instant attack, slow decay
    auto smooth = [decayRate](double current, double target) -> double {
      if (target > current) return target;
      return std::max(target, current - decayRate);
    };
    m_barL = smooth(m_barL, targetBarL);
    m_barR = smooth(m_barR, targetBarR);
  }

  // Peak smoothing (always fast attack, slow decay)
  auto peakSmooth = [peakDecay](double current, double target) -> double {
    if (target > current) return target;
    return std::max(target, current - peakDecay);
  };
  m_peakL = peakSmooth(m_peakL, targetPeakL);
  m_peakR = peakSmooth(m_peakR, targetPeakR);

  // Peak hold
  if (targetPeakL >= m_peakHoldL) { m_peakHoldL = targetPeakL; m_peakHoldCountL = 0; }
  else if (++m_peakHoldCountL > PEAK_HOLD_TICKS) { m_peakHoldL = std::max(targetPeakL, m_peakHoldL - peakHoldDecay); }

  if (targetPeakR >= m_peakHoldR) { m_peakHoldR = targetPeakR; m_peakHoldCountR = 0; }
  else if (++m_peakHoldCountR > PEAK_HOLD_TICKS) { m_peakHoldR = std::max(targetPeakR, m_peakHoldR - peakHoldDecay); }
}

static int DbToX(double db, int barLeft, int barWidth)
{
  double frac = (db + 60.0) / 60.0;
  frac = std::max(0.0, std::min(1.0, frac));
  return barLeft + static_cast<int>(frac * barWidth);
}

void LevelsPanel::Draw(HDC hdc, RECT rect, int nch)
{
  if (!m_gdi.valid || m_gdi.mode != m_mode) CreateGdiCache();

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
  FillRect(hdc, &rect, m_gdi.bgBrush);

  SetBkMode(hdc, TRANSPARENT);

  double bar[2] = { m_barL, m_barR };
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
    FillRect(hdc, &barBg, m_gdi.barBgBrush);

    // Main bar — gradient segments
    int barX = DbToX(bar[ch], barLeft, barWidth);
    if (barX > barLeft) {
      int greenEnd = std::min(DbToX(-18.0, barLeft, barWidth), barX);
      if (greenEnd > barLeft) {
        RECT seg = { barLeft, yTop, greenEnd, yBot };
        FillRect(hdc, &seg, m_gdi.greenBrush);
      }
      int yelStart = DbToX(-18.0, barLeft, barWidth);
      if (barX > yelStart) {
        int yelEnd = std::min(DbToX(-6.0, barLeft, barWidth), barX);
        if (yelEnd > yelStart) {
          RECT seg = { yelStart, yTop, yelEnd, yBot };
          FillRect(hdc, &seg, m_gdi.yellowBrush);
        }
      }
      int redStart = DbToX(-6.0, barLeft, barWidth);
      if (barX > redStart) {
        RECT seg = { redStart, yTop, barX, yBot };
        FillRect(hdc, &seg, m_gdi.redBrush);
      }
    }

    // Peak hold indicator
    int peakX = DbToX(peak[ch], barLeft, barWidth);
    if (peakX > barLeft + 2) {
      HPEN peakPen = (peak[ch] > -6.0) ? m_gdi.peakRed
                   : (peak[ch] > -18.0) ? m_gdi.peakYellow
                   : m_gdi.peakGreen;
      HPEN prevPen = (HPEN)SelectObject(hdc, peakPen);
      MoveToEx(hdc, peakX, yTop, nullptr);
      LineTo(hdc, peakX, yBot);
      SelectObject(hdc, prevPen);
    }

    // dB tick marks on bars
    static const double ticks[] = { -48, -36, -24, -18, -12, -6, -3, 0 };
    HPEN prevTick = (HPEN)SelectObject(hdc, m_gdi.tickPen);
    for (double db : ticks) {
      int tx = DbToX(db, barLeft, barWidth);
      MoveToEx(hdc, tx, yTop, nullptr);
      LineTo(hdc, tx, yBot);
    }
    SelectObject(hdc, prevTick);
  }

  // dB scale labels row below bars
  int scaleY = rect.bottom - scaleH;
  SetTextColor(hdc, RGB(130, 130, 130));

  // Mode label instead of "dB"
  RECT dbLbl = { rect.left, scaleY, barLeft - 1, rect.bottom };
  DrawText(hdc, GetModeLabel(), -1, &dbLbl, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  static const double scaleMarks[] = { -54, -48, -42, -36, -30, -24, -18, -12, -6, -3, 0 };
  HPEN prevScale = (HPEN)SelectObject(hdc, m_gdi.scalePen);
  for (double db : scaleMarks) {
    int x = DbToX(db, barLeft, barWidth);
    char buf[8];
    snprintf(buf, sizeof(buf), "%.0f", db);
    MoveToEx(hdc, x, scaleY, nullptr);
    LineTo(hdc, x, scaleY + 3);

    RECT tr = { x - 14, scaleY + 2, x + 14, rect.bottom };
    DrawText(hdc, buf, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  SelectObject(hdc, prevScale);
}
