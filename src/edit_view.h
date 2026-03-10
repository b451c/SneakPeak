// edit_view.h — Main EditView window class
#pragma once

#include "platform.h"
#include "config.h"
#include "globals.h"
#include "waveform_view.h"
#include "toolbar.h"
#include "gain_panel.h"
#include "marker_manager.h"
#include "levels_panel.h"
#include "spectral_view.h"
#include "minimap_view.h"
#include <vector>
#include <string>

// Audio clipboard for cut/copy/paste
struct AudioClipboard {
  std::vector<double> samples;
  int numChannels = 0;
  int sampleRate = 0;
  int numFrames = 0;
};

// Context menu IDs
enum ContextMenuID {
  CM_UNDO = 2000,
  CM_CUT,
  CM_COPY,
  CM_PASTE,
  CM_DELETE,
  CM_SILENCE,
  CM_SELECT_ALL,
  CM_SEPARATOR_EDIT,
  CM_NORMALIZE,
  CM_FADE_IN,
  CM_FADE_OUT,
  CM_REVERSE,
  CM_GAIN_UP,
  CM_GAIN_DOWN,
  CM_DC_REMOVE,
  CM_SEPARATOR_VIEW,
  CM_ZOOM_IN,
  CM_ZOOM_OUT,
  CM_ZOOM_FIT,
  CM_ZOOM_SEL,
  CM_SHOW_MARKERS,
  CM_ADD_MARKER,
  CM_ADD_REGION,
  CM_DELETE_MARKER,
  CM_EDIT_MARKER,
  CM_GAIN_PANEL,
  CM_MONO_DOWNMIX,
  CM_TOGGLE_SPECTRAL,
  CM_SNAP_ZERO,
  CM_MINIMAP,
  CM_SUPPORT_KOFI,
  CM_SUPPORT_BMAC,
  CM_SUPPORT_PAYPAL,
};

class EditView {
public:
  EditView();
  ~EditView();

  void Create();
  void Destroy();
  void Toggle();
  bool IsVisible() const;
  HWND GetHwnd() const { return m_hwnd; }

  void LoadSelectedItem();
  void OnTimer();

  static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
  INT_PTR HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
  void OnSize(int w, int h);
  void OnPaint(HDC hdc);
  void OnMouseDown(int x, int y, WPARAM wParam);
  void OnMouseUp(int x, int y);
  void OnMouseMove(int x, int y, WPARAM wParam);
  void OnMouseWheel(int x, int y, int delta, WPARAM wParam);
  void OnDoubleClick(int x, int y);
  void OnKeyDown(WPARAM key);
  void OnToolbarClick(int button);
  void OnRightClick(int x, int y);
  void OnContextMenuCommand(int id);

  void DrawRuler(HDC hdc);
  void DrawBottomPanel(HDC hdc);
  void DrawScrollbar(HDC hdc);
  void DrawSplitter(HDC hdc);
  void GetItemTitle(char* buf, int bufSize);
  void RecalcLayout(int w, int h);

  // Clipboard operations
  void DoCopy();
  void DoCut();
  void DoPaste();
  void DoDelete();
  void DoSilence();

  // Destructive processing
  void DoNormalize();
  void DoFadeIn();
  void DoFadeOut();
  void DoReverse();
  void DoGain(double factor);
  void DoDCRemove();

  // Navigation
  void NavigateToMarker(bool forward);
  void DoLoopSelection();

  // Helpers for destructive ops
  void GetSelectionSampleRange(int& startFrame, int& endFrame) const;
  void WriteAndRefresh();
  void SyncSelectionToReaper();
  void UndoSave();
  void UndoRestore();

  RECT m_toolbarRect = {};
  RECT m_rulerRect = {};
  RECT m_waveformRect = {};
  RECT m_splitterRect = {};
  RECT m_spectralRect = {};
  RECT m_minimapRect = {};
  RECT m_scrollbarRect = {};
  RECT m_bottomPanelRect = {};

  HWND m_hwnd = nullptr;
  WaveformView m_waveform;
  Toolbar m_toolbar;

  bool m_dragging = false;
  bool m_scrollbarDragging = false;
  int m_lastMouseX = 0;
  int m_lastMouseY = 0;

  // Fade handle dragging
  enum FadeDragType { FADE_NONE, FADE_IN, FADE_OUT };
  FadeDragType m_fadeDragging = FADE_NONE;
  int m_fadeDragStartY = 0;
  int m_fadeDragStartShape = 0;

  MarkerManager m_markers;

  GainPanel m_gainPanel;
  LevelsPanel m_levels;
  SpectralView m_spectral;
  MinimapView m_minimap;
  bool m_spectralVisible = false;
  bool m_minimapVisible = false;
  int m_minimapHeight = MINIMAP_HEIGHT;
  bool m_minimapDragging = false;       // resize drag (top edge)
  bool m_minimapScrollDragging = false; // click-drag to scroll view
  float m_splitterRatio = 0.55f; // waveform gets 55% of content area
  bool m_splitterDragging = false;
  bool m_spectralFreqDragging = false;
  int m_spectralFreqDragChTop = 0;
  int m_spectralFreqDragChH = 0;

  // Playback tracking
  bool m_startedPlayback = false;  // true when we initiated playback
  bool m_wasPlaying = false;       // previous play state for edge detection
  bool m_autoStopped = false;      // true after auto-stop, prevents re-trigger loop
  int m_playGraceTicks = 0;        // skip auto-stop for N ticks after play start

  // Drag & drop export
  bool m_dragExportPending = false;
  int m_dragStartX = 0;
  int m_dragStartY = 0;
  std::string m_dragTempPath;
  void InitiateDragExport();
  void CleanupDragTemp();

  // Solo button
  bool m_trackSoloed = false;
  RECT m_soloBtnRect = {};
  void DrawSoloButton(HDC hdc);
  bool ClickSoloButton(int x, int y);
  void ToggleTrackSolo();
  void UpdateSoloState();

  // Undo state
  bool m_hasUndo = false;

  // Dirty indicator (destructive edit pending)
  bool m_dirty = false;

  // Cached file size (avoid stat() every paint)
  double m_cachedFileSizeMB = 0.0;

  // WAV format info (preserved for writing back)
  int m_wavBitsPerSample = 16;
  int m_wavAudioFormat = 1; // 1=PCM, 3=float
  int m_lastChanMode = -1;  // tracks I_CHANMODE for change detection

  static AudioClipboard s_clipboard;
  static const int TIMER_REFRESH = 100;
  static const int TIMER_INTERVAL_MS = 33;
};
