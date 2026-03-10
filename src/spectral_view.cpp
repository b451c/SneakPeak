// spectral_view.cpp — Pre-computed spectrogram + instant SWELL bitmap rendering
#include "spectral_view.h"
#include "waveform_view.h"
#include "config.h"
#include "globals.h"
#include "theme.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float s_hann[SpectralView::FFT_SIZE];
static std::once_flag s_hannFlag;

static void InitHann()
{
  std::call_once(s_hannFlag, []() {
    for (int i = 0; i < SpectralView::FFT_SIZE; i++)
      s_hann[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * i / (SpectralView::FFT_SIZE - 1))));
  });
}

static void DoFFT(float* re, float* im, int N)
{
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= N; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wRe = cosf(ang), wIm = sinf(ang);
    for (int i = 0; i < N; i += len) {
      float cRe = 1.0f, cIm = 0.0f;
      int half = len / 2;
      for (int j = 0; j < half; j++) {
        int a = i + j, b = i + j + half;
        float tRe = re[b] * cRe - im[b] * cIm;
        float tIm = re[b] * cIm + im[b] * cRe;
        re[b] = re[a] - tRe; im[b] = im[a] - tIm;
        re[a] += tRe; im[a] += tIm;
        float nRe = cRe * wRe - cIm * wIm;
        cIm = cRe * wIm + cIm * wRe;
        cRe = nRe;
      }
    }
  }
}

SpectralView::SpectralView()
{
  for (int i = 0; i < 256; i++) {
    m_colorLUT[i] = MagmaColor((float)i / 255.0f);
    m_colorLUT_Native[i] = (unsigned int)m_colorLUT[i];
  }
  InitHann();
}

SpectralView::~SpectralView()
{
  m_cancelRequested.store(true);
  if (m_computeThread.joinable())
    m_computeThread.join();
  if (m_memDC) SWELL_DeleteGfxContext(m_memDC);
}

void SpectralView::SetRect(int x, int y, int w, int h)
{
  RECT r = { x, y, x + w, y + h };
  if (memcmp(&r, &m_rect, sizeof(RECT)) != 0) {
    m_rect = r;
    m_renderValid.store(false);
  }
}

COLORREF SpectralView::MagmaColor(float t)
{
  t = std::max(0.0f, std::min(1.0f, t));
  struct S { float p; int r, g, b; };
  static const S s[] = {
    {0.00f,  2, 2, 10}, {0.15f, 40, 5, 80}, {0.35f,140,20,50},
    {0.55f,210,60,15},  {0.75f,245,160,20}, {0.90f,252,230,80},
    {1.00f,255,255,200}
  };
  int seg = 5;
  for (int i = 1; i < 7; i++) { if (t <= s[i].p) { seg = i - 1; break; } }
  float f = (s[seg+1].p - s[seg].p > 0) ? (t - s[seg].p) / (s[seg+1].p - s[seg].p) : 0;
  f = std::max(0.0f, std::min(1.0f, f));
  return RGB((int)(s[seg].r + f*(float)(s[seg+1].r-s[seg].r)),
             (int)(s[seg].g + f*(float)(s[seg+1].g-s[seg].g)),
             (int)(s[seg].b + f*(float)(s[seg+1].b-s[seg].b)));
}

void SpectralView::ClearSpectrum()
{
  // Cancel and wait for any running computation
  m_cancelRequested.store(true);
  if (m_computeThread.joinable())
    m_computeThread.join();
  m_cancelRequested.store(false);

  std::lock_guard<std::mutex> lock(m_specMutex);
  m_specData.clear();
  m_specCols = 0;
  m_specValid.store(false);
  m_computing.store(false);
  m_progress.store(0.0f);
  m_renderValid.store(false);
}

// Launch async spectrogram computation — returns immediately
void SpectralView::PrecomputeSpectrum(const WaveformView& waveform)
{
  // If already computing, don't re-launch
  if (m_computing.load()) return;

  ClearSpectrum();

  int nch = waveform.GetNumChannels();
  int sr = waveform.GetSampleRate();
  const auto& audio = waveform.GetAudioData();
  int totalFrames = (int)audio.size() / std::max(1, nch);

  if (nch < 1 || sr <= 0 || audio.empty()) return;

  // Store metadata (safe — thread not running yet)
  m_specNch = nch;
  m_specSr = sr;
  m_specDuration = (double)totalFrames / (double)sr;
  m_specCols = (totalFrames + HOP_SIZE - 1) / HOP_SIZE;
  if (m_specCols < 1) return;

  // Copy audio data for the thread (avoid referencing waveform from thread)
  std::vector<double> audioCopy(audio.begin(), audio.end());

  m_computing.store(true);
  m_progress.store(0.0f);

  m_computeThread = std::thread(&SpectralView::ComputeThreadFunc, this,
                                 std::move(audioCopy), nch, sr, totalFrames);
}

// Background thread: computes FFT spectrogram
void SpectralView::ComputeThreadFunc(std::vector<double> audio, int nch, int sr, int totalFrames)
{
  int specCols = (totalFrames + HOP_SIZE - 1) / HOP_SIZE;

  // Allocate result buffer locally
  std::vector<unsigned char> specData((size_t)specCols * (size_t)nch * FFT_HALF, 0);

  float re[FFT_SIZE], im[FFT_SIZE];

  for (int fc = 0; fc < specCols; fc++) {
    // Check for cancellation periodically
    if (m_cancelRequested.load()) {
      m_computing.store(false);
      return;
    }

    int centerFrame = fc * HOP_SIZE;

    for (int ch = 0; ch < nch; ch++) {
      for (int i = 0; i < FFT_SIZE; i++) {
        int frame = centerFrame - FFT_SIZE / 2 + i;
        re[i] = (frame >= 0 && frame < totalFrames)
          ? (float)audio[(size_t)frame * (size_t)nch + (size_t)ch] * s_hann[i]
          : 0.0f;
        im[i] = 0.0f;
      }
      DoFFT(re, im, FFT_SIZE);

      unsigned char* dst = &specData[((size_t)fc * (size_t)nch + (size_t)ch) * FFT_HALF];
      for (int bin = 0; bin < FFT_HALF; bin++) {
        float m = sqrtf(re[bin]*re[bin] + im[bin]*im[bin]);
        float db = 20.0f * log10f(m / (float)FFT_SIZE + 1e-10f);
        float norm = (db + 90.0f) / 90.0f;
        dst[bin] = (unsigned char)(std::max(0.0f, std::min(1.0f, norm)) * 255.0f);
      }
    }

    m_progress.store((float)(fc + 1) / (float)specCols);
  }

  // Hand off result to main thread
  {
    std::lock_guard<std::mutex> lock(m_specMutex);
    m_specData = std::move(specData);
    m_specCols = specCols;
  }

  m_specValid.store(true);
  m_computing.store(false);
  m_renderValid.store(false);
}

// Render visible portion from pre-computed data (optimized)
void SpectralView::RenderView(const WaveformView& waveform)
{
  int width = m_rect.right - m_rect.left - DB_SCALE_WIDTH;
  int height = m_rect.bottom - m_rect.top;
  if (width < 1 || height < 1) return;

  double viewStart = waveform.GetViewStart();
  double viewDur = waveform.GetViewDuration();

  if (m_renderValid.load() && m_cachedViewStart == viewStart &&
      m_cachedViewDuration == viewDur &&
      m_cachedWidth == width && m_cachedHeight == height) return;

  m_pixW = width;
  m_pixH = height;
  m_pixels.assign((size_t)width * (size_t)height, 0);

  if (!m_specValid.load() || m_specCols < 1) goto done;

  {
    std::lock_guard<std::mutex> lock(m_specMutex);
    int nch = m_specNch;
    int chSep = (nch > 1) ? CHANNEL_SEPARATOR_HEIGHT : 0;
    int chH = (nch > 1) ? (height - chSep) / 2 : height;

    double colTime = (double)HOP_SIZE / (double)m_specSr;
    double nyquist = (double)m_specSr / 2.0;
    double fMin = std::min(FREQ_MIN, nyquist * 0.5);
    double fMax = std::min(FREQ_MAX, nyquist);
    double logRatio = fMax / fMin;

    // Pre-compute frequency→bin LUT for each pixel row (shared across columns)
    std::vector<int> binLow(chH);
    std::vector<int> binHigh(chH);
    std::vector<float> binFrac(chH);

    for (int py = 0; py < chH; py++) {
      double pyFrac = (double)py / (double)chH;
      double freq = fMin * pow(logRatio, pyFrac);
      float binF = (float)(freq / nyquist * (double)(FFT_HALF - 1));
      binLow[py] = (int)binF;
      binHigh[py] = std::min(binLow[py] + 1, FFT_HALF - 1);
      binFrac[py] = binF - (float)binLow[py];
    }

    for (int px = 0; px < width; px++) {
      double t = viewStart + (viewDur * px) / width;
      float specCol = (float)(t / colTime);
      int sc0 = std::max(0, std::min(m_specCols - 1, (int)specCol));
      int sc1 = std::max(0, std::min(m_specCols - 1, sc0 + 1));
      float hFrac = specCol - (float)(int)specCol;

      for (int ch = 0; ch < nch; ch++) {
        int chTop = ch * (chH + chSep);
        const unsigned char* col0 = &m_specData[((size_t)sc0 * (size_t)nch + (size_t)ch) * FFT_HALF];
        const unsigned char* col1 = &m_specData[((size_t)sc1 * (size_t)nch + (size_t)ch) * FFT_HALF];

        for (int py = 0; py < chH; py++) {
          int screenY = chTop + chH - 1 - py;
          if (screenY < 0 || screenY >= height) continue;

          int b0 = binLow[py], b1 = binHigh[py];
          float vf = binFrac[py];

          // Bilinear interpolation
          float top = (float)col0[b0] * (1.0f - hFrac) + (float)col1[b0] * hFrac;
          float bot = (float)col0[b1] * (1.0f - hFrac) + (float)col1[b1] * hFrac;
          float val = top * (1.0f - vf) + bot * vf;

          m_pixels[(size_t)px * (size_t)height + (size_t)screenY] =
            (unsigned char)std::max(0.0f, std::min(255.0f, val));
        }
      }
    }
  }

done:
  m_renderValid.store(true);
  m_cachedViewStart = viewStart;
  m_cachedViewDuration = viewDur;
  m_cachedWidth = width;
  m_cachedHeight = height;
}

// --- Frequency scale (Audition-style) ---

void SpectralView::DrawFreqScale(HDC hdc, int yTop, int height, int sampleRate)
{
  int scaleLeft = m_rect.right - DB_SCALE_WIDTH;
  int scaleRight = m_rect.right;

  RECT colRect = { scaleLeft, yTop, scaleRight, yTop + height };
  HBRUSH colBg = CreateSolidBrush(RGB(30, 30, 30));
  FillRect(hdc, &colRect, colBg);
  DeleteObject(colBg);

  HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, scaleLeft, yTop, nullptr);
  LineTo(hdc, scaleLeft, yTop + height);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(hdc, TRANSPARENT);

  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal10);

  double nyquist = sampleRate / 2.0;
  double fMin = std::min(FREQ_MIN, nyquist * 0.5);
  double fMax = std::min(FREQ_MAX, nyquist);

  // Full range labels: low frequencies + kHz range
  struct FreqLabel { double freq; const char* text; bool bold; };
  static const FreqLabel labels[] = {
    {    50, "50",   false },
    {   100, "100",  false },
    {   200, "200",  false },
    {   500, "500",  false },
    {  1000, "1k",   false },
    {  2000, "2k",   false },
    {  4000, "4k",   false },
    {  6000, "6k",   false },
    {  8000, "8k",   false },
    { 10000, "10k",  true  },
    { 12000, "12k",  false },
    { 14000, "14k",  false },
    { 16000, "16k",  false },
    { 18000, "18k",  false },
    { 20000, "20k",  false },
  };

  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
  int lastDrawnY = yTop + height + 20;

  for (const auto& lab : labels) {
    if (lab.freq < fMin || lab.freq > fMax) continue;

    int y = FreqToY(lab.freq, yTop, height);
    if (y < yTop + 1 || y > yTop + height - 2) continue;
    if (lastDrawnY - y < 11) continue;

    // Tick
    oldPen = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, scaleLeft + 1, y, nullptr);
    LineTo(hdc, scaleLeft + 5, y);
    SelectObject(hdc, oldPen);

    // Label
    if (lab.bold) {
      SelectObject(hdc, g_fonts.bold10);
      SetTextColor(hdc, RGB(190, 190, 190));
    } else {
      SelectObject(hdc, g_fonts.normal10);
      SetTextColor(hdc, RGB(150, 150, 150));
    }

    int labelTop = y - 5, labelBot = y + 5;
    if (y - yTop < 5) { labelTop = yTop; labelBot = yTop + 11; }
    else if (yTop + height - y < 5) { labelTop = yTop + height - 11; labelBot = yTop + height; }

    RECT tr = { scaleLeft + 6, labelTop, scaleRight - 2, labelBot };
    DrawText(hdc, lab.text, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    lastDrawnY = y;
  }

  DeleteObject(tickPen);
  SelectObject(hdc, oldFont);
}

// --- Playhead & cursor drawing ---

void SpectralView::DrawPlayhead(HDC hdc, const WaveformView& waveform)
{
  int contentRight = m_rect.right - DB_SCALE_WIDTH;

  bool isPlaying = g_GetPlayState && (g_GetPlayState() & 1);

  if (!isPlaying) {
    // Stopped: solid line at edit cursor
    int cx = waveform.TimeToX(waveform.GetCursorTime());
    if (cx > m_rect.left && cx < contentRight) {
      HPEN pen = CreatePen(PS_SOLID, 2, g_theme.editCursor);
      HPEN old = (HPEN)SelectObject(hdc, pen);
      MoveToEx(hdc, cx, m_rect.top, nullptr);
      LineTo(hdc, cx, m_rect.bottom);
      SelectObject(hdc, old);
      DeleteObject(pen);
    }
  } else {
    // Playing: dashed edit cursor + solid playhead

    // Edit cursor — dashed
    int cx = waveform.TimeToX(waveform.GetCursorTime());
    if (cx > m_rect.left && cx < contentRight) {
      HPEN pen = CreatePen(PS_SOLID, 1, g_theme.editCursor);
      HPEN old = (HPEN)SelectObject(hdc, pen);
      for (int dy = m_rect.top; dy < m_rect.bottom; dy += 6) {
        MoveToEx(hdc, cx, dy, nullptr);
        LineTo(hdc, cx, std::min(dy + 3, (int)m_rect.bottom));
      }
      SelectObject(hdc, old);
      DeleteObject(pen);
    }

    // Playhead — solid moving line
    if (g_GetPlayPosition2) {
      double relPos = waveform.AbsTimeToRelTime(g_GetPlayPosition2());
      int px = waveform.TimeToX(relPos);
      if (px > m_rect.left && px < contentRight) {
        HPEN pen = CreatePen(PS_SOLID, 2, g_theme.playhead);
        HPEN old = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, px, m_rect.top, nullptr);
        LineTo(hdc, px, m_rect.bottom);
        SelectObject(hdc, old);
        DeleteObject(pen);
      }
    }
  }
}

// --- Selection overlay ---

void SpectralView::DrawSelection(HDC hdc, const WaveformView& waveform)
{
  if (!waveform.HasSelection()) return;

  double s1 = std::min(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
  double s2 = std::max(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
  int x1 = waveform.TimeToX(s1);
  int x2 = waveform.TimeToX(s2);
  int lim = m_rect.right - DB_SCALE_WIDTH;

  // Selection boundary lines
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
  HPEN old = (HPEN)SelectObject(hdc, pen);
  if (x1 > m_rect.left && x1 < lim) {
    MoveToEx(hdc, x1, m_rect.top, nullptr);
    LineTo(hdc, x1, m_rect.bottom);
  }
  if (x2 > m_rect.left && x2 < lim && x2 != x1) {
    MoveToEx(hdc, x2, m_rect.top, nullptr);
    LineTo(hdc, x2, m_rect.bottom);
  }
  SelectObject(hdc, old);
  DeleteObject(pen);

}

// --- Frequency selection overlay ---

void SpectralView::DrawFreqSelectionOverlay(HDC hdc, const WaveformView& waveform)
{
  if (!m_freqSelActive) return;

  int nch = waveform.GetNumChannels();
  int height = m_rect.bottom - m_rect.top;
  int chSep = (nch > 1) ? CHANNEL_SEPARATOR_HEIGHT : 0;
  int chH = (nch > 1) ? (height - chSep) / 2 : height;
  int contentRight = m_rect.right - DB_SCALE_WIDTH;

  double fLow = GetFreqSelLow();
  double fHigh = GetFreqSelHigh();

  HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 200, 255));
  HPEN old = (HPEN)SelectObject(hdc, pen);

  for (int ch = 0; ch < nch; ch++) {
    int chTop = m_rect.top + ch * (chH + chSep);
    int yLow = FreqToY(fLow, chTop, chH);
    int yHigh = FreqToY(fHigh, chTop, chH);
    if (yLow < yHigh) std::swap(yLow, yHigh); // yHigh is higher freq = lower y

    // Horizontal lines at freq boundaries
    MoveToEx(hdc, m_rect.left, yHigh, nullptr);
    LineTo(hdc, contentRight, yHigh);
    MoveToEx(hdc, m_rect.left, yLow, nullptr);
    LineTo(hdc, contentRight, yLow);
  }

  SelectObject(hdc, old);
  DeleteObject(pen);
}

// --- Y ↔ Frequency conversion ---

double SpectralView::YToFreq(int y, int channelTop, int channelHeight) const
{
  if (channelHeight <= 0) return FREQ_MIN;
  double nyquist = m_specSr > 0 ? (double)m_specSr / 2.0 : 22050.0;
  double fMin = std::min(FREQ_MIN, nyquist * 0.5);
  double fMax = std::min(FREQ_MAX, nyquist);

  // y increases downward, low y = high freq
  double pyFrac = 1.0 - (double)(y - channelTop) / (double)channelHeight;
  pyFrac = std::max(0.0, std::min(1.0, pyFrac));
  return fMin * pow(fMax / fMin, pyFrac);
}

int SpectralView::FreqToY(double freqHz, int channelTop, int channelHeight) const
{
  if (channelHeight <= 0) return channelTop;
  double nyquist = m_specSr > 0 ? (double)m_specSr / 2.0 : 22050.0;
  double fMin = std::min(FREQ_MIN, nyquist * 0.5);
  double fMax = std::min(FREQ_MAX, nyquist);

  freqHz = std::max(fMin, std::min(fMax, freqHz));
  double rowFrac = log(freqHz / fMin) / log(fMax / fMin);
  return channelTop + channelHeight - 1 - (int)(rowFrac * (channelHeight - 1));
}

// --- Frequency selection ---

double SpectralView::GetFreqSelLow() const
{
  return std::min(m_freqSelStart, m_freqSelEnd);
}

double SpectralView::GetFreqSelHigh() const
{
  return std::max(m_freqSelStart, m_freqSelEnd);
}

void SpectralView::StartFreqSelection(double freqHz)
{
  m_freqSelActive = true;
  m_freqSelStart = freqHz;
  m_freqSelEnd = freqHz;
}

void SpectralView::UpdateFreqSelection(double freqHz)
{
  m_freqSelEnd = freqHz;
}

void SpectralView::ClearFreqSelection()
{
  m_freqSelActive = false;
  m_freqSelStart = 0.0;
  m_freqSelEnd = 0.0;
}

// --- Loading overlay ---

void SpectralView::DrawLoadingOverlay(HDC hdc)
{
  // Dark background
  HBRUSH bg = CreateSolidBrush(RGB(10, 10, 15));
  FillRect(hdc, &m_rect, bg);
  DeleteObject(bg);

  int cx = (m_rect.left + m_rect.right) / 2;
  int cy = (m_rect.top + m_rect.bottom) / 2;

  SetBkMode(hdc, TRANSPARENT);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal13);

  // Progress bar
  float pct = m_progress.load();
  int barW = std::min(200, (m_rect.right - m_rect.left) - 40);
  int barH = 4;
  int barL = cx - barW / 2;
  int barT = cy + 10;

  // Track
  RECT trackRect = { barL, barT, barL + barW, barT + barH };
  HBRUSH trackBr = CreateSolidBrush(RGB(40, 40, 50));
  FillRect(hdc, &trackRect, trackBr);
  DeleteObject(trackBr);

  // Fill
  int fillW = (int)(pct * (float)barW);
  if (fillW > 0) {
    RECT fillRect = { barL, barT, barL + fillW, barT + barH };
    HBRUSH fillBr = CreateSolidBrush(RGB(80, 140, 220));
    FillRect(hdc, &fillRect, fillBr);
    DeleteObject(fillBr);
  }

  // Text
  char text[32];
  snprintf(text, sizeof(text), "Computing spectrum... %d%%", (int)(pct * 100.0f));
  SetTextColor(hdc, RGB(140, 160, 200));
  RECT tr = { m_rect.left, cy - 10, m_rect.right, cy + 8 };
  DrawText(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectObject(hdc, oldFont);
}

// --- Main paint ---

void SpectralView::Paint(HDC hdc, const WaveformView& waveform)
{
  int width = m_rect.right - m_rect.left;
  int height = m_rect.bottom - m_rect.top;
  if (width < 10 || height < 10) return;

  if (!waveform.HasItem() || waveform.GetAudioData().empty()) {
    HBRUSH bg = CreateSolidBrush(RGB(5, 5, 10));
    FillRect(hdc, &m_rect, bg);
    DeleteObject(bg);
    return;
  }

  // Launch async computation if not started
  if (!m_specValid.load() && !m_computing.load())
    PrecomputeSpectrum(waveform);

  // Show loading overlay while computing
  if (m_computing.load()) {
    DrawLoadingOverlay(hdc);
    return;
  }

  // Not ready yet (shouldn't happen, but guard)
  if (!m_specValid.load()) return;

  RenderView(waveform);

  int contentW = width - DB_SCALE_WIDTH;
  if (m_pixW < 1 || m_pixH < 1) return;

  // Fast SWELL bitmap rendering
  if (!m_memDC || m_memW != contentW || m_memH != height) {
    if (m_memDC) SWELL_DeleteGfxContext(m_memDC);
    m_memDC = SWELL_CreateMemContext(hdc, contentW, height);
    m_memW = contentW;
    m_memH = height;
  }

  if (m_memDC) {
    unsigned int* fbuf = (unsigned int*)SWELL_GetCtxFrameBuffer(m_memDC);
    if (fbuf) {
      int cols = std::min(contentW, m_pixW);
      for (int col = 0; col < cols; col++) {
        const unsigned char* colData = &m_pixels[col * m_pixH];
        for (int row = 0; row < m_pixH; row++)
          fbuf[row * contentW + col] = m_colorLUT_Native[colData[row]];
      }
      BitBlt(hdc, m_rect.left, m_rect.top, contentW, height, m_memDC, 0, 0, SRCCOPY);
    }
  }

  // Freq scale (per channel)
  int nch = waveform.GetNumChannels();
  int sr = waveform.GetSampleRate();
  if (nch > 1) {
    int chH = (height - CHANNEL_SEPARATOR_HEIGHT) / 2;
    DrawFreqScale(hdc, m_rect.top, chH, sr);
    DrawFreqScale(hdc, m_rect.top + chH + CHANNEL_SEPARATOR_HEIGHT, chH, sr);
  } else {
    DrawFreqScale(hdc, m_rect.top, height, sr);
  }

  // Selection overlay
  DrawSelection(hdc, waveform);

  // Frequency band selection
  DrawFreqSelectionOverlay(hdc, waveform);

  // Playhead / edit cursor (matches waveform behavior exactly)
  DrawPlayhead(hdc, waveform);
}
