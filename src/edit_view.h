// edit_view.h — Main SneakPeak window class
#pragma once

#include "platform.h"
#include "config.h"
#include "globals.h"
#include "waveform_view.h"
#include "audio_engine.h"
#include "toolbar.h"
#include "gain_panel.h"
#include "marker_manager.h"
#include "levels_panel.h"
#include "spectral_view.h"
#include "minimap_view.h"
#include "dynamics_engine.h"
#include "dynamics_panel.h"
#include "settings_panel.h"
#include "limiter_panel.h"
#include "ui_render.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

// Standalone undo/redo entry (STA-2). Whole-file and length-changing edits
// snapshot the FULL buffer; bounded selection edits (heal, click repair,
// silence, selection gain) snapshot only the touched RANGE - orders of
// magnitude less memory on long files (a full 30-min stereo snapshot is
// ~1.4 GB; a 5-second heal slice is ~4 MB).
struct StandaloneUndoEntry {
  bool full = true;
  int startFrame = 0;        // range entries: first frame of the slice
  std::vector<double> data;  // full: whole buffer; range: slice (frames*nch)
};

// Standalone file state — preserved when switching tabs
struct StandaloneFileState {
  std::string filePath;
  std::vector<double> audioData;
  std::vector<StandaloneUndoEntry> undoStack;
  std::vector<StandaloneUndoEntry> redoStack;
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
  CM_SHOW_RMS,
  CM_SHOW_METERS,
  CM_SHOW_RULER,
  CM_SPECTRAL_NOTES,
  CM_APPLY_DYNAMICS,
  CM_ENV_SHAPE_LINEAR,
  CM_ENV_SHAPE_SQUARE,
  CM_ENV_SHAPE_SLOW,
  CM_ENV_SHAPE_FAST,
  CM_ENV_SHAPE_FAST_END,
  CM_ENV_SHAPE_BEZIER,
  CM_ENV_DELETE_POINT,
  CM_SWITCH_TIMELINE,
  CM_REPLACE_SOURCE,
  CM_PRESET_BASE,  // + PRESET_COUNT entries
  CM_PRESET_LAST = CM_PRESET_BASE + 10,
  CM_DYN_SAVE_PRESET,                                  // "Save preset as..."
  CM_DYN_USER_PRESET_BASE,                             // + MAX_USER_PRESETS apply entries
  CM_DYN_USER_PRESET_LAST = CM_DYN_USER_PRESET_BASE + 32,
  CM_DYN_DEL_PRESET_BASE,                              // + MAX_USER_PRESETS delete entries
  CM_DYN_DEL_PRESET_LAST = CM_DYN_DEL_PRESET_BASE + 32,
  // Global UI scale (v2.2.0 B-1). The CM_UI_SCALE_* items are the OFF-build (GDI)
  // fallback control; the premium build uses the Settings panel (CM_SETTINGS).
  CM_UI_SCALE_SMALLER,                                 // step the UI scale down
  CM_UI_SCALE_LARGER,                                  // step the UI scale up
  CM_UI_SCALE_RESET,                                   // reset the UI scale to 100%
  CM_UI_SCALE_PRESET_BASE,                             // + absolute % presets (see context_menu.cpp)
  CM_UI_SCALE_PRESET_LAST = CM_UI_SCALE_PRESET_BASE + 16,
  CM_ZOOM_CENTER,                                      // toggle wheel-zoom center: mouse <-> edit cursor (#83)
  CM_SETTINGS,                                         // open the premium Settings panel
  // Spectral Repair (v2.3.0 INC-5) - standalone destructive, spectral view only.
  CM_SPECTRAL_HEAL_BASE,                               // + strength presets (see context_menu.cpp)
  CM_SPECTRAL_HEAL_LAST = CM_SPECTRAL_HEAL_BASE + 4,
  CM_REPAIR_CLICKS,                                    // AR click repair on the time selection
  CM_REDO,                                             // Ctrl+Shift+Z / Ctrl+Y
  CM_ENV_RESET_TENSION,                                // T2-1: reset bezier curvature to 0
  // Hard Limiter (v2.4.0 INC-L1) - standalone destructive, premium panel.
  CM_APPLY_LIMITER,                                    // open the HARD LIMITER panel
  CM_LIM_PRESET_BASE,                                  // + kLimPresetCount factory presets
  CM_LIM_PRESET_LAST = CM_LIM_PRESET_BASE + 4,
  CM_LAST // sentinel -- keep last
};

// A user-saved dynamics preset (name + the serialized DynamicsParams string).
struct DynUserPreset {
  std::string name;
  std::string params;   // DynamicsParamsToString() output
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
  void ToggleMasterView();  // REAPER action (#63 X-Raym): toggle the MASTER output view (same as the mode-bar tab)
  void RunToolbarCommand(int button);  // named toolbar actions (forum #51): same path as a toolbar click
  void OnTimer();

  // Mode bar / standalone tab management
  void SaveCurrentStandaloneState();
  void AddStandaloneFile(const char* path);

  static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  // Called directly from accelerator callback (SWS pattern - no SendMessage bounce)
  void OnKeyDown(WPARAM key);
  // Inline dynamics type-value editor (Inc 8): the accelerator routes keys here while
  // an editor is open, so typed digits/Enter/ESC never trigger global shortcuts.
  bool IsDynamicsEditingValue() const { return m_dynamicsPanel.IsEditingValue(); }
  void HandleDynamicsEditKey(WPARAM key);
  bool IsLimiterEditingValue() const { return m_limiterPanel.IsEditingValue(); }
  void HandleLimiterEditKey(WPARAM key);
  bool HasFocus() const { return m_hasFocus; }
  bool IsDocked() const { return m_isDocked; }

private:
  INT_PTR HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
  void OnSize(int w, int h);
  void OnPaint(HDC hdc);
  void OnPaintOverlay(HDC hdc);   // HiDPI overlay (premium panel/spike) on the real window DC, post-composite
  double GetUiDpr() const;        // device-pixel ratio for crisp HiDPI Blend2D rendering
  void OnMouseDown(int x, int y, WPARAM wParam);
  void OnMouseDownWaveform(int x, int y, WPARAM wParam);
  void OnMiddleDown(int x, int y);   // middle-mouse pan start (#61)
  int  HitSelectionEdge(int x, int y);  // selection edge under cursor: 0 none, 1 start, 2 end (#64)
  // Spectral marquee edge grips (bitmask; corners = two bits set)
  enum { GRIP_T_START = 1, GRIP_T_END = 2, GRIP_F_LOW = 4, GRIP_F_HIGH = 8 };
  int  HitMarqueeEdge(int x, int y, int chTop, int chH);
  void SpectralChannelAt(int y, int& chTop, int& chH); // channel band under y
  void OnMouseUp(int x, int y);
  void OnMouseMove(int x, int y, WPARAM wParam);
  void OnMouseWheel(int x, int y, int delta, WPARAM wParam);
  void FlushFadeWheelUndo();   // close a pending wheel-nudge fade undo block
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

  // --- Global UI scale (v2.2.0 B-1) ---
  void   ApplyUiScale(double scale);         // the single scale-change entry point: clamp + relayout + repaint
  void   SaveUiScale();                      // persist g_uiScale to ExtState (int x1000, locale-safe)
  double QuerySystemDefaultUiScale() const;  // map the system DPI to a scale, for the first-run auto-seed
  double ComputeFitUiScale() const;          // largest scale at which the fixed chrome fits the client area
  void   MarkUiScaleUserSet();               // user chose a scale -> WM_DPICHANGED must never stomp it (durable)

  // LoadSelectedItem sub-methods
  bool LoadSelectedItemMulti(int count); // returns true if handled

  // Post-gain reload dispatcher
  void ReloadAfterGainChange(double savedViewStart, double savedViewDur,
                             const WaveformSelection& savedSel, double savedCursor, double db);

  // Auto-activate take volume envelope if missing. Uses REAPER action 40693
  // (native toggle) which targets active take of selected items. Saves/restores
  // the current item selection. Returns the envelope handle, or nullptr on
  // failure. Optional out: wasCreated = true if we activated, false if existed.
  TrackEnvelope* EnsureVolumeEnvelope(MediaItem_Take* take, MediaItem* item, bool* wasCreated = nullptr);

  // Replace every REAPER take whose source file matches oldPath with a source pointing
  // to newPath. Returns the number of takes updated. Used by "Replace Source in REAPER
  // Timeline" in standalone mode. Path comparison is case-insensitive on Windows.
  int ReplaceSourceInTimeline(const std::string& oldPath, const std::string& newPath);
  void DoReplaceSourceInTimeline();

  // Fetch latest release tag from GitHub API via curl; toast the result
  // (up-to-date / update available with version numbers). Blocking with 5s timeout.
  void DoCheckForUpdate();

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
  void DoSpectralHeal(double strength);  // v2.3.0 INC-5: STFT heal of time x freq selection
  void DoRepairClicks();                 // v2.3.0 INC-5: AR click repair on time selection
  void DoApplyLimiter();                 // v2.4.0 INC-L1: true-peak hard limiter apply

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
  void RedoRestore();

  RECT m_modeBarRect = {};
  RECT m_modeLabelRect = {};  // clickable area of the mode label (MULTI/TIMELINE/ITEM/SET)
  RECT m_supportRect = {};    // clickable support link in mode bar
  RECT m_versionRect = {};    // clickable version label in mode bar (runs update check)
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
  bool m_hasFocus = false;
  int m_timelineEditGuard = 0; // ticks to suppress timeline exit after edit operation
  WaveformSelection m_pendingSelRestore = {}; // selection to restore after guarded reload
  bool m_dragging = false;
  int m_envDragGrabDy = 0; // env point drag: point screen Y - cursor Y at grab
  // T2-1 (#51): Alt+drag on an envelope segment edits its bezier tension.
  bool m_envTensionDragging = false;
  int m_envTensionPtIdx = -1;    // owning LEFT point of the dragged segment
  double m_envTensionStart = 0.0; // tension at mouse-down
  double m_envTensionCur = 0.0;   // live value (cursor readout)
  int m_envTensionStartY = 0;
  int m_envTensionDir = 1;        // sign(v1 - v2): drag up = bulge toward v1
  bool m_scrollbarDragging = false;
  bool m_mmbPanning = false;   // middle-mouse horizontal pan (#61)
  int m_mmbLastX = 0;          // last cursor X during the MMB pan
  int m_lastMouseX = 0;
  int m_lastMouseY = 0;

  // Fade handle dragging (REAPER non-destructive + standalone destructive)
  enum FadeDragType { FADE_NONE, FADE_IN, FADE_OUT };
  FadeDragType m_fadeDragging = FADE_NONE;
  int m_fadeDragStartY = 0;
  double m_fadeDragStartDir = 0.0;  // starting curvature for vertical drag
  int m_fadeDragAnchorX = 0;        // Shift fine-drag anchor (rebased on Shift toggle mid-drag)
  double m_fadeDragAnchorLen = 0.0;
  bool m_fadeDragFine = false;
  bool m_fadeWheelUndoOpen = false; // wheel-nudge undo block; closed by OnTimer after idle
  DWORD m_fadeWheelLastTick = 0;

  // Slip content (T2-2e, forum #51): Alt+drag in plain ITEM mode slides the
  // take source under the item (D_STARTOFFS). Anchor-based like the fade drag.
  bool m_slipDragging = false;
  int m_slipStartX = 0;
  double m_slipStartOffs = 0.0;
  double m_slipPlayrate = 1.0;
  double m_slipMaxOffs = 0.0;       // srcLen - itemLen*playrate (non-looped clamp)
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
  SettingsPanel m_settingsPanel;  // premium Settings overlay (UI scale; migrated prefs next)
  LimiterPanel m_limiterPanel;    // premium HARD LIMITER overlay (v2.4.0 INC-L1)
  RECT m_gearRect = {};           // settings gear in the mode bar (premium build only)
  bool m_dynamicsVisible = false;
  bool m_spectralVisible = false;
  bool m_spectralWasLoading = false; // OnTimer: repaint pump while spectrum computes
  bool m_spectralPainted = false;  // triggers one repaint after FFT completes
  bool m_minimapVisible = false;
  bool m_showMeters = true;
  bool m_showRuler = true;        // hide-ruler layout flag (forum #51); markers fall back onto the waveform
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
  // Incremental load (STA-1): long files decode in OnTimer slices; the new
  // tab installs at completion, the current view keeps working meanwhile.
  AudioEngine::StreamLoad m_stdLoad;
  bool m_stdLoading = false;
  void StepStandaloneLoad();    // OnTimer slice + progress title
  void FinishStandaloneLoad();  // install buffer + tab bookkeeping
  void EvictStandaloneTabIfFull();
  void InstallStandaloneTab(const std::string& spath);
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
  // Standalone undo/redo stacks (full or range snapshots - StandaloneUndoEntry)
  std::vector<StandaloneUndoEntry> m_standaloneUndoStack;
  std::vector<StandaloneUndoEntry> m_standaloneRedoStack;
  static const int MAX_STANDALONE_UNDO = 20;
  void StandaloneUndoSave();                              // full-buffer snapshot
  void StandaloneUndoPushFull(std::vector<double>&& oldData); // zero-copy full slot
  void StandaloneUndoSaveRange(int startFrame, int numFrames); // bounded edits
  // Bumped on every standalone buffer mutation (edit/undo/redo/tab/load): the
  // background limiter apply swaps its result in only if this is unchanged.
  uint64_t m_standaloneBufferSerial = 0;
  void StandaloneUndoRestore();
  void StandaloneRedoRestore();
  // Swap `entry` with the live buffer, pushing the inverse onto `inverseStack`
  void StandaloneApplyUndoEntry(StandaloneUndoEntry& entry,
                                std::vector<StandaloneUndoEntry>& inverseStack);
  void StandaloneFinishRestore(const char* what); // shared undo/redo tail

  // Dirty indicator (destructive edit pending)
  bool m_dirty = false;

  // Toast overlay (e.g. "Saved!")
  DWORD m_toastStartTick = 0;
  char m_toastText[64] = {};
  UiCanvas m_toastCanvas;   // premium toast renderer (Inc F)
  void ShowToast(const char* text);
  void DrawToast(HDC hdc);
  void DrawToastPremium(HDC hdc);   // alpha-faded Blend2D toast, drawn last in the overlay

  // Cached file size (avoid stat() every paint)
  double m_cachedFileSizeMB = 0.0;

  // WAV format info (preserved for writing back)
  int m_wavBitsPerSample = 16;
  int m_wavAudioFormat = 1; // 1=PCM, 3=float
  enum class RulerMode { Relative = 0, Absolute = 1, BarsBeats = 2 };
  RulerMode m_rulerMode = RulerMode::Relative;
  int m_lastChanMode = -1;  // tracks I_CHANMODE for change detection
  int m_audioChangeCheckCounter = 0;  // poll counter for external audio changes

  // Channel solo via take pan balance (badges [1]/[2]): the user's pan is saved
  // on first solo and restored on un-solo / item switch. Take-scoped state.
  MediaItem_Take* m_chanSoloTake = nullptr;
  double m_chanSoloPrevPan = 0.0;

  // Mode bar hover target (visual feedback; rects cached by DrawModeBar).
  // >= 0 -> index into m_modeBarTabs; negatives = the fixed elements.
  enum { MB_HOVER_NONE = -1, MB_HOVER_GEAR = -2, MB_HOVER_SUPPORT = -3, MB_HOVER_VERSION = -4 };
  int m_modeBarHover = MB_HOVER_NONE;

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
  bool m_zoomOnEditCursor = false; // wheel zoom centers on the edit cursor instead of the mouse (#83)
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
  void RefreshDynamicsAvgGr();   // push real avg GR into the panel after open (no makeup leap on first drag)
  void ReanalyzeDynamicsAfterEdit(); // re-run Analyze/ComputeCompression after a type-value commit (mirrors wheel)
  void ApplyEnvelopeBypass(bool bypassed); // A/B: write envelope ACTIVE state on all segments (shared mouse + ESC paths)
  void CloseDynamicsPanel();       // close from a non-mouse path (ESC / D hotkey, #77): un-bypass A/B + end Live undo
  void RestoreDynamicsViewPrefs(); // apply persisted Dyn/Env/GR overlay prefs (+ panel size/pos) after the panel opens
  void SaveDynamicsViewPrefs();    // persist Dyn/Env/GR overlay toggles as global user prefs (ExtState)
  void SaveDynamicsGeom();         // persist the premium panel size (free-resize scale) + position (ExtState)
  // User dynamics presets (stored globally in ExtState, shown in the Preset dropdown).
  static constexpr int MAX_USER_PRESETS = 32;
  void ShowDynamicsPresetMenu();                 // build + track the Preset dropdown (factory + user)

  // --- Hard Limiter host glue (v2.4.0 INC-L1) --------------------------------
  // Debounced preview worker: computes the limiter envelope on a COPY of the
  // standalone buffer (spectral_view threading pattern; generation counter
  // instead of a cancel flag), decimates it to min-gain buckets for the
  // waveform GR band, and measures the in/out peaks for the panel readouts.
  void ShowLimiterPresetMenu();          // 4 factory presets under the preset box
  void SaveLimiterParams();              // lim_* ExtState session defaults
  void RestoreLimiterParams();           // (first run -> preset 0) + panel offsets
  void SaveLimiterGeom();                // lim_off_x / lim_off_y
  void DrawLimiterOverlay(HDC hdc);      // top-anchored GR band + trace (GDI pass)
  void MarkLimiterParamsChanged();       // debounce tick + gen bump + pending "..."
  void InvalidateLimiterPreview();       // buffer changed (apply/undo/load)
  void LimiterPreviewTick();             // OnTimer: draft/full launch + finish pump
  void StartLimiterPreview();            // FULL: detection + refinement + OUT measure
  void LimiterPreviewThread(std::vector<double> audio, int frames, int nch,
                            int sr, LimiterParams p, uint64_t gen);
  // DRAFT path (live knob response): the expensive detector peaks depend only
  // on the buffer + truePeak/link, so they are cached once by the full pass
  // and knob changes re-run just the cheap envelope chain - no debounce, the
  // GR band tracks the drag. The refined full pass upgrades it after settle.
  void StartLimiterPreviewDraft();
  void LimiterPreviewDraftThread(std::shared_ptr<const std::vector<double>> peaks,
                                 int frames, int chains, int sr,
                                 LimiterParams p, uint64_t gen);
  // Background Apply (podcast-length files must not freeze the window): the
  // worker limits a COPY with title progress; LimiterApplyTick swaps the
  // result in only when the live buffer is untouched, else discards it.
  void LimiterApplyTick();
  void LimiterApplyThread(int nch, int sr, int s0, int s1, int ramp);
  std::thread m_limApplyThread;
  std::atomic<bool> m_limApplyBusy{ false };
  std::atomic<bool> m_limApplyCancel{ false };
  std::atomic<bool> m_limApplyDone{ false };
  std::atomic<int> m_limApplyPct{ 0 };
  std::vector<double> m_limApplyOut;   // worker-owned copy until Done
  LimiterResult m_limApplyResult;      // worker-written, read after Done
  LimiterParams m_limApplyParams;      // params captured at launch
  int m_limApplyS0 = 0, m_limApplyS1 = 0, m_limApplyFrames = 0;
  uint64_t m_limApplySerial = 0;       // buffer identity at launch
  int m_limApplyFileIdx = -1;
  std::shared_ptr<const std::vector<double>> m_limPeakCache; // pre-gain detector peaks
  int m_limPeakCacheFrames = 0, m_limPeakCacheChains = 0;   // cache identity...
  bool m_limPeakCacheTP = true;                             // ...(under the mutex)
  bool m_limFullPending = false;  // draft on screen: schedule the refined pass
  bool m_limPrevDraft = false;    // current result lacks the OUT measure (under the mutex)
  std::thread m_limPrevThread;
  std::atomic<bool> m_limPrevComputing{ false };
  std::atomic<bool> m_limPrevFinished{ false };  // one-shot: pump repaints + stats
  std::atomic<uint64_t> m_limPrevGen{ 1 };       // bumped on param/buffer change
  std::mutex m_limPrevMutex;                     // guards the result block below
  std::vector<float> m_limPrevEnvMin;            // decimated min gain per bucket
  int m_limPrevFrames = 0;                       // buffer identity of the result
  LimiterResult m_limPrevResult;
  bool m_limPrevValid = false;
  bool m_limPrevDirty = false;                   // params/buffer changed since compute
  DWORD m_limPrevChangeTick = 0;                 // debounce reference (~150 ms)

  std::vector<DynUserPreset> LoadUserPresets();  // parse user presets from ExtState
  void SaveUserPresets(const std::vector<DynUserPreset>& list);
  void AddUserPreset();                          // prompt for a name, save current panel params
  bool ApplyUserPreset(int idx);                 // load preset idx into the panel; false if out of range
  void DeleteUserPreset(int idx);

  // dpr-watchdog (v2.2.0): cache the last GetUiDpr() seen in OnTimer; on a change
  // (monitor drag / OS scale change), force a full repaint so premium surfaces
  // re-blit crisp. -1 = uninitialised (seeded on the first tick). Costs nothing
  // when idle (one cheap comparison per ~33ms tick).
  double m_lastUiDpr = -1.0;
  // True once the user has manually chosen a UI scale (panel/menu); persisted as
  // ExtState "ui_scale_user". While false, WM_DPICHANGED (Win) may auto-follow the
  // monitor's DPI; a manual choice is never stomped. (Read on all platforms; the
  // only consumer is the Windows WM_DPICHANGED handler.)
  bool m_uiScaleUserSet = false;

  static AudioClipboard s_clipboard;
  static const int TIMER_REFRESH = 100;
  static const int TIMER_INTERVAL_MS = 33;
};
