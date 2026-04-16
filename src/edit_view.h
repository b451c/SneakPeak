// edit_view.h — Main SneakPeak window class
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
#include "dynamics_engine.h"
#include "dynamics_panel.h"
#include <vector>
#include <string>

// Standalone file state — preserved when switching tabs
struct StandaloneFileState {
  std::string filePath;
  std::vector<double> audioData;
  std::vector<std::vector<double>> undoStack;
  int numChannels = 0;
  int sampleRate = 44100;
  int audioSampleCount = 0;
  int bitsPerSample = 16;
  int audioFormat = 1;
  double itemDuration = 0.0;
  double cursorTime = 0.0;
  double viewStartTime = 0.0;
  double viewDuration = 1.0;
  WaveformSelection selection;
  bool dirty = false;
  WaveformView::StandaloneFade fade;
  std::string savedPath;            // where file was last saved (empty = never saved)
  bool overwriteConfirmed = false;  // user confirmed overwrite of original WAV
};

// Tab hit-test cache for mode bar
struct ModeBarTab {
  RECT rect;
  RECT closeRect;
  int fileIdx;
  bool isReaper;
};

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
  CM_RIPPLE_DELETE,
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
  CM_SUPPORT_GITHUB,
  CM_NORMALIZE_LUFS,
  CM_NORMALIZE_LUFS_16,
  CM_MULTI_MODE_MIX,
  CM_MULTI_MODE_LAYERED,
  CM_MULTI_MODE_LAYERED_TRACKS,
  CM_SHOW_JOIN_LINES,
  CM_TRACK_VIEW,
  CM_GROUP_SET,
  CM_SPLIT,
  CM_DOCK_WINDOW,
  CM_RULER_RELATIVE,
  CM_RULER_ABSOLUTE,
  CM_RULER_BARS_BEATS,
  CM_METER_PEAK,
  CM_METER_RMS,
  CM_METER_VU,
  CM_METER_SOURCE_MASTER,
  CM_SHOW_VOLUME_ENVELOPE,
  CM_SHOW_DYNAMICS,
  CM_APPLY_DYNAMICS,
  CM_ENV_SHAPE_LINEAR,
  CM_ENV_SHAPE_SQUARE,
  CM_ENV_SHAPE_SLOW,
  CM_ENV_SHAPE_FAST,
  CM_ENV_SHAPE_FAST_END,
  CM_ENV_SHAPE_BEZIER,
  CM_ENV_DELETE_POINT,
  CM_SWITCH_TIMELINE,
  CM_PRESET_BASE,  // + PRESET_COUNT entries
  CM_PRESET_LAST = CM_PRESET_BASE + 10,
  CM_LAST // sentinel -- keep last
};

class SneakPeak {
public:
  SneakPeak();
  ~SneakPeak();

  void Create();
  void Destroy();
  void Toggle();
  bool IsVisible() const;
  bool IsPendingClose() const { return m_pendingClose; }
  bool IsStandaloneMode() const { return m_waveform.IsStandaloneMode(); }
  HWND GetHwnd() const { return m_hwnd; }

  void LoadSelectedItem();
  void ToggleTrackView();
  void OnTimer();

  // Mode bar / standalone tab management
  void SaveCurrentStandaloneState();
  void AddStandaloneFile(const char* path);

  static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  // Called directly from accelerator callback (SWS pattern - no SendMessage bounce)
  void OnKeyDown(WPARAM key);

private:
  INT_PTR HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
  void OnSize(int w, int h);
  void OnPaint(HDC hdc);
  void OnMouseDown(int x, int y, WPARAM wParam);
  void OnMouseDownWaveform(int x, int y, WPARAM wParam);
  void OnMouseUp(int x, int y);
  void OnMouseMove(int x, int y, WPARAM wParam);
  void OnMouseWheel(int x, int y, int delta, WPARAM wParam);
  void OnDoubleClick(int x, int y);
  bool HandlePendingClose();
  void ValidateItemPointers();
  void UpdateAutoScroll();
  void UpdatePlaybackFollow();
  void UpdateGainPreview();
  void UpdateItemState();
  void OnToolbarClick(int button);
  void OnRightClick(int x, int y);
  void OnContextMenuCommand(int id);

  void DrawRuler(HDC hdc);
  void DrawRulerBarsBeats(HDC hdc);
  void DrawBottomPanel(HDC hdc);
  void DrawScrollbar(HDC hdc);
  void DrawSplitter(HDC hdc);
  void GetItemTitle(char* buf, int bufSize);
  void RecalcLayout(int w, int h);

  // LoadSelectedItem sub-methods
  bool LoadSelectedItemMulti(int count); // returns true if handled

  // Post-gain reload dispatcher
  void ReloadAfterGainChange(double savedViewStart, double savedViewDur,
                             const WaveformSelection& savedSel, double savedCursor, double db);

  // Clipboard operations
  void DoCopy();
  void DoCut();
  void DoPaste();
  void DoPasteDestructive();
  void DoDelete(bool ripple = false);
  void DoDeleteStandalone();
  void DoDeleteNonDestructive(bool ripple = false);
  void DoSilence();

  // Destructive processing
  void DoNormalize();
  void DoFadeIn();
  void DoFadeOut();
  void DoReverse();
  void DoGain(double factor);
  void DoDCRemove();
  void DoNormalizeLUFS(double targetLufs = -14.0);

  // Navigation
  void NavigateToMarker(bool forward);
  void DoLoopSelection();

  // Timeline view (post-cut sibling items with gaps)
  std::vector<MediaItem*> FindSiblingItems(MediaTrack* track, MediaItem* sourceItem);
  void RefreshTimelineView();

  // Helpers for destructive ops
  void GetSelectionSampleRange(int& startFrame, int& endFrame) const;
  void WriteAndRefresh();
  void SyncSelectionToReaper();
  void UpdateTitle();
  void UndoSave();
  void UndoRestore();

  RECT m_modeBarRect = {};
  RECT m_modeLabelRect = {};  // clickable area of the mode label (MULTI/TIMELINE/ITEM/SET)
  RECT m_supportRect = {};    // clickable support link in mode bar
  RECT m_toolbarRect = {};
  RECT m_rulerRect = {};
  RECT m_waveformRect = {};
  RECT m_splitterRect = {};
  RECT m_spectralRect = {};
  RECT m_minimapRect = {};
  RECT m_scrollbarRect = {};
  RECT m_bottomPanelRect = {};
  RECT m_metersRect = {};

  HWND m_hwnd = nullptr;
  WaveformView m_waveform;
  Toolbar m_toolbar;

  bool m_pendingClose = false;
  bool m_isDocked = false;
  int m_timelineEditGuard = 0; // ticks to suppress timeline exit after edit operation
  WaveformSelection m_pendingSelRestore = {}; // selection to restore after guarded reload
  bool m_dragging = false;
  bool m_scrollbarDragging = false;
  int m_lastMouseX = 0;
  int m_lastMouseY = 0;

  // Fade handle dragging (REAPER non-destructive + standalone destructive)
  enum FadeDragType { FADE_NONE, FADE_IN, FADE_OUT };
  FadeDragType m_fadeDragging = FADE_NONE;
  int m_fadeDragStartY = 0;
  double m_fadeDragStartDir = 0.0;  // starting curvature for vertical drag
  bool m_standaloneFadeDrag = false; // true when dragging standalone fade handle

  // Envelope point dragging + freehand drawing
  bool m_envDragging = false;
  int m_envDragPointIdx = -1;  // index of point being dragged (-1 = none)
  double m_envDragMinTime = 0.0;  // left neighbor time (clamp bound, segment-relative)
  double m_envDragMaxTime = 0.0;  // right neighbor time (clamp bound, segment-relative)
  TrackEnvelope* m_envDragEnv = nullptr;     // envelope being edited (correct segment in timeline/SET)
  double m_envDragSegOffset = 0.0;           // segment's relativeOffset for viewTime<->envTime
  double m_envDragSegDuration = 0.0;         // duration of segment being edited
  bool m_envFreehand = false;  // freehand drawing mode (add points on mousemove)
  int m_envFreehandLastX = 0;  // throttle: last X where point was added

  // Envelope selection rectangle (right-click drag)
  bool m_envRectSelecting = false;
  int m_envRectStartX = 0, m_envRectStartY = 0;
  int m_envRectEndX = 0, m_envRectEndY = 0;

  // Envelope auto-refresh (detect envelope appear/change in REAPER)
  bool m_lastEnvExists = false;
  int m_lastEnvPointCount = 0;

  MarkerManager m_markers;

  GainPanel m_gainPanel;
  LevelsPanel m_levels;
  SpectralView m_spectral;
  MinimapView m_minimap;
  DynamicsEngine m_dynamics;
  DynamicsPanel m_dynamicsPanel;
  bool m_dynamicsVisible = false;
  bool m_spectralVisible = false;
  bool m_spectralPainted = false;  // triggers one repaint after FFT completes
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

  // Mode bar
  void DrawModeBar(HDC hdc);
  void RestoreStandaloneState(int idx);
  void OnModeBarCloseTab(int idx);
  std::vector<StandaloneFileState> m_standaloneFiles;
  int m_activeFileIdx = -1;
  std::vector<ModeBarTab> m_modeBarTabs;

  // Standalone file mode (drag & drop from disk)
  void LoadStandaloneFile(const char* path);
  void SaveStandaloneFile();
  void SaveStandaloneFileAs();
  void BakePendingFades();
  void StandalonePlayStop();
  void StandaloneCleanupPreview();
  std::string m_savedPath;           // last saved path (empty = never saved)
  bool m_overwriteConfirmed = false; // confirmed overwrite of original WAV

  // Drag & drop export
  bool m_dragExportPending = false;
  bool m_dragExportImmediate = false; // Alt+drag = immediate, no-Alt = on window exit
  int m_dragStartX = 0;
  int m_dragStartY = 0;
  std::string m_dragTempPath;
  bool m_dragIsOriginal = false;  // true when dragging original file (don't delete)
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
  // Standalone undo stack (snapshots of audio data)
  std::vector<std::vector<double>> m_standaloneUndoStack;
  static const int MAX_STANDALONE_UNDO = 20;
  void StandaloneUndoSave();
  void StandaloneUndoRestore();

  // Dirty indicator (destructive edit pending)
  bool m_dirty = false;

  // Toast overlay (e.g. "Saved!")
  DWORD m_toastStartTick = 0;
  char m_toastText[64] = {};
  void ShowToast(const char* text);
  void DrawToast(HDC hdc);

  // Cached file size (avoid stat() every paint)
  double m_cachedFileSizeMB = 0.0;

  // WAV format info (preserved for writing back)
  int m_wavBitsPerSample = 16;
  int m_wavAudioFormat = 1; // 1=PCM, 3=float
  enum class RulerMode { Relative = 0, Absolute = 1, BarsBeats = 2 };
  RulerMode m_rulerMode = RulerMode::Relative;
  int m_lastChanMode = -1;  // tracks I_CHANMODE for change detection
  int m_audioChangeCheckCounter = 0;  // poll counter for external audio changes

  // Standalone preview playback
  bool m_previewActive = false;
  bool m_previewCacheDirty = true; // true when temp WAV needs rewrite
  void* m_previewReg = nullptr; // preview_register_t* (opaque to avoid header dep)
  PCM_source* m_previewSrc = nullptr;
  std::string m_previewTempPath;

  // Working set (locked multi-item edit range)
  struct WorkingSet {
    MediaTrack* track = nullptr;
    std::vector<MediaItem*> items; // explicit item list (only user-selected items)
    double startPos = 0.0;         // timeline start (for ripple edit bounds)
    double endPos = 0.0;           // timeline end (for ripple edit bounds)
    bool active = false;           // currently displayed
    bool dormant = false;          // user clicked away, set preserved for restore
  };
  WorkingSet m_workingSet;
  void LoadWorkingSet();
  void RefreshWorkingSet();
  void ExitWorkingSet();
  bool IsWorkingSetItem(MediaItem* item) const;
  void GroupSetItems();
  void UngroupSetItems();
  int GetSetGroupId(double rangeStart, double rangeEnd) const; // 0 = not grouped

  // Master meter mode (when no item selected)
  bool m_masterMode = false;
  bool m_meterFromMaster = false; // meter reads master track instead of item
  static const int MASTER_ROLLING_SIZE = 4096;
  float m_masterPeakBufL[MASTER_ROLLING_SIZE] = {};
  float m_masterPeakBufR[MASTER_ROLLING_SIZE] = {};
  int m_masterPeakHead = 0;
  int m_masterPeakCount = 0;
  void DrawMasterWaveform(HDC hdc);
  void DrawDynamicsCurve(HDC hdc);
  void ApplyDynamicsToEnvelope();
  void SaveDynamicsToItem();
  bool LoadDynamicsFromItem();

  static AudioClipboard s_clipboard;
  static const int TIMER_REFRESH = 100;
  static const int TIMER_INTERVAL_MS = 33;
};
