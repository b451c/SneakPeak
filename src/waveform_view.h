// waveform_view.h — Waveform display and interaction for EditView
#pragma once

#include "platform.h"
#include "config.h"
#include "globals.h"
#include <vector>
#include <cmath>

struct WaveformSelection {
  double startTime = 0.0;
  double endTime = 0.0;
  bool active = false;
};

class WaveformView {
public:
  WaveformView();
  ~WaveformView();

  // Item binding
  void SetItem(MediaItem* item);
  void ClearItem();
  bool HasItem() const { return m_item != nullptr; }
  MediaItem* GetItem() const { return m_item; }

  // Geometry
  void SetRect(int x, int y, int w, int h);
  RECT GetRect() const { return m_rect; }

  // View state
  double GetViewStart() const { return m_viewStartTime; }
  double GetViewEnd() const { return m_viewStartTime + m_viewDuration; }
  double GetViewDuration() const { return m_viewDuration; }
  double GetItemDuration() const { return m_itemDuration; }
  double GetItemPosition() const { return m_itemPosition; }
  int GetNumChannels() const { return m_numChannels; }
  float GetVerticalZoom() const { return m_verticalZoom; }

  // Navigation
  void ZoomHorizontal(double factor, double centerTime);
  void ZoomVertical(float factor);
  void ScrollH(double deltaTime);
  void ZoomToFit();
  void ZoomToSelection();

  // Selection
  void StartSelection(double time);
  void UpdateSelection(double time);
  void EndSelection();
  void ClearSelection();
  WaveformSelection GetSelection() const { return m_selection; }
  bool HasSelection() const { return m_selection.active && m_selection.startTime != m_selection.endTime; }

  // Coordinate conversion
  double XToTime(int x) const;
  int TimeToX(double time) const;

  // Rendering (GDI)
  void Paint(HDC hdc);
  void Invalidate() { m_peaksValid = false; }
  void ReloadAudio();  // re-read samples from source (after normalize etc.)

  // Cursor
  void SetCursorTime(double time) { m_cursorTime = time; }
  double GetCursorTime() const { return m_cursorTime; }

  // Fade drag feedback
  void SetFadeDragInfo(int dragType, int shape);

  // Channel active state (mute buttons: both on by default)
  bool IsChannelActive(int ch) const { return m_channelActive[ch]; } // ch: 0=L, 1=R
  int GetChanMode() const; // returns I_CHANMODE value based on active state
  bool ClickChannelButton(int x, int y); // returns true if hit

  // Audio data access (for destructive editing)
  std::vector<double>& GetAudioData() { return m_audioData; }
  const std::vector<double>& GetAudioData() const { return m_audioData; }
  int GetAudioSampleCount() const { return m_audioSampleCount; }
  int GetSampleRate() const { return m_sampleRate; }
  MediaItem_Take* GetTake() const { return m_take; }
  double GetTakeOffset() const { return m_takeOffset; }

  // Update after destructive edit
  void SetAudioSampleCount(int count) { m_audioSampleCount = count; }
  void SetItemDuration(double dur) { m_itemDuration = dur; }
  void SetItemPosition(double pos) { m_itemPosition = pos; }

private:
  void LoadAudioData();
  void UpdatePeaks();
  void DrawWaveformChannel(HDC hdc, int channel, int yTop, int height);
  void DrawSelection(HDC hdc);
  void DrawCursor(HDC hdc);
  void DrawCenterLine(HDC hdc, int yCenter);
  void DrawDbGridLines(HDC hdc, int channel, int yTop, int height);
  void DrawTimeGrid(HDC hdc);
  void DrawDbScale(HDC hdc, int channel, int yTop, int height);
  void DrawFadeBackground(HDC hdc);
  void DrawFadeEnvelope(HDC hdc);
  int GetChannelTop(int channel) const;
  int GetChannelHeight() const;

  // Item data
  MediaItem* m_item = nullptr;
  MediaItem_Take* m_take = nullptr;
  double m_itemPosition = 0.0;
  double m_itemDuration = 0.0;
  double m_takeOffset = 0.0;
  int m_numChannels = 0;
  int m_sampleRate = 44100;

  // Cached audio samples (loaded once per item)
  std::vector<double> m_audioData;  // interleaved [sample * nch + ch]
  int m_audioSampleCount = 0;       // total frames loaded

  // View state
  double m_viewStartTime = 0.0;
  double m_viewDuration = 1.0;
  float m_verticalZoom = 1.0f;
  double m_cursorTime = 0.0;

  // Selection
  WaveformSelection m_selection;
  bool m_selecting = false;

  // Peaks cache (computed from m_audioData, no API calls)
  std::vector<double> m_peakMax;
  std::vector<double> m_peakMin;
  bool m_peaksValid = false;
  double m_peaksCachedStart = 0.0;
  double m_peaksCachedDuration = 0.0;
  int m_peaksCachedWidth = 0;

  // Fade drag feedback
  int m_fadeDragType = 0;  // 0=none, 1=fadeIn, 2=fadeOut
  int m_fadeDragShape = 0;

  // Channel active (mute buttons)
  bool m_channelActive[2] = { true, true };

  // Geometry
  RECT m_rect = {0, 0, 0, 0};
};
