// spectral_view.h — Spectrogram: async pre-computed at load, instant paint
#pragma once

#include "platform.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

class WaveformView;

class SpectralView {
public:
  SpectralView();
  ~SpectralView();

  void SetRect(int x, int y, int w, int h);
  RECT GetRect() const { return m_rect; }

  void Paint(HDC hdc, const WaveformView& waveform);
  void Invalidate() { m_renderValid = false; }

  // Pre-compute spectrogram for entire item (async — returns immediately)
  void PrecomputeSpectrum(const WaveformView& waveform);
  void ClearSpectrum();

  bool IsLoading() const { return m_computing.load(); }
  bool IsReady() const { return m_specValid.load(); }
  float GetProgress() const { return m_progress.load(); }


  // FFT parameters
  static const int FFT_SIZE = 2048;
  static const int FFT_HALF = FFT_SIZE / 2;
  static const int HOP_SIZE = 512;

  // Display frequency range (Hz)
  static constexpr double FREQ_MIN = 20.0;
  static constexpr double FREQ_MAX = 22050.0;

  // Frequency selection (Y-axis band selection)
  bool HasFreqSelection() const { return m_freqSelActive; }
  double GetFreqSelLow() const;
  double GetFreqSelHigh() const;
  void StartFreqSelection(double freqHz);
  void UpdateFreqSelection(double freqHz);
  void ClearFreqSelection();

  // Convert Y pixel coordinate to frequency (Hz)
  double YToFreq(int y, int channelTop, int channelHeight) const;
  int FreqToY(double freqHz, int channelTop, int channelHeight) const;

private:

  void ComputeThreadFunc(std::vector<double> audio, int nch, int sr, int totalFrames);
  void RenderView(const WaveformView& waveform);
  void DrawFreqScale(HDC hdc, int yTop, int height, int sampleRate);
  void DrawPlayhead(HDC hdc, const WaveformView& waveform);
  void DrawSelection(HDC hdc, const WaveformView& waveform);
  void DrawFreqSelectionOverlay(HDC hdc, const WaveformView& waveform);
  void DrawLoadingOverlay(HDC hdc);
  static COLORREF MagmaColor(float t);

  // Color LUTs
  COLORREF m_colorLUT[256];
  unsigned int m_colorLUT_Native[256];

  // Pre-computed full spectrogram
  std::vector<unsigned char> m_specData;
  int m_specCols = 0;
  int m_specNch = 0;
  int m_specSr = 0;
  double m_specDuration = 0.0;
  std::atomic<bool> m_specValid{false};

  // Async computation
  std::thread m_computeThread;
  std::atomic<bool> m_computing{false};
  std::atomic<bool> m_cancelRequested{false};
  std::atomic<float> m_progress{0.0f};
  std::mutex m_specMutex; // protects m_specData during handoff

  // Rendered pixel cache (for current view)
  std::vector<unsigned char> m_pixels;
  int m_pixW = 0;
  int m_pixH = 0;
  std::atomic<bool> m_renderValid{false};
  double m_cachedViewStart = 0.0;
  double m_cachedViewDuration = 0.0;
  int m_cachedWidth = 0;
  int m_cachedHeight = 0;

  // Offscreen bitmap DC
  HDC m_memDC = nullptr;
  int m_memW = 0;
  int m_memH = 0;

  // Frequency selection state
  bool m_freqSelActive = false;
  double m_freqSelStart = 0.0;
  double m_freqSelEnd = 0.0;

  RECT m_rect = {};
};
