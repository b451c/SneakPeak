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
#ifdef _WIN32
  if (m_memBmp) DeleteObject(m_memBmp);
  if (m_memDC) DeleteDC(m_memDC);
#else
  if (m_memDC) SWELL_DeleteGfxContext(m_memDC);
#endif
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

// Background thread: computes the FFT spectrogram. Columns are independent, so
// the work is split across a small pool of sub-workers (the FFTs dominate), and
// channel PAIRS are packed into one complex transform (Z = chA + i*chB;
// X_A[k] = (Z[k]+conj(Z[N-k]))/2, X_B[k] = (Z[k]-conj(Z[N-k]))/(2i)), halving
// the FFT count on stereo. Together roughly an order of magnitude faster than
// the old single-threaded one-FFT-per-channel loop on typical machines.
void SpectralView::ComputeThreadFunc(std::vector<double> audio, int nch, int sr, int totalFrames)
{
  (void)sr;
  int specCols = (totalFrames + HOP_SIZE - 1) / HOP_SIZE;

  // Allocate result buffer locally; workers write disjoint column ranges.
  std::vector<unsigned char> specData((size_t)specCols * (size_t)nch * FFT_HALF, 0);

  std::atomic<int> colsDone{0};

  auto worker = [&](int c0, int c1) {
    std::vector<float> re((size_t)FFT_SIZE), im((size_t)FFT_SIZE);
    for (int fc = c0; fc < c1; fc++) {
      if (m_cancelRequested.load()) return;
      int centerFrame = fc * HOP_SIZE;

      auto gather = [&](int ch, float* dst) { // windowed frame of one channel
        for (int i = 0; i < FFT_SIZE; i++) {
          int frame = centerFrame - FFT_SIZE / 2 + i;
          dst[i] = (frame >= 0 && frame < totalFrames)
            ? (float)audio[(size_t)frame * (size_t)nch + (size_t)ch] * s_hann[i]
            : 0.0f;
        }
      };
      auto writeMag = [&](int ch, int bin, float xr, float xi) {
        float m = sqrtf(xr * xr + xi * xi);
        float db = 20.0f * log10f(m / (float)FFT_SIZE + 1e-10f);
        float norm = (db + 90.0f) / 90.0f;
        specData[((size_t)fc * (size_t)nch + (size_t)ch) * FFT_HALF + (size_t)bin] =
          (unsigned char)(std::max(0.0f, std::min(1.0f, norm)) * 255.0f);
      };

      for (int ch = 0; ch < nch; ch += 2) {
        if (ch + 1 < nch) {
          gather(ch, re.data());
          gather(ch + 1, im.data());
          DoFFT(re.data(), im.data(), FFT_SIZE);
          for (int bin = 0; bin < FFT_HALF; bin++) {
            int mirror = bin == 0 ? 0 : FFT_SIZE - bin;
            float a = re[bin], b = im[bin];
            float c = re[mirror], d = im[mirror];
            writeMag(ch,     bin, 0.5f * (a + c), 0.5f * (b - d));
            writeMag(ch + 1, bin, 0.5f * (b + d), 0.5f * (c - a));
          }
        } else { // odd channel count: plain real-in-re transform for the tail
          gather(ch, re.data());
          std::fill(im.begin(), im.end(), 0.0f);
          DoFFT(re.data(), im.data(), FFT_SIZE);
          for (int bin = 0; bin < FFT_HALF; bin++)
            writeMag(ch, bin, re[bin], im[bin]);
        }
      }

      int done = colsDone.fetch_add(1) + 1;
      if ((done & 31) == 0 || done == specCols)
        m_progress.store((float)done / (float)specCols);
    }
  };

  int hw = (int)std::thread::hardware_concurrency();
  int nWorkers = std::max(1, std::min(8, hw - 1));
  nWorkers = std::min(nWorkers, specCols);
  if (nWorkers <= 1) {
    worker(0, specCols);
  } else {
    std::vector<std::thread> pool;
    int chunk = (specCols + nWorkers - 1) / nWorkers;
    for (int w = 0; w < nWorkers; w++) {
      int c0 = w * chunk, c1 = std::min(specCols, c0 + chunk);
      if (c0 >= c1) break;
      pool.emplace_back(worker, c0, c1);
    }
    for (auto& th : pool) th.join();
  }

  if (m_cancelRequested.load()) {
    m_computing.store(false);
    return;
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
  int width = m_rect.right - m_rect.left - SP(DB_SCALE_WIDTH);
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
    int chSep = (nch > 1) ? SP(CHANNEL_SEPARATOR_HEIGHT) : 0;
    int chH = (nch > 1) ? (height - chSep) / 2 : height;

    double colTime = (double)HOP_SIZE / (double)m_specSr;
    double nyquist = (double)m_specSr / 2.0;
    double fMin = std::min(FREQ_MIN, nyquist * 0.5);
    double fMax = std::min(FREQ_MAX, nyquist);
    double logRatio = fMax / fMin;

    // Pre-compute frequency→bin LUTs for each pixel row (shared across columns):
    // center sample for the bilinear (zoom-in) path plus the row's bin SPAN for
    // the peak-preserving (zoom-out) path.
    std::vector<int> binLow(chH);
    std::vector<int> binHigh(chH);
    std::vector<float> binFrac(chH);
    std::vector<float> binEdge((size_t)chH + 1);

    for (int py = 0; py < chH; py++) {
      double pyFrac = (double)py / (double)chH;
      double freq = fMin * pow(logRatio, pyFrac);
      float binF = (float)(freq / nyquist * (double)(FFT_HALF - 1));
      binLow[py] = (int)binF;
      binHigh[py] = std::min(binLow[py] + 1, FFT_HALF - 1);
      binFrac[py] = binF - (float)binLow[py];
    }
    for (int py = 0; py <= chH; py++) {
      double pyFrac = (double)py / (double)chH;
      double freq = fMin * pow(logRatio, pyFrac);
      binEdge[(size_t)py] = (float)(freq / nyquist * (double)(FFT_HALF - 1));
    }

    for (int px = 0; px < width; px++) {
      double t = viewStart + (viewDur * px) / width;
      double tNext = viewStart + (viewDur * (px + 1)) / width;
      float specCol = (float)(t / colTime);
      int sc0 = std::max(0, std::min(m_specCols - 1, (int)specCol));
      int sc1 = std::max(0, std::min(m_specCols - 1, sc0 + 1));
      int scEnd = std::max(sc0, std::min(m_specCols - 1, (int)(tNext / colTime)));
      float hFrac = specCol - (float)(int)specCol;
      bool timeDown = (scEnd - sc0) >= 2; // pixel spans 2+ hop columns

      for (int ch = 0; ch < nch; ch++) {
        int chTop = ch * (chH + chSep);
        const unsigned char* col0 = &m_specData[((size_t)sc0 * (size_t)nch + (size_t)ch) * FFT_HALF];
        const unsigned char* col1 = &m_specData[((size_t)sc1 * (size_t)nch + (size_t)ch) * FFT_HALF];

        for (int py = 0; py < chH; py++) {
          int screenY = chTop + chH - 1 - py;
          if (screenY < 0 || screenY >= height) continue;

          int b0 = binLow[py], b1 = binHigh[py];
          int bSpanEnd = std::min(FFT_HALF - 1, (int)binEdge[(size_t)py + 1]);
          bool freqDown = (bSpanEnd - b0) >= 2; // row spans 2+ bins

          float val;
          if (timeDown) {
            // Time downsampling: take the MAX over the covered column x bin
            // block (peak-preserving, like the waveform peaks path) so narrow
            // clicks / thin tones stay visible zoomed out.
            unsigned char mx = 0;
            int bHi = std::max(b0, bSpanEnd);
            for (int c = sc0; c <= scEnd; c++) {
              const unsigned char* colp =
                &m_specData[((size_t)c * (size_t)nch + (size_t)ch) * FFT_HALF];
              for (int b = b0; b <= bHi; b++) mx = std::max(mx, colp[b]);
            }
            val = (float)mx;
          } else if (freqDown) {
            // Zoomed in but the row spans 2+ bins (upper log-scale region):
            // vertical peak per column + horizontal lerp between the two hop
            // columns - peak-true in frequency WITHOUT the hard column edges
            // a plain block max produces when a pixel sits between columns.
            int bHi = std::max(b0, bSpanEnd);
            unsigned char m0 = 0, m1 = 0;
            for (int b = b0; b <= bHi; b++) {
              m0 = std::max(m0, col0[b]);
              m1 = std::max(m1, col1[b]);
            }
            val = (float)m0 * (1.0f - hFrac) + (float)m1 * hFrac;
          } else {
            // Zoom-in: bilinear interpolation (smooth, unchanged behavior)
            float vf = binFrac[py];
            float top = (float)col0[b0] * (1.0f - hFrac) + (float)col1[b0] * hFrac;
            float bot = (float)col0[b1] * (1.0f - hFrac) + (float)col1[b1] * hFrac;
            val = top * (1.0f - vf) + bot * vf;
          }

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

// Scale/grid label sets (forum #88): Hz (default) or note names. The A-octaves
// are evenly spaced on the log-frequency axis, so the note grid reads as a
// regular lattice; A4 = 440 Hz is the bold reference line.
struct FreqLabel { double freq; const char* text; bool bold; };
static const FreqLabel* ActiveFreqLabels(bool notes, int* count)
{
  static const FreqLabel kHz[] = {
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
  static const FreqLabel kNotes[] = {
    {    27.5, "A0", false },
    {    55.0, "A1", false },
    {   110.0, "A2", false },
    {   220.0, "A3", false },
    {   440.0, "A4", true  },
    {   880.0, "A5", false },
    {  1760.0, "A6", false },
    {  3520.0, "A7", false },
    {  7040.0, "A8", false },
    { 14080.0, "A9", false },
  };
  if (notes) { *count = (int)(sizeof(kNotes) / sizeof(kNotes[0])); return kNotes; }
  *count = (int)(sizeof(kHz) / sizeof(kHz[0]));
  return kHz;
}

void SpectralView::DrawFreqScale(HDC hdc, int yTop, int height, int sampleRate)
{
  int scaleLeft = m_rect.right - SP(DB_SCALE_WIDTH);
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

  int labelCount = 0;
  const FreqLabel* labels = ActiveFreqLabels(m_noteScale, &labelCount);

  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
  int lastDrawnY = yTop + height + SP(20);

  for (int li = 0; li < labelCount; li++) {
    const FreqLabel& lab = labels[li];
    if (lab.freq < fMin || lab.freq > fMax) continue;

    int y = FreqToY(lab.freq, yTop, height);
    if (y < yTop + 1 || y > yTop + height - SP(2)) continue;
    if (lastDrawnY - y < SP(11)) continue;

    // Tick
    oldPen = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, scaleLeft + 1, y, nullptr);
    LineTo(hdc, scaleLeft + SP(5), y);
    SelectObject(hdc, oldPen);

    // Label
    if (lab.bold) {
      SelectObject(hdc, g_fonts.bold10);
      SetTextColor(hdc, RGB(190, 190, 190));
    } else {
      SelectObject(hdc, g_fonts.normal10);
      SetTextColor(hdc, RGB(150, 150, 150));
    }

    int labelTop = y - SP(5), labelBot = y + SP(5);
    if (y - yTop < SP(5)) { labelTop = yTop; labelBot = yTop + SP(11); }
    else if (yTop + height - y < SP(5)) { labelTop = yTop + height - SP(11); labelBot = yTop + height; }

    RECT tr = { scaleLeft + SP(6), labelTop, scaleRight - SP(2), labelBot };
    DrawTextUTF8(hdc, lab.text, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    lastDrawnY = y;
  }

  DeleteObject(tickPen);
  SelectObject(hdc, oldFont);
}

// --- Playhead & cursor drawing ---

void SpectralView::DrawPlayhead(HDC hdc, const WaveformView& waveform)
{
  int contentRight = m_rect.right - SP(DB_SCALE_WIDTH);

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
  if (m_freqSelActive) return; // marquee: DrawFreqSelectionOverlay owns the rectangle

  double s1 = std::min(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
  double s2 = std::max(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
  int x1 = waveform.TimeToX(s1);
  int x2 = waveform.TimeToX(s2);
  int lim = m_rect.right - SP(DB_SCALE_WIDTH);

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

// Marquee interior haze: brighten the framebuffer pixels inside the
// time x frequency rectangle toward white (~14%), Audition-class "frosted"
// selection. Runs on the raw 32-bit framebuffer right after the LUT colorize
// pass - portable (no AlphaBlend) and channel-order agnostic (whitening
// treats all three color bytes the same; the high byte is left alone).
static inline unsigned int WhitenPx(unsigned int c, unsigned int a)  // a/256 toward white
{
  unsigned int r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
  r += ((255u - r) * a) >> 8;
  g += ((255u - g) * a) >> 8;
  b += ((255u - b) * a) >> 8;
  return (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
}

void SpectralView::ApplySelectionHaze(unsigned int* fbuf, int allocW, int contentW,
                                      int height, const WaveformView& waveform) const
{
  if (!m_freqSelActive || !waveform.HasSelection()) return;

  double s1 = std::min(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
  double s2 = std::max(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
  int x1 = std::max(0, std::min(contentW, waveform.TimeToX(s1) - (int)m_rect.left));
  int x2 = std::max(0, std::min(contentW, waveform.TimeToX(s2) - (int)m_rect.left));
  if (x2 <= x1) return;

  int nch = waveform.GetNumChannels();
  int chSep = (nch > 1) ? SP(CHANNEL_SEPARATOR_HEIGHT) : 0;
  int chH = (nch > 1) ? (height - chSep) / 2 : height;
  double fLow = GetFreqSelLow();
  double fHigh = GetFreqSelHigh();

  for (int ch = 0; ch < nch; ch++) {
    int chTop = m_rect.top + ch * (chH + chSep);
    int yLow = FreqToY(fLow, chTop, chH) - (int)m_rect.top;
    int yHigh = FreqToY(fHigh, chTop, chH) - (int)m_rect.top;
    if (yLow < yHigh) std::swap(yLow, yHigh); // yHigh = higher freq = lower y
    yHigh = std::max(0, std::min(height, yHigh));
    yLow = std::max(0, std::min(height, yLow));
    for (int y = yHigh; y < yLow; y++) {
      unsigned int* row = fbuf + (size_t)y * (size_t)allocW;
      for (int x = x1; x < x2; x++) row[x] = WhitenPx(row[x], 36);  // ~14% frost
    }
  }
}

// Horizontal frequency grid (forum #88): a faint 1px line at every label
// frequency of the active scale (Hz or note A-octaves), blended into the
// framebuffer like the marquee haze - subtle enough to read under content.
void SpectralView::ApplyFreqGrid(unsigned int* fbuf, int allocW, int contentW,
                                 int height, const WaveformView& waveform) const
{
  int labelCount = 0;
  const FreqLabel* labels = ActiveFreqLabels(m_noteScale, &labelCount);
  double nyquist = m_specSr > 0 ? (double)m_specSr / 2.0 : 22050.0;
  double fMin = std::min(FREQ_MIN, nyquist * 0.5);
  double fMax = std::min(FREQ_MAX, nyquist);

  int nch = waveform.GetNumChannels();
  int chSep = (nch > 1) ? SP(CHANNEL_SEPARATOR_HEIGHT) : 0;
  int chH = (nch > 1) ? (height - chSep) / 2 : height;

  for (int ch = 0; ch < nch; ch++) {
    int chTop = m_rect.top + ch * (chH + chSep);
    int bandTop = chTop - (int)m_rect.top;
    for (int li = 0; li < labelCount; li++) {
      if (labels[li].freq < fMin || labels[li].freq > fMax) continue;
      int y = FreqToY(labels[li].freq, chTop, chH) - (int)m_rect.top;
      if (y <= bandTop || y >= bandTop + chH - 1 || y < 0 || y >= height) continue;
      unsigned int* row = fbuf + (size_t)y * (size_t)allocW;
      const unsigned int a = labels[li].bold ? 30u : 18u;  // ~12% / ~7%
      for (int x = 0; x < contentW; x++) row[x] = WhitenPx(row[x], a);
    }
  }
}

// Manual dashes (4 on / 4 off): SWELL only guarantees solid pens, so PS_DOT is
// not portable. Draws horizontal when y1 == y2, vertical when x1 == x2.
static void DashedLine(HDC hdc, int x1, int y1, int x2, int y2)
{
  if (y1 == y2) {
    if (x2 < x1) std::swap(x1, x2);
    for (int x = x1; x < x2; x += 8) {
      MoveToEx(hdc, x, y1, nullptr);
      LineTo(hdc, std::min(x + 4, x2), y1);
    }
  } else {
    if (y2 < y1) std::swap(y1, y2);
    for (int y = y1; y < y2; y += 8) {
      MoveToEx(hdc, x1, y, nullptr);
      LineTo(hdc, x1, std::min(y + 4, y2));
    }
  }
}

void SpectralView::DrawFreqSelectionOverlay(HDC hdc, const WaveformView& waveform)
{
  if (!m_freqSelActive) return;

  int nch = waveform.GetNumChannels();
  int height = m_rect.bottom - m_rect.top;
  int chSep = (nch > 1) ? SP(CHANNEL_SEPARATOR_HEIGHT) : 0;
  int chH = (nch > 1) ? (height - chSep) / 2 : height;
  int contentRight = m_rect.right - SP(DB_SCALE_WIDTH);

  double fLow = GetFreqSelLow();
  double fHigh = GetFreqSelHigh();

  // Marquee: with a time selection active the band renders as a dashed
  // time x frequency rectangle (the repair selection); a band-only selection
  // (Alt+drag) keeps the full-width horizontal boundary lines.
  bool marquee = waveform.HasSelection();
  // Fresh marquee click: the band exists but the rectangle does not yet -
  // drawing the full-width band lines here flashes a cyan line on every click.
  if (m_marqueeGesture && !marquee) return;
  int x1 = m_rect.left, x2 = contentRight;
  if (marquee) {
    double s1 = std::min(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
    double s2 = std::max(waveform.GetSelection().startTime, waveform.GetSelection().endTime);
    x1 = std::max((int)m_rect.left, std::min(contentRight, waveform.TimeToX(s1)));
    x2 = std::max((int)m_rect.left, std::min(contentRight, waveform.TimeToX(s2)));
    if (x2 <= x1) return; // rectangle fully off-view
  }

  HPEN pen = CreatePen(PS_SOLID, 1, marquee ? RGB(235, 235, 235) : RGB(0, 200, 255));
  HPEN old = (HPEN)SelectObject(hdc, pen);

  for (int ch = 0; ch < nch; ch++) {
    int chTop = m_rect.top + ch * (chH + chSep);
    int yLow = FreqToY(fLow, chTop, chH);
    int yHigh = FreqToY(fHigh, chTop, chH);
    if (yLow < yHigh) std::swap(yLow, yHigh); // yHigh is higher freq = lower y

    if (marquee) {
      DashedLine(hdc, x1, yHigh, x2, yHigh);
      DashedLine(hdc, x1, yLow, x2, yLow);
      DashedLine(hdc, x1, yHigh, x1, yLow);
      DashedLine(hdc, x2, yHigh, x2, yLow);
    } else {
      MoveToEx(hdc, x1, yHigh, nullptr);
      LineTo(hdc, x2, yHigh);
      MoveToEx(hdc, x1, yLow, nullptr);
      LineTo(hdc, x2, yLow);
    }
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
  int barW = std::min(SP(200), (int)(m_rect.right - m_rect.left) - SP(40));
  int barH = SPmin(4);
  int barL = cx - barW / 2;
  int barT = cy + SP(10);

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
  DrawTextUTF8(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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

  int contentW = width - SP(DB_SCALE_WIDTH);
  if (m_pixW < 1 || m_pixH < 1) return;

  // [BUG forum #65] The mem-context framebuffer stride is NOT always the width:
  // SWELL's Linux (LICE) backend pads the row span to (w+4)&~4, so writing rows
  // at contentW shears the image diagonally for widths with w%8 in {4..7}
  // (macOS CGBitmapContext uses exactly w*4 and our Win32 32bpp DIB has no row
  // padding - both immune, which is why this only bit Linux). Allocating the
  // context at a multiple-of-8 width makes stride == alloc width on EVERY
  // backend (LICE: (w+4)&~4 == w when w%8 == 0); the BitBlt then copies only
  // the visible contentW. Same pattern as ui_render.cpp presentSurface.
  const int allocW = (contentW + 7) & ~7;

#ifdef _WIN32
  // Win32: CreateDIBSection for direct framebuffer access
  if (!m_memDC || m_memW != allocW || m_memH != height) {
    if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC); m_memDC = nullptr; }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = allocW;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m_memBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (m_memBmp) {
      m_memDC = CreateCompatibleDC(hdc);
      SelectObject(m_memDC, m_memBmp);
    }
    m_memW = allocW;
    m_memH = height;
  }
  if (m_memDC && m_memBmp) {
    BITMAP bm;
    GetObject(m_memBmp, sizeof(bm), &bm);
    unsigned int* fbuf = (unsigned int*)bm.bmBits;
    if (fbuf) {
      int cols = std::min(contentW, m_pixW);
      for (int col = 0; col < cols; col++) {
        const unsigned char* colData = &m_pixels[col * m_pixH];
        for (int row = 0; row < m_pixH; row++)
          fbuf[row * allocW + col] = m_colorLUT_Native[colData[row]];
      }
      ApplyFreqGrid(fbuf, allocW, contentW, height, waveform);
      ApplySelectionHaze(fbuf, allocW, contentW, height, waveform);
      BitBlt(hdc, m_rect.left, m_rect.top, contentW, height, m_memDC, 0, 0, SRCCOPY);
    }
  }
#else
  // SWELL: direct framebuffer access via SWELL_CreateMemContext
  if (!m_memDC || m_memW != allocW || m_memH != height) {
    if (m_memDC) SWELL_DeleteGfxContext(m_memDC);
    m_memDC = SWELL_CreateMemContext(hdc, allocW, height);
    m_memW = allocW;
    m_memH = height;
  }
  if (m_memDC) {
    unsigned int* fbuf = (unsigned int*)SWELL_GetCtxFrameBuffer(m_memDC);
    if (fbuf) {
      int cols = std::min(contentW, m_pixW);
      for (int col = 0; col < cols; col++) {
        const unsigned char* colData = &m_pixels[col * m_pixH];
        for (int row = 0; row < m_pixH; row++)
          fbuf[row * allocW + col] = m_colorLUT_Native[colData[row]];
      }
      ApplyFreqGrid(fbuf, allocW, contentW, height, waveform);
      ApplySelectionHaze(fbuf, allocW, contentW, height, waveform);
      BitBlt(hdc, m_rect.left, m_rect.top, contentW, height, m_memDC, 0, 0, SRCCOPY);
    }
  }
#endif

  // Freq scale (per channel)
  int nch = waveform.GetNumChannels();
  int sr = waveform.GetSampleRate();
  if (nch > 1) {
    int chH = (height - SP(CHANNEL_SEPARATOR_HEIGHT)) / 2;
    DrawFreqScale(hdc, m_rect.top, chH, sr);
    DrawFreqScale(hdc, m_rect.top + chH + SP(CHANNEL_SEPARATOR_HEIGHT), chH, sr);
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
