// SneakPeak — REAPER Extension for Item Waveform Editing
// Inspired by Adobe Audition's Edit View / Waveform Editor

#include "platform.h"

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL

// Core
#define REAPERAPI_WANT_DockWindowAddEx
#define REAPERAPI_WANT_DockWindowRemove
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_plugin_register
#define REAPERAPI_WANT_GetMainHwnd

// Media items
#define REAPERAPI_WANT_CountSelectedMediaItems
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_SetMediaItemSelected
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String


// Audio accessor
#define REAPERAPI_WANT_CreateTakeAudioAccessor
#define REAPERAPI_WANT_DestroyAudioAccessor
#define REAPERAPI_WANT_GetAudioAccessorSamples

// Transport
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_GetPlayPosition
#define REAPERAPI_WANT_GetPlayPosition2
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_OnPlayButton
#define REAPERAPI_WANT_OnStopButton

// Preview playback (standalone mode)
#define REAPERAPI_WANT_PlayPreview
#define REAPERAPI_WANT_StopPreview

// Markers
#define REAPERAPI_WANT_EnumProjectMarkers3
#define REAPERAPI_WANT_AddProjectMarker2
#define REAPERAPI_WANT_DeleteProjectMarkerByIndex
#define REAPERAPI_WANT_SetProjectMarkerByIndex2

// Source / destructive editing
#define REAPERAPI_WANT_PCM_Source_CreateFromFile
#define REAPERAPI_WANT_SetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceFileName
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_GetSetMediaItemInfo_String

// Undo
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2

// UI refresh
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_UpdateTimeline
#define REAPERAPI_WANT_PreventUIRefresh

// Item manipulation
#define REAPERAPI_WANT_SplitMediaItem
#define REAPERAPI_WANT_DeleteTrackMediaItem
#define REAPERAPI_WANT_GetMediaItem_Track
#define REAPERAPI_WANT_AddMediaItemToTrack
#define REAPERAPI_WANT_AddTakeToMediaItem
#define REAPERAPI_WANT_UpdateItemInProject

// Track items (for track follow)
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackNumMediaItems
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetMediaItemNumTakes
#define REAPERAPI_WANT_GetMediaItemTake
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_CountSelectedTracks

// Track properties (solo)
#define REAPERAPI_WANT_GetSetMediaTrackInfo

// Time selection
#define REAPERAPI_WANT_GetSet_LoopTimeRange2

// Tempo map (bars/beats ruler)
#define REAPERAPI_WANT_TimeMap2_timeToBeats
#define REAPERAPI_WANT_TimeMap2_beatsToTime
#define REAPERAPI_WANT_TimeMap_GetTimeSigAtTime
#define REAPERAPI_WANT_TimeMap_GetMeasureInfo

// UI / dialogs
#define REAPERAPI_WANT_GetUserInputs

// Theme
#define REAPERAPI_WANT_GetThemeColor

// Master track metering
#define REAPERAPI_WANT_GetMasterTrack
#define REAPERAPI_WANT_Track_GetPeakInfo
#define REAPERAPI_WANT_Track_GetPeakHoldDB

// Project path
#define REAPERAPI_WANT_GetProjectPathEx
#define REAPERAPI_WANT_EnumProjects

// Pointer validation
#define REAPERAPI_WANT_ValidatePtr2

// Envelope API
#define REAPERAPI_WANT_GetTakeEnvelopeByName
#define REAPERAPI_WANT_CountEnvelopePoints
#define REAPERAPI_WANT_GetEnvelopePoint
#define REAPERAPI_WANT_GetEnvelopeScalingMode
#define REAPERAPI_WANT_ScaleFromEnvelopeMode
#define REAPERAPI_WANT_Envelope_Evaluate
#define REAPERAPI_WANT_SetEnvelopePoint
#define REAPERAPI_WANT_InsertEnvelopePointEx
#define REAPERAPI_WANT_DeleteEnvelopePointEx
#define REAPERAPI_WANT_DeleteEnvelopePointRange
#define REAPERAPI_WANT_Envelope_SortPoints
#define REAPERAPI_WANT_GetEnvelopePointByTime
#define REAPERAPI_WANT_ScaleToEnvelopeMode
#define REAPERAPI_WANT_GetEnvelopeStateChunk
#define REAPERAPI_WANT_SetEnvelopeStateChunk
#define REAPERAPI_WANT_GetSetEnvelopeInfo_String

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "globals.h"
#include "edit_view.h"
#include "theme.h"
#include "debug.h"
#include <memory>

static std::unique_ptr<SneakPeak> g_sneakPeak;
static int g_cmdToggle = 0;
static int g_cmdLoadItem = 0;
static int g_cmdTrackView = 0;
static int g_cmdMasterView = 0;

// Toolbar commands as named actions (forum #51): every toolbar button is
// bindable in REAPER's Action List, so the Action List is the keymap editor.
// The action runs the exact toolbar-click path (RunToolbarCommand).
struct ToolbarActionDef { const char* id; const char* desc; int button; };
static const ToolbarActionDef kToolbarActions[] = {
  { "SneakPeak_ZoomIn",     "SneakPeak: Zoom in",              TB_ZOOM_IN },
  { "SneakPeak_ZoomOut",    "SneakPeak: Zoom out",             TB_ZOOM_OUT },
  { "SneakPeak_ZoomFit",    "SneakPeak: Zoom to fit",          TB_ZOOM_FIT },
  { "SneakPeak_ZoomSel",    "SneakPeak: Zoom to selection",    TB_ZOOM_SEL },
  { "SneakPeak_Play",       "SneakPeak: Play",                 TB_PLAY },
  { "SneakPeak_Stop",       "SneakPeak: Stop",                 TB_STOP },
  { "SneakPeak_Normalize",  "SneakPeak: Normalize",            TB_NORMALIZE },
  { "SneakPeak_FadeIn",     "SneakPeak: Fade in",              TB_FADE_IN },
  { "SneakPeak_FadeOut",    "SneakPeak: Fade out",             TB_FADE_OUT },
  { "SneakPeak_Reverse",    "SneakPeak: Reverse",              TB_REVERSE },
  { "SneakPeak_VZoomIn",    "SneakPeak: Vertical zoom in",     TB_VZOOM_IN },
  { "SneakPeak_VZoomOut",   "SneakPeak: Vertical zoom out",    TB_VZOOM_OUT },
  { "SneakPeak_VZoomReset", "SneakPeak: Vertical zoom reset",  TB_VZOOM_RESET },
};
constexpr int kToolbarActionCount = (int)(sizeof(kToolbarActions) / sizeof(kToolbarActions[0]));
static int g_cmdToolbar[kToolbarActionCount] = {};
static MediaItem* g_lastSelectedItem = nullptr;
static int g_lastSelectedCount = 0;

// --- #83: never swallow keys the user bound to OUR OWN actions ---------------
// The "<accelerator" consume list (bare letters/Space/...) runs ahead of REAPER's
// shortcut processing whenever our window has focus (or the docked cursor-hover
// fallback engages). If the user bound e.g. "SneakPeak: Open/Close SneakPeak" to
// a bare letter, we would eat the very key meant to toggle us ("launch shortcut
// dead when docked"). Before consuming, match the pressed key+modifiers against
// the main-section shortcuts of our registered commands; on a match, return -666
// so the main window runs the binding. Fail-open: a null API or an unrecognized
// shortcut description keeps today's behavior (we consume).
static KbdSectionInfo* (*g_SectionFromUniqueID)(int) = nullptr;
static int (*g_CountActionShortcuts)(KbdSectionInfo*, int) = nullptr;
static bool (*g_GetActionShortcutDesc)(KbdSectionInfo*, int, int, char*, int) = nullptr;

static bool ieqToken(const char* a, const char* b)
{
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
    if (ca != cb) return false;
    ++a; ++b;
  }
  return !*a && !*b;
}

// Key-name token from a REAPER shortcut description -> VK code. Only the keys our
// consume list can eat need recognizing; anything else returns 0 (= no match).
static int descTokenToVk(const char* t)
{
  if (t[0] && !t[1]) {
    char c = t[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (int)c;
    return 0;
  }
  if (ieqToken(t, "Space")) return VK_SPACE;
  if (ieqToken(t, "Tab")) return VK_TAB;
  if (ieqToken(t, "Esc") || ieqToken(t, "Escape")) return VK_ESCAPE;
  if (ieqToken(t, "Backspace")) return VK_BACK;
  if (ieqToken(t, "Delete") || ieqToken(t, "Del")) return VK_DELETE;
  if (ieqToken(t, "Home")) return VK_HOME;
  if (ieqToken(t, "End")) return VK_END;
  if (ieqToken(t, "Left")) return VK_LEFT;
  if (ieqToken(t, "Right")) return VK_RIGHT;
  if (ieqToken(t, "Up")) return VK_UP;
  if (ieqToken(t, "Down")) return VK_DOWN;
  return 0;
}

static bool keyMatchesOurBinding(WPARAM key)
{
  if (!g_SectionFromUniqueID || !g_CountActionShortcuts || !g_GetActionShortcutDesc)
    return false;
  KbdSectionInfo* sec = g_SectionFromUniqueID(0);   // main section
  if (!sec) return false;

  // Current modifier state. SWELL key mapping (mac): physical Cmd = VK_CONTROL,
  // Option = VK_MENU, physical Ctrl = VK_LWIN - matched against the desc tokens.
  const bool mShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool mCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool mAlt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
  const bool mWin   = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;

  int cmds[4 + kToolbarActionCount] = { g_cmdToggle, g_cmdLoadItem, g_cmdTrackView, g_cmdMasterView };
  for (int i = 0; i < kToolbarActionCount; ++i) cmds[4 + i] = g_cmdToolbar[i];
  for (int c = 0; c < 4 + kToolbarActionCount; ++c) {
    if (!cmds[c]) continue;
    const int n = g_CountActionShortcuts(sec, cmds[c]);
    for (int i = 0; i < n; ++i) {
      char desc[128] = {};
      if (!g_GetActionShortcutDesc(sec, cmds[c], i, desc, sizeof(desc)) || !desc[0])
        continue;
      bool dShift = false, dCtrl = false, dAlt = false, dWin = false;
      int vk = 0;
      char tok[64];
      int ti = 0;
      for (const char* p = desc;; ++p) {
        if (*p && *p != '+') { if (ti < 63) tok[ti++] = *p; continue; }
        tok[ti] = '\0';
        ti = 0;
        if (*p && tok[0] == '\0') continue;     // leading '+' or "++" - skip empty
        bool isMod = true;
#ifdef __APPLE__
        if      (ieqToken(tok, "Cmd"))                              dCtrl = true;
        else if (ieqToken(tok, "Ctrl") || ieqToken(tok, "Control")) dWin  = true;
        else if (ieqToken(tok, "Opt") || ieqToken(tok, "Option") ||
                 ieqToken(tok, "Alt"))                              dAlt  = true;
#else
        if      (ieqToken(tok, "Ctrl") || ieqToken(tok, "Control")) dCtrl = true;
        else if (ieqToken(tok, "Alt"))                              dAlt  = true;
        else if (ieqToken(tok, "Win") || ieqToken(tok, "Super"))    dWin  = true;
#endif
        else if (ieqToken(tok, "Shift"))                            dShift = true;
        else isMod = false;
        if (!isMod) vk = descTokenToVk(tok);    // last non-modifier token = the key
        if (!*p) break;
      }
      if (vk && vk == (int)key &&
          dShift == mShift && dCtrl == mCtrl && dAlt == mAlt && dWin == mWin)
        return true;
    }
  }
  return false;
}

// Keyboard accelerator — SWS pattern: call OnKeyDown directly, never SendMessage.
// Return 1 = consumed, -666 = our window has focus but pass to REAPER main, 0 = not ours.
static int translateAccelSneakPeak(MSG* msg, accelerator_register_t* ctx)
{
  if (!g_sneakPeak || !g_sneakPeak->IsVisible() || !g_sneakPeak->GetHwnd()) return 0;

  HWND ourHwnd = g_sneakPeak->GetHwnd();

  // Focus detection: try multiple methods (docked WS_CHILD windows on Windows
  // don't reliably report focus via GetFocus/WM_SETFOCUS)
  bool ours = g_sneakPeak->HasFocus();
  if (!ours) {
    HWND focus = GetFocus();
    ours = (focus == ourHwnd) || (IsChild(ourHwnd, focus) != 0);
  }
  if (!ours && g_sneakPeak->IsDocked()) {
    // Docked fallback: check if mouse cursor is inside our window.
    // User must click in our window to make a selection, so mouse is over us.
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(ourHwnd, &wr);
    ours = PtInRect(&wr, pt) != 0;
  }
  if (!ours) return 0;

  if (msg->message == WM_KEYDOWN) {
    // Inline dynamics value editor open -> route EVERY key to it (digits/./-/Bksp/
    // Enter/ESC) and consume, so typed characters never trigger global shortcuts.
    // Must come before the normal shortcut filter. Inert when not editing (and in the
    // GDI build, where IsDynamicsEditingValue() is always false).
    if (g_sneakPeak->IsDynamicsEditingValue()) {
      g_sneakPeak->HandleDynamicsEditKey(msg->wParam);
      return 1;
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    WPARAM k = msg->wParam;
    bool handled = false;
    if (k == VK_HOME || k == VK_END || k == VK_SPACE || k == VK_ESCAPE || k == VK_TAB) handled = true;
    else if (k == VK_DELETE || k == VK_BACK) handled = true;
    else if (k == VK_UP || k == VK_DOWN || k == VK_LEFT || k == VK_RIGHT) handled = true;
    else if (ctrl && (k == 'C' || k == 'X' || k == 'V' || k == 'Z' || k == 'N' || k == 'A' || k == 'S')) handled = true;
    else if (!ctrl && (k == 'M' || k == 'G' || k == 'E' || k == 'S' || k == 'T' || k == 'D')) handled = true;
    if (handled) {
      // #83: the user's own SneakPeak action binding always wins over our
      // internal editor keys - force it to the main window so it fires.
      if (keyMatchesOurBinding(k)) return -666;
      g_sneakPeak->OnKeyDown(msg->wParam);
      return 1; // we ate this key
    }
  }

  // Our window has focus but we don't want this key.
  // -666 = SWS magic: force passthrough to REAPER main window so global shortcuts work.
  return -666;
}

static accelerator_register_t g_accelReg = { translateAccelSneakPeak, true, nullptr };

static void pollSelectionTimer()
{
  if (!g_sneakPeak || !g_sneakPeak->IsVisible()) return;
  if (!g_CountSelectedMediaItems || !g_GetSelectedMediaItem) return;

  int count = g_CountSelectedMediaItems(nullptr);
  MediaItem* item = (count > 0) ? g_GetSelectedMediaItem(nullptr, 0) : nullptr;

  // In standalone mode, only exit when user selects items in REAPER
  if (g_sneakPeak->IsStandaloneMode()) {
    if (count <= 0) return; // no items selected — stay in standalone
    // Items selected — save standalone state before switching to REAPER
    g_sneakPeak->SaveCurrentStandaloneState();
    // The save MOVED the active buffer out (STA-2) - the view MUST be replaced
    // before the next paint, even when the user clicked the SAME item that was
    // loaded before standalone. Reset the change detector so the load below
    // always fires (it used to skip, painting a moved-out buffer -> crash).
    g_lastSelectedItem = nullptr;
    g_lastSelectedCount = 0;
    // Fall through to load them (exits standalone via ClearItem)
  }

  // Validate cached pointer — item may have been deleted
  if (g_lastSelectedItem && ValidatePtr2 &&
      !ValidatePtr2(nullptr, (void*)g_lastSelectedItem, "MediaItem*"))
    g_lastSelectedItem = nullptr;

  if (item != g_lastSelectedItem || count != g_lastSelectedCount) {
    g_lastSelectedItem = item;
    g_lastSelectedCount = count;
    g_sneakPeak->LoadSelectedItem();
  }
}

static bool hookCommandProc(int command, int flag)
{
  if (command == g_cmdToggle) {
    DBG("[SneakPeak] hookCommandProc: toggle action fired, visible=%d\n",
        (g_sneakPeak && g_sneakPeak->IsVisible()) ? 1 : 0);
    if (!g_sneakPeak) g_sneakPeak = std::make_unique<SneakPeak>();
    if (!g_sneakPeak->GetHwnd()) {
      g_sneakPeak->Create();
      g_sneakPeak->LoadSelectedItem();
    } else if (!g_sneakPeak->IsVisible()) {
      // Window exists but hidden (e.g., docker closed) - recreate
      g_sneakPeak->Destroy();
      g_sneakPeak->Create();
      g_sneakPeak->LoadSelectedItem();
    } else {
      g_sneakPeak->Toggle();
    }
    return true;
  }
  if (command == g_cmdLoadItem) {
    if (g_sneakPeak && g_sneakPeak->GetHwnd()) g_sneakPeak->LoadSelectedItem();
    return true;
  }
  if (command == g_cmdTrackView) {
    if (g_sneakPeak && g_sneakPeak->GetHwnd()) g_sneakPeak->ToggleTrackView();
    return true;
  }
  if (command == g_cmdMasterView) {
    if (g_sneakPeak && g_sneakPeak->GetHwnd()) g_sneakPeak->ToggleMasterView();
    return true;
  }
  for (int i = 0; i < kToolbarActionCount; ++i) {
    if (command && command == g_cmdToolbar[i]) {
      // Only act on a live, visible window: the commands target the loaded audio
      // (Normalize/Reverse must never edit invisibly).
      if (g_sneakPeak && g_sneakPeak->GetHwnd() && g_sneakPeak->IsVisible())
        g_sneakPeak->RunToolbarCommand(kToolbarActions[i].button);
      return true;
    }
  }
  return false;
}

static int toggleActionCallback(int command)
{
  if (command == g_cmdToggle) {
    if (!g_sneakPeak || !g_sneakPeak->GetHwnd()) return 0;
    if (g_sneakPeak->IsPendingClose()) return 0;
    return g_sneakPeak->IsVisible() ? 1 : 0;
  }
  return -1;
}

static void onAtExit()
{
  DBG("[SneakPeak] onAtExit\n");
  Theme_DestroyFonts();
  if (g_sneakPeak) {
    if (g_SetExtState) {
      g_SetExtState("SneakPeak", "was_visible",
                     g_sneakPeak->IsVisible() ? "1" : "0", true);
    }
    g_sneakPeak->Destroy();
    g_sneakPeak.reset();
  }
}

static int g_startupCounter = 0;
static const int STARTUP_DELAY_TICKS = 15;

static void startupTimerFunc()
{
  if (++g_startupCounter < STARTUP_DELAY_TICKS) return;
  g_plugin_register("-timer", (void*)(void(*)())startupTimerFunc);

  bool wasVisible = false;
  if (g_GetExtState) {
    const char* vis = g_GetExtState("SneakPeak", "was_visible");
    if (vis && vis[0] == '1') wasVisible = true;
  }

  if (wasVisible) {
    if (!g_sneakPeak) g_sneakPeak = std::make_unique<SneakPeak>();
    if (!g_sneakPeak->GetHwnd()) {
      g_sneakPeak->Create();
      g_sneakPeak->LoadSelectedItem();
    }
  }

  // Start selection polling
  g_plugin_register("timer", (void*)(void(*)())pollSelectionTimer);
}

extern "C" {

REAPER_PLUGIN_DLL_EXPORT int ReaperPluginEntry(
  HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
  if (!rec) {
    if (g_sneakPeak) {
      g_sneakPeak->Destroy();
      g_sneakPeak.reset();
    }
    return 0;
  }

  if (rec->caller_version < 0x20E) return 0;

  REAPERAPI_LoadAPI(rec->GetFunc);

  g_reaperMainHwnd = rec->hwnd_main;
  g_plugin_register = rec->Register;

  g_DockWindowAddEx = DockWindowAddEx;
  g_DockWindowRemove = DockWindowRemove;
  g_Main_OnCommand = Main_OnCommand;
  g_GetExtState = GetExtState;
  g_SetExtState = SetExtState;

  g_CountSelectedMediaItems = CountSelectedMediaItems;
  g_GetSelectedMediaItem = GetSelectedMediaItem;
  g_SetMediaItemSelected = SetMediaItemSelected;
  g_GetActiveTake = GetActiveTake;
  g_GetMediaItemTake_Source = GetMediaItemTake_Source;
  g_GetMediaItemInfo_Value = GetMediaItemInfo_Value;
  g_GetSetMediaItemTakeInfo = GetSetMediaItemTakeInfo;
  g_GetSetMediaItemTakeInfo_String = GetSetMediaItemTakeInfo_String;


  g_CreateTakeAudioAccessor = CreateTakeAudioAccessor;
  g_DestroyAudioAccessor = DestroyAudioAccessor;
  g_GetAudioAccessorSamples = GetAudioAccessorSamples;
  g_AudioAccessorStateChanged = (bool(*)(AudioAccessor*))rec->GetFunc("AudioAccessorStateChanged");
  g_AudioAccessorValidateState = (bool(*)(AudioAccessor*))rec->GetFunc("AudioAccessorValidateState");

  // #83: shortcut introspection for keyMatchesOurBinding (optional - fail-open)
  g_SectionFromUniqueID = (KbdSectionInfo*(*)(int))rec->GetFunc("SectionFromUniqueID");
  g_CountActionShortcuts = (int(*)(KbdSectionInfo*, int))rec->GetFunc("CountActionShortcuts");
  g_GetActionShortcutDesc = (bool(*)(KbdSectionInfo*, int, int, char*, int))rec->GetFunc("GetActionShortcutDesc");

  g_GetPlayState = GetPlayState;
  g_GetPlayPosition = GetPlayPosition;
  g_GetPlayPosition2 = GetPlayPosition2;
  g_GetCursorPosition = GetCursorPosition;
  g_SetEditCurPos = SetEditCurPos;
  g_OnPlayButton = OnPlayButton;
  g_OnStopButton = OnStopButton;
  g_PlayPreview = PlayPreview;
  g_StopPreview = StopPreview;
  g_StartPreviewFade = (int(*)(ReaProject*, preview_register_t*, double, int))
      rec->GetFunc("StartPreviewFade");

  g_EnumProjectMarkers3 = EnumProjectMarkers3;
  g_AddProjectMarker2 = AddProjectMarker2;
  g_DeleteProjectMarkerByIndex = DeleteProjectMarkerByIndex;
  g_SetProjectMarkerByIndex2 = SetProjectMarkerByIndex2;

  g_SplitMediaItem = SplitMediaItem;
  g_DeleteTrackMediaItem = DeleteTrackMediaItem;
  g_GetMediaItem_Track = GetMediaItem_Track;
  g_AddMediaItemToTrack = AddMediaItemToTrack;
  g_AddTakeToMediaItem = AddTakeToMediaItem;
  g_UpdateItemInProject = UpdateItemInProject;

  g_GetTrackNumMediaItems = GetTrackNumMediaItems;
  g_GetTrackMediaItem = GetTrackMediaItem;
  g_CountTracks = CountTracks;
  g_GetTrack = GetTrack;
  g_GetMediaItemNumTakes = GetMediaItemNumTakes;
  g_GetMediaItemTake = GetMediaItemTake;
  g_GetSelectedTrack = GetSelectedTrack;
  g_CountSelectedTracks = CountSelectedTracks;

  g_GetSetMediaTrackInfo = GetSetMediaTrackInfo;

  g_PCM_Source_CreateFromFile = PCM_Source_CreateFromFile;
  g_SetMediaItemTake_Source = SetMediaItemTake_Source;
  g_GetMediaSourceFileName = GetMediaSourceFileName;
  g_SetMediaItemInfo_Value = SetMediaItemInfo_Value;
  g_GetSetMediaItemInfo_String = GetSetMediaItemInfo_String;

  g_Undo_BeginBlock2 = Undo_BeginBlock2;
  g_Undo_EndBlock2 = Undo_EndBlock2;

  g_UpdateArrange = UpdateArrange;
  g_UpdateTimeline = UpdateTimeline;
  g_PreventUIRefresh = PreventUIRefresh;

  g_GetSet_LoopTimeRange2 = GetSet_LoopTimeRange2;

  g_TimeMap2_timeToBeats = TimeMap2_timeToBeats;
  g_TimeMap2_beatsToTime = TimeMap2_beatsToTime;
  g_TimeMap_GetTimeSigAtTime = TimeMap_GetTimeSigAtTime;
  g_TimeMap_GetMeasureInfo = TimeMap_GetMeasureInfo;

  g_GetUserInputs = GetUserInputs;
  g_GetMasterTrack = GetMasterTrack;
  g_Track_GetPeakInfo = Track_GetPeakInfo;
  g_Track_GetPeakHoldDB = Track_GetPeakHoldDB;
  g_GetProjectPathEx = GetProjectPathEx;
  g_EnumProjects = EnumProjects;
  g_ValidatePtr2 = ValidatePtr2;
  g_CalculateNormalization = (double(*)(PCM_source*, int, double, double, double))
      rec->GetFunc("CalculateNormalization");

  // Envelope API
  g_GetTakeEnvelopeByName = GetTakeEnvelopeByName;
  g_CountEnvelopePoints = CountEnvelopePoints;
  g_GetEnvelopePoint = GetEnvelopePoint;
  g_GetEnvelopeScalingMode = GetEnvelopeScalingMode;
  g_ScaleFromEnvelopeMode = ScaleFromEnvelopeMode;
  g_Envelope_Evaluate = Envelope_Evaluate;
  g_SetEnvelopePoint = SetEnvelopePoint;
  g_InsertEnvelopePointEx = InsertEnvelopePointEx;
  g_DeleteEnvelopePointEx = DeleteEnvelopePointEx;
  g_DeleteEnvelopePointRange = DeleteEnvelopePointRange;
  g_Envelope_SortPoints = Envelope_SortPoints;
  g_GetEnvelopePointByTime = GetEnvelopePointByTime;
  g_ScaleToEnvelopeMode = ScaleToEnvelopeMode;
  g_GetEnvelopeStateChunk = GetEnvelopeStateChunk;
  g_SetEnvelopeStateChunk = SetEnvelopeStateChunk;
  g_GetSetEnvelopeInfo_String = GetSetEnvelopeInfo_String;

  // Seed the global UI scale from ExtState BEFORE creating fonts so the initial
  // fonts are built at the persisted scale (no flash-then-resize). Validated parse
  // (locale-safe int x1000); ignore anything out of [800,2000]. If unset, the
  // DPI-auto first-run seed happens later in SneakPeak::Create() (needs an HWND).
  if (g_GetExtState) {
    const char* us = g_GetExtState("SneakPeak", "ui_scale");
    if (us && us[0]) {
      char* e = nullptr;
      long v = strtol(us, &e, 10);
      if (e != us && v >= 800 && v <= 2000) g_uiScale = v / 1000.0;
    }
  }

  // Theme colors
  Theme_SetGetThemeColor((void*)GetThemeColor);
  Theme_Init();
  Theme_CreateFonts();

  // Register actions
  g_cmdToggle = rec->Register("command_id", (void*)"SneakPeak_Toggle");
  if (!g_cmdToggle) return 0;

  g_cmdLoadItem = rec->Register("command_id", (void*)"SneakPeak_LoadSelectedItem");
  g_cmdTrackView = rec->Register("command_id", (void*)"SneakPeak_ToggleTrackView");
  g_cmdMasterView = rec->Register("command_id", (void*)"SneakPeak_ToggleMasterView");

  static gaccel_register_t accelToggle = {{0, 0, 0}, "SneakPeak: Open/Close SneakPeak"};
  accelToggle.accel.cmd = static_cast<unsigned short>(g_cmdToggle);
  rec->Register("gaccel", &accelToggle);

  static gaccel_register_t accelLoad = {{0, 0, 0}, "SneakPeak: Load Selected Item"};
  accelLoad.accel.cmd = static_cast<unsigned short>(g_cmdLoadItem);
  rec->Register("gaccel", &accelLoad);

  static gaccel_register_t accelTrackView = {{0, 0, 0}, "SneakPeak: Toggle Track View"};
  accelTrackView.accel.cmd = static_cast<unsigned short>(g_cmdTrackView);
  rec->Register("gaccel", &accelTrackView);

  // #63 (X-Raym): a bindable action for the MASTER output view (the mode-bar tab).
  static gaccel_register_t accelMasterView = {{0, 0, 0}, "SneakPeak: Toggle Master Track View"};
  accelMasterView.accel.cmd = static_cast<unsigned short>(g_cmdMasterView);
  rec->Register("gaccel", &accelMasterView);

  // Toolbar commands as named actions (forum #51): bindable in the Action List.
  static gaccel_register_t accelToolbar[kToolbarActionCount] = {};
  for (int i = 0; i < kToolbarActionCount; ++i) {
    g_cmdToolbar[i] = rec->Register("command_id", (void*)kToolbarActions[i].id);
    if (!g_cmdToolbar[i]) continue;   // fail-open: the toolbar itself still works
    accelToolbar[i].accel.cmd = static_cast<unsigned short>(g_cmdToolbar[i]);
    accelToolbar[i].desc = kToolbarActions[i].desc;
    rec->Register("gaccel", &accelToolbar[i]);
  }

  rec->Register("hookcommand", (void*)hookCommandProc);
  rec->Register("toggleaction", (void*)toggleActionCallback);
  rec->Register("<accelerator", &g_accelReg);
  rec->Register("atexit", (void*)onAtExit);

  g_plugin_register("timer", (void*)(void(*)())startupTimerFunc);

  DBG("[SneakPeak] Plugin loaded successfully\n");
  return 1;
}

} // extern "C"
