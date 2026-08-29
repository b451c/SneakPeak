// edit_view.h — Main SneakPeak window class
#pragma once

#include "platform.h"
#include "config.h"
#include "globals.h"
#include "waveform_view.h"
#include "audio_engine.h"
#include "audio_stream.h"
#include "wav_writer.h"
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
#include "loop_finder.h"
#include "looplab_panel.h"
#include "oneshot_panel.h"
#include "ui_render.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <functional>

namespace WavInplace { struct Progress; }

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
  int loopStartFrame = -1;          // Loop Lab region (v2.4 INC-A1), frames
  int loopEndFrame = -1;
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
  CM_LIM_SAVE_PRESET,                                  // "Save preset as..."
  CM_LIM_USER_PRESET_BASE,                             // + MAX_USER_PRESETS apply entries
  CM_LIM_USER_PRESET_LAST = CM_LIM_USER_PRESET_BASE + 32,
  CM_LIM_DEL_PRESET_BASE,                              // + MAX_USER_PRESETS delete entries
  CM_LIM_DEL_PRESET_LAST = CM_LIM_DEL_PRESET_BASE + 32,
  // Loop Lab (v2.4 INC-A1) - standalone loop region.
  CM_LOOP_FROM_SELECTION,                              // set loop from the time selection
  CM_AUDITION_LOOP,                                    // gapless region preview (toggle)
  CM_CLEAR_LOOP,
  CM_FIND_LOOP_POINTS,                                 // INC-A2: NCC candidate finder
  CM_WELD_LOOP,                                        // INC-A3: crossfade the seam
  CM_LOOP_WRITE_SMPL,                                  // INC-A4: write smpl on save (check)
  CM_AUDITION_SEAM,                                    // seam-only audition (toggle)
  CM_ONESHOT_FACTORY,                                  // INC-B1: open the prep panel
  CM_LOOP_LAB,                                         // INC-A5: open the Loop Lab panel
  CM_EDIT_COPY_STANDALONE,                             // INC-B4: ITEM -> copy as standalone tab
  CM_MULTI_MODE_LANES,                                 // row 15 #2: Lanes (per Track) - appended, ids are numeric in specs
  CM_SPECTRAL_FFT_BASE,                                // row 15 #3: + SpectralView::kFftSizes index (512..4096)
  CM_SPECTRAL_FFT_LAST = CM_SPECTRAL_FFT_BASE + 3,
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
  void SaveStandaloneFile();   // Ctrl+S / the SneakPeak_SaveStandalone action (no-op outside Standalone)

  static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  // Called directly from accelerator callback (SWS pattern - no SendMessage bounce)
  void OnKeyDown(WPARAM key);
  // Update check (A5.4): curl runs on a worker, the result is picked up in
  // OnTimer; the shared state outlives the window so a late reply is harmless.
  struct UpdateCheck {
    std::atomic<bool> done{false};
    std::string response;
    int rc = -1;
  };
  std::shared_ptr<UpdateCheck> m_updateCheck;
  void PollUpdateCheck();
  // True when OnKeyDown would act on `key` in the current state - the
  // accelerator eats a key only then, everything else reaches REAPER (A5.1).
  bool KeyHasAction(WPARAM key) const;
  bool AnyDragActive() const;      // any mouse-capture drag (waveform, envelope, panels)
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
  // Every repaint request goes through here: it marks the cached scene stale.
  // The playback tick alone calls InvalidateRect directly (playhead only).
  void Invalidate(const RECT* rc = nullptr);
  void InvalidateOverlay(const RECT* rc = nullptr);   // premium overlay ticks: scene stays cached
  void ReleaseScene();
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
  void PollProjectState();          // A6.2: follow edits made outside SneakPeak
  bool SegmentsMatchProject() const; // every segment/layer still describes its item
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
  // Windows: REAPER holds the file of every take that references it, so a
  // Standalone save over a file that is also in the project needs those items
  // offline for the write (F22 for saves, s12). No-ops elsewhere.
  int TakeItemsUsingPathOffline(const std::string& path, std::vector<MediaItem*>& savedSel);
  void BringItemsBackOnline(const std::vector<MediaItem*>& savedSel);
  // Windows, ITEM-mode in-place edits: the bracket around a write whose
  // selection may move meanwhile (F5 background job). No-ops elsewhere.
  void TakeSelectionOffline();
  void BringOfflineItemsBackOnline();
  int ReplaceSourceInTimeline(const std::string& oldPath, const std::string& newPath);
  void DoReplaceSourceInTimeline();

  // Fetch latest release tag from GitHub API via curl; toast the result
  // (up-to-date / update available with version numbers). Blocking with 5s timeout.
  void DoCheckForUpdate();

  // Clipboard operations
  bool DoCopy();   // false = nothing reached the clipboard (A2.1: Cut must not delete)
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
  void DoApplyLimiterItem();             // INC-L2: ITEM mode, destructive-rewrite path
  void DoRunOneShot();                   // v2.4 INC-B1/B2: per-slice trim/fade/normalize -> WAVs
  bool SingleBufferModeOk() const;            // INC-B3: Standalone or plain ITEM mode, samples in
  bool SingleItemViewOk() const;              // 8g: the same view shape, samples not required
  // INC-B2 slice helpers: the slice list for the active mode (whole file /
  // regions-markers / silence gaps), the per-slice kept-bounds (shared by the
  // exporter and the live preview so they can never disagree), one slice's
  // export, and the native naming-pattern edit dialog.
  std::vector<std::pair<int, int>> OneShotBuildSlices(const OneShotParams& p);
  // Kept bounds of [s0, s1) inside `data` (interleaved, `nch`, `sr`): the
  // preview passes the working buffer, the exporter its full-rate slice.
  static bool OneShotTrimBounds(const OneShotParams& p, const double* data, int nch,
                                int sr, int s0, int s1, int* a, int* b);
  // Samples of [t0, t1) of view time at full rate: Standalone copies its
  // buffer, ITEM modes stream the source (export_stream.cpp). nch/sr = the
  // result's. False = nothing readable.
  bool SliceSamples(double t0, double t1, std::vector<double>& out, int* nch, int* sr);
  int  OneShotExportSlice(const OneShotParams& p, int s0, int s1,
                          const std::string& outPath, char* note, size_t noteSz,
                          char* err, size_t errSz);   // 1 written, 0 skip, -1 abort
  void EditOneShotPattern();
  bool OneShotSourceParts(std::string* dir, std::string* base);  // export dir + {name}
  void OpenOneShotFolder();              // OPEN FOLDER button: reveal the export dir
  void DoEditCopyStandalone();           // v2.4 INC-B4: ITEM buffer -> {name}_edit.wav -> new tab
  void SaveOneShotParams();              // os_* ExtState session defaults
  void RestoreOneShotParams();
  void SaveOneShotGeom();
  void SaveLoopLabParams();              // loop_weld_ms session default
  void RestoreLoopLabParams();           // + panel offsets
  void SaveLoopLabGeom();                // loop_off_x / loop_off_y
  // Live prep preview (no blind knobs): kept spans recomputed on param
  // change, drawn as dimmed cut zones + fade ramps while the panel is open.
  // One span per slice (INC-B2) - WHOLE mode keeps the single trim span.
  void OneShotPreviewTick();
  void DrawOneShotOverlay(HDC hdc);
  std::vector<std::pair<int, int>> m_osSpans;   // kept regions [a, b); empty = no preview
  bool m_osPreviewDirty = true;
  uint64_t m_osPreviewSerial = 0;

  // Navigation
  void NavigateToMarker(bool forward);
  void DoLoopSelection();

  // Timeline view (post-cut sibling items with gaps)
  std::vector<MediaItem*> FindSiblingItems(MediaTrack* track, MediaItem* sourceItem);
  void RefreshTimelineView();

  // Helpers for destructive ops
  void GetSelectionSampleRange(int& startFrame, int& endFrame) const;
  void WriteAndRefresh();
  // True when the working buffer maps 1:1 onto the source file (rate, offset,
  // playrate, length, CHANNELS) - the only case a whole-file write is valid.
  bool BufferCoversWholeFile(const std::string& path, WavInfo& srcInfo) const;
  bool BeginDestructiveWrite(std::string& path);
  bool DestructiveSourceOk();   // false + toast on SECTION / reversed sources (A1.2)
  void EndDestructiveWrite(bool written);   // synchronous whole-file path: rollback + finish
  void FinishDestructiveWrite(bool written, bool restored);   // the refresh / failure report
  std::string UndoSnapshotPath() const;     // the free undo slot's file (a/b alternate)
  bool RestoreFromSnapshot();   // pre-edit copy back over the current take's file (A1.4)
  void GetSelectionSourceRange(int64_t& startFrame, int64_t& endFrame) const;
  void DiscardItemUndo();
  void SyncSelectionToReaper();
  void UpdateTitle();
  bool UndoSave();   // false = no pre-edit copy: the caller must not touch the file (A1.3)
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
  // Cached scene (A9.3): OnPaint renders into this persistent buffer only when
  // Invalidate() ran or the waveform peaks went stale; WM_PAINT blits it and
  // draws the playhead + overlay on top, so playback ticks cost a blit.
  HDC m_sceneDC = nullptr;
  int m_sceneW = 0, m_sceneH = 0;
  bool m_sceneValid = false;
#ifdef _WIN32
  HBITMAP m_sceneBmp = nullptr, m_sceneOldBmp = nullptr;
#endif
  WaveformView m_waveform;
  Toolbar m_toolbar;

  bool m_pendingClose = false;
  bool m_isDocked = false;
  bool m_hasFocus = false;
  int m_timelineEditGuard = 0; // ticks to suppress timeline exit after edit operation
  int m_lastProjectState = -1;  // GetProjectStateChangeCount seen by PollProjectState
  WaveformSelection m_pendingSelRestore = {}; // selection to restore after guarded reload
  bool m_dragging = false;
  bool m_inMouseUp = false;         // re-entrancy guard: ReleaseCapture() inside OnMouseUp raises WM_CAPTURECHANGED
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
  double m_envDragPlayrate = 1.0;            // take D_PLAYRATE: view time -> take-envelope time
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
  std::vector<double> m_meterLiveBuf;   // 8g: meter window read through the live accessor
  SpectralView m_spectral;
  MinimapView m_minimap;
  DynamicsEngine m_dynamics;
  DynamicsPanel m_dynamicsPanel;
  SettingsPanel m_settingsPanel;  // premium Settings overlay (UI scale; migrated prefs next)
  LimiterPanel m_limiterPanel;    // premium HARD LIMITER overlay (v2.4.0 INC-L1)
  OneShotPanel m_oneShotPanel;    // premium ONE-SHOT PREP overlay (v2.4 INC-B1)
  LoopLabPanel m_loopLabPanel;    // premium LOOP LAB overlay (v2.4 INC-A5)
  RECT m_gearRect = {};           // settings gear in the mode bar (premium build only)
  bool m_dynamicsVisible = false;
  bool m_spectralVisible = false;
  bool m_spectralWasLoading = false; // OnTimer: repaint pump while spectrum computes
  unsigned m_spectralPaintedGen = ~0u;  // SpectralView generation painted Ready (~0u = none)
  // Drop the spectrum for a recompute. A short file recomputes faster than one
  // timer tick, so the "was loading" pump never fires; ClearSpectrum bumps the
  // generation and OnTimer paints once per generation that reaches Ready.
  void ResetSpectrum() { m_spectral.ClearSpectrum(); }
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

  // Background ITEM audio load (INC-PK1, design_sdk_peaks_hybrid.md): the
  // waveform shows SDK peaks instantly; the sample buffer decodes here in
  // OnTimer slices and installs on finish. Ops that need samples gate on
  // ItemAudioReady().
  // Phase 2a: one job per take - single item, every timeline/SET segment,
  // every multi-item layer - serviced in order by StepItemAudioLoad.
  struct ItemAudioJob {
    MediaItem_Take* take = nullptr;
    MediaItem* item = nullptr;
    AudioAccessor* accessor = nullptr; // created lazily when the job starts
    int dstFrame = 0;                  // shared-buffer views: frame offset in samples
    int frames = 0;
    int srcNch = 1;                    // channels to read (multi: source count)
    int layerIdx = -1;                 // multi-item: layer index (own staging)
    std::vector<double> staging;       // multi-item only
  };
  struct ItemAudioLoad {
    std::vector<ItemAudioJob> jobs;
    size_t jobIdx = 0;
    int framesRead = 0;                // within the current job
    int doneFrames = 0, totalFrames = 0; // progress
    std::vector<double> samples;       // shared buffer (single/timeline/SET)
    int readRate = 0, nch = 0;
    bool multi = false;
    bool single = false;               // single item: I_CHANMODE fold on finish
    unsigned generation = 0;           // WaveformView::GetLoadGeneration at start
    int lastPct = -1;
    bool active = false;
  };
  ItemAudioLoad m_itemLoad;

  // Phase 2b (dynamics_pipeline.cpp): knob drags never run the engine inline.
  struct DynWorker {
    DynamicsEngine engine;             // computes here; swapped with m_dynamics on completion
    std::thread thread;
    std::atomic<bool> busy{false};
    std::atomic<bool> hasResult{false};
  };
  DynWorker m_dynWorker;
  bool m_dynParamsDirty = false;       // knob moved since the last job
  DWORD m_dynLiveWriteDue = 0;         // debounced Live write deadline (0 = none)
  void StepDynamicsPipeline();         // OnTimer: start/collect jobs, debounced Live write
  void FlushDynamicsPipeline();        // mouse-up: finish + write the final position
  bool TakeDynamicsResult();
  void LiveWriteEnvelope();
  void JoinDynamicsWorker(bool discardResult); // true = the audio/view changes: drop everything
  // 8f (dynamics_pipeline.cpp): item views analyse Dynamics from the stream.
  // The job streams the view at full rate on its own thread into a DynTrace
  // (accessors opened here before the thread starts, closed after the join);
  // the DynWorker above then analyses the finished, shared trace.
  struct DynTraceJob {
    AudioStream stream;
    DynTraceBuilder builder;
    std::thread thread;
    std::atomic<bool> abort{false};
    std::atomic<bool> done{false};
    std::atomic<int64_t> framesDone{0};
    std::shared_ptr<const DynTrace> result;   // set by the thread before done
    DynTraceKey key;
    unsigned generation = 0;
    int lastPct = -1;
    bool active = false;
  };
  DynTraceJob m_dynTraceJob;
  std::shared_ptr<const DynTrace> m_dynTrace;  // the current view's trace
  unsigned m_dynTraceGen = ~0u;                 // load generation it belongs to
  unsigned m_dynTraceFailedGen = ~0u;           // generation whose stream errored (no retry loop)
  bool m_dynApplyPending = false;               // Apply pressed before a result existed
  const DynamicsParams& CurrentDynParams() const;
  bool DynamicsWanted() const;
  bool DynTraceCurrent(const DynamicsParams& p) const;
  void StartDynTraceJob();
  void StepDynTraceJob();       // OnTimer: abort/finish/progress + self-healing start
  void AbortDynTraceJob();      // main thread: abort + join + close the accessors
  void RequestDynamicsAnalysis();   // the ONE entry for "analysis must be (re)done"
  // 8e (export_stream.cpp): Edit Copy streams the item at full rate into its
  // file in OnTimer slices - the working buffer is downsampled on long items.
  struct ExportPump {
    AudioStream stream;
    WavWriter writer;
    std::string outPath;
    std::vector<double> chunk;
    unsigned generation = 0;           // abort when the view moves on
    int lastPct = -1;
    bool active = false;
  };
  ExportPump m_exportPump;
  void StartEditCopyExport(const std::string& outPath);
  void StepExportPump();        // OnTimer slice + progress title; installs the tab on finish
  void AbortExportPump();       // item change / destructive write / close
  // Drag export (ITEM modes): the view range streamed at the source rate into
  // a WAV in the source's format, item/segment fades baked per chunk.
  std::string ExportItemRangeToWav(double t0, double t1);
  void BakeItemFades(double* chunk, int64_t viewFrame0, int n, int nch, int sr) const;
  // F5 (destructive_job.cpp): Reverse / Gain / DC Remove rewrite the source file
  // IN PLACE on a worker (wav_inplace.h) - the pre-edit copy, the op and the
  // rollback copy all run there; the main thread brackets the write, shows
  // progress in the title and finalizes on the finishing tick. The view is
  // pinned to the item meanwhile (design_f5_background_destructive.md).
  using DestructiveOp = std::function<bool(const std::string& path, int64_t s0, int64_t s1,
                                           const WavInplace::Progress* prog)>;
  struct DestructiveJob {
    std::thread thread;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<int> phase{0};         // 0 = pre-edit copy, 1 = the op
    std::atomic<int> pct{0};
    bool snapshotOk = false, ok = false, restored = false;   // worker -> main, read after the join
    std::string path, snapshot;        // the source file, its pre-edit copy (free undo slot)
    int64_t s0 = 0, s1 = 0;            // the selection in FILE frames
    std::string verb, doing, undoLabel;   // "Reverse", "Reversing", REAPER's undo label
    DestructiveOp op;
    std::function<void()> displayEdit; // the working-buffer edit, applied on success only
    MediaItem* selItem = nullptr;      // REAPER's selection at start: re-sync on finish
    int selCount = 0;
    std::string lastTitle;
    bool active = false;
  };
  DestructiveJob m_destructiveJob;
  void StartDestructiveJob(const char* verb, const char* doing, const char* undoLabel,
                           DestructiveOp op, std::function<void()> displayEdit);
  void DestructiveJobThread();
  void StepDestructiveJob();       // OnTimer: progress title, finalize on completion
  void FinalizeDestructiveJob();   // main thread after the join: undo bookkeeping + refresh
  void AbortDestructiveJob();      // window close: cancel + join + finalize
  bool DestructiveJobBusy();       // toast + true while a job runs (file readers/writers refuse)
  unsigned m_itemLoadFailedGen = ~0u;  // generation that produced no jobs (no retry loop)
  bool m_itemLoadOverCap = false;      // last start refused: buffer > kMaxBufferBytes
  // 8g: a lazy view (WaveformView::ItemBufferIsLazy) loads only when `wanted` -
  // RequireItemAudio, or the OnTimer self-heal while a sample panel is open.
  void StartItemAudioLoad(bool wanted = false);
  bool SamplePanelOpen() const { return m_spectralVisible || m_oneShotPanel.IsVisible(); }
  void StepItemAudioLoad();     // OnTimer slice + progress title
  void FinishItemAudioLoad();
  void AbortItemAudioLoad();
  bool ItemAudioReady() const;                 // buffer present (or standalone)
  bool RequireItemAudio(const char* what);     // gate: toast + false while loading
  void StepSdkPeaksBuild();     // pump PCM_Source_BuildPeaks when .reapeaks absent
  int m_sdkPeaksBuildStage = -1; // -1 idle, 0 begun
  void EvictStandaloneTabIfFull();
  void InstallStandaloneTab(const std::string& spath);
  void SaveStandaloneFileAs();
  void BakePendingFades();
  void StandalonePlayStop();
  void StandaloneAuditionLoop();                       // Loop Lab: gapless region toggle
  void StandaloneAuditionSeam();                       // seam-only: tail+wrap+head+gap
  void RestartLoopAudition();                          // re-arm after loop edits
  std::vector<double> StandaloneFadedCopy();           // buffer + pending fades
  void DoWeldLoop(double crossfadeMs);                 // INC-A3: equal-power seam bake
  bool StandaloneWritePreviewFile(int startFrame, int endFrame);
  bool StandaloneStartPreviewPlayback(double curpos, bool loopFlag, double displayOffset);
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
  // Destructive ITEM undo (single level, 2026-07-02): destructive edits
  // rewrite the source FILE, which REAPER's native undo cannot restore -
  // so UndoSave snapshots the pre-edit FILE (byte copy in the temp dir; never
  // the working buffer, which is downsampled on long items - F6) and
  // UndoRestore copies it back into the same inode (path-checked).
  std::string m_itemUndoPath;   // the source file the snapshot belongs to
  std::string m_itemUndoFile;   // the snapshot copy (empty = no snapshot)
  std::vector<MediaItem*> m_offlineItems;   // Windows: items 40440 took offline for the write in flight
  int m_itemUndoSlot = 0;       // next snapshot name (_a/_b alternate: the old copy outlives a failed new one)
  // Standalone undo/redo stacks (full or range snapshots - StandaloneUndoEntry)
  std::vector<StandaloneUndoEntry> m_standaloneUndoStack;
  std::vector<StandaloneUndoEntry> m_standaloneRedoStack;
  static const int MAX_STANDALONE_UNDO = 20;
  void StandaloneUndoSave();                              // full-buffer snapshot
  void StandaloneUndoPushFull(std::vector<double>&& oldData); // zero-copy full slot
  void StandaloneUndoSaveRange(int startFrame, int numFrames); // bounded edits
  // Bumped on every standalone buffer mutation (edit/undo/redo/tab/load): the
  // background limiter apply swaps its result in only if this is unchanged,
  // and the preview worker gates its peak-cache store on it (atomic: read
  // from the worker at handoff).
  std::atomic<uint64_t> m_standaloneBufferSerial{ 0 };
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
  char m_toastText[128] = {};   // refusal messages run to ~90 chars
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
  // Loop Lab audition (v2.4 INC-A1): the temp WAV holds JUST the loop region
  // and the preview register loops it natively; offset maps curpos back to
  // absolute waveform time for the playhead/meters.
  bool   m_previewLoop = false;
  double m_previewLoopOffset = 0.0;
  // Seam audition: the temp WAV is tail + head + 250 ms gap; the playhead
  // needs a piecewise map back to absolute time (MapPreviewPos).
  bool   m_previewSeam = false;
  double m_previewSeamPre = 0.0;     // tail length (s)
  double m_previewSeamPost = 0.0;    // head length (s)
  double m_previewSeamTailT0 = 0.0;  // absolute time of the tail start
  double m_previewSeamHeadT0 = 0.0;  // absolute time of the head start
  double MapPreviewPos(double pos) const;
  int    m_previewCacheStart = 0;    // frame range the cached temp WAV holds
  int    m_previewCacheEnd = -1;
  PCM_source* m_previewSrc = nullptr;
  std::string m_previewTempPath;

  // Working set (locked multi-item edit range)
  struct WorkingSet {
    MediaTrack* track = nullptr;
    std::string trackGuid;         // REAPER recycles track addresses: identity beyond the pointer
    std::vector<MediaItem*> items; // explicit item list (only user-selected items)
    double startPos = 0.0;         // timeline start (for ripple edit bounds)
    double endPos = 0.0;           // timeline end (for ripple edit bounds)
    bool active = false;           // currently displayed
    bool dormant = false;          // user clicked away, set preserved for restore
  };
  WorkingSet m_workingSet;
  void LoadWorkingSet();
  void RefreshWorkingSet();
  bool PruneWorkingSet();   // drop dead item/track pointers; false = set gone (reset)
  static std::string TrackGuid(MediaTrack* track);
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
  void DoApplyDynamicsStandalone();  // v2.4 INC-D1: GR curve multiplied into the buffer
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
  void DrawLoopRegion(HDC hdc);          // Loop Lab brackets + tinted ruler strip
  int m_loopDrag = 0;                    // bracket drag: 0 none, 1 start, 2 end
  bool m_writeLoopOnSave = true;         // INC-A4: append smpl on save (persisted)
  // Loop Lab finder (INC-A2): a worker scores loop-point candidates on a COPY
  // of the buffer (NCC + spectral tie-break); the results render as numbered
  // pins on the ruler - click a pin to set the loop and start the audition.
  void StartLoopFind();
  void LoopFindThread(std::vector<double> audio, int frames, int nch, int sr,
                      uint64_t serial);
  void LoopFindTick();
  void DrawLoopPins(HDC hdc);
  int HitTestLoopPin(int x, int y) const;   // -1 = none
  std::thread m_loopFindThread;
  std::atomic<bool> m_loopFindBusy{ false };
  std::atomic<bool> m_loopFindDone{ false };
  std::vector<LoopCandidate> m_loopFindResult;  // worker-written, read after Done
  uint64_t m_loopFindSerial = 0;                // buffer identity at launch
  std::vector<LoopCandidate> m_loopCandidates;  // pins on display (transient)
  void MarkLimiterParamsChanged();       // debounce tick + gen bump + pending "..."
  void InvalidateLimiterPreview();       // buffer changed (apply/undo/load)
  void LimiterPreviewTick();             // OnTimer: draft/full launch + finish pump
  void StartLimiterPreview();            // FULL: detection + refinement + OUT measure
  void LimiterPreviewThread(std::vector<double> audio, int frames, int nch,
                            int sr, LimiterParams p, uint64_t gen,
                            uint64_t bufSerial);
  std::atomic<int> m_limPrevPct{ 0 };    // full-pass progress -> panel readouts
  bool m_limPrevDraftRunning = false;    // in-flight worker is a draft (main thread)
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

  // Limiter user presets (v2.4.0; parallel to the dynamics set below, blob
  // key lim_user_presets, locale-safe LimiterParamsToString payload).
  std::vector<DynUserPreset> LoadLimUserPresets();
  void SaveLimUserPresets(const std::vector<DynUserPreset>& list);
  void AddLimUserPreset();                       // prompt for a name, save panel params
  bool ApplyLimUserPreset(int idx);              // load into the panel (+ name in the box)
  void DeleteLimUserPreset(int idx);

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
