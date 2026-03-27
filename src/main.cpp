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

// Track items (for track follow)
#define REAPERAPI_WANT_GetTrackNumMediaItems
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_CountSelectedTracks

// Track properties (solo)
#define REAPERAPI_WANT_GetSetMediaTrackInfo

// Time selection
#define REAPERAPI_WANT_GetSet_LoopTimeRange2

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
static MediaItem* g_lastSelectedItem = nullptr;
static int g_lastSelectedCount = 0;

// Keyboard accelerator for docked window — intercepts Ctrl+Z/X/C/V etc.
static int translateAccelSneakPeak(MSG* msg, accelerator_register_t* ctx)
{
  if (!g_sneakPeak || !g_sneakPeak->IsVisible() || !g_sneakPeak->GetHwnd()) return 0;

  HWND focus = GetFocus();
  HWND ourHwnd = g_sneakPeak->GetHwnd();
  if (focus != ourHwnd && GetParent(focus) != ourHwnd) return 0;

  if (msg->message == WM_KEYDOWN) {
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    WPARAM k = msg->wParam;
    bool handled = false;
    if (k == VK_HOME || k == VK_END || k == VK_SPACE || k == VK_ESCAPE || k == VK_TAB) handled = true;
    else if (k == VK_DELETE || k == VK_BACK) handled = true;
    else if (ctrl && (k == 'C' || k == 'X' || k == 'V' || k == 'Z' || k == 'N' || k == 'A' || k == 'S')) handled = true;
    else if (!ctrl && (k == 'M' || k == 'G' || k == 'E' || k == 'S' || k == 'T')) handled = true;
    if (handled) {
      SendMessage(ourHwnd, WM_KEYDOWN, msg->wParam, msg->lParam);
      return 1;
    }
  }

  return 0;
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

  g_GetTrackNumMediaItems = GetTrackNumMediaItems;
  g_GetTrackMediaItem = GetTrackMediaItem;
  g_GetSelectedTrack = GetSelectedTrack;
  g_CountSelectedTracks = CountSelectedTracks;

  g_GetSetMediaTrackInfo = GetSetMediaTrackInfo;

  g_PCM_Source_CreateFromFile = PCM_Source_CreateFromFile;
  g_SetMediaItemTake_Source = SetMediaItemTake_Source;
  g_GetMediaSourceFileName = GetMediaSourceFileName;
  g_SetMediaItemInfo_Value = SetMediaItemInfo_Value;

  g_Undo_BeginBlock2 = Undo_BeginBlock2;
  g_Undo_EndBlock2 = Undo_EndBlock2;

  g_UpdateArrange = UpdateArrange;
  g_UpdateTimeline = UpdateTimeline;
  g_PreventUIRefresh = PreventUIRefresh;

  g_GetSet_LoopTimeRange2 = GetSet_LoopTimeRange2;

  g_GetUserInputs = GetUserInputs;
  g_GetMasterTrack = GetMasterTrack;
  g_Track_GetPeakInfo = Track_GetPeakInfo;
  g_Track_GetPeakHoldDB = Track_GetPeakHoldDB;
  g_GetProjectPathEx = GetProjectPathEx;
  g_EnumProjects = EnumProjects;
  g_ValidatePtr2 = ValidatePtr2;
  g_CalculateNormalization = (double(*)(PCM_source*, int, double, double, double))
      rec->GetFunc("CalculateNormalization");

  // Theme colors
  Theme_SetGetThemeColor((void*)GetThemeColor);
  Theme_Init();
  Theme_CreateFonts();

  // Register actions
  g_cmdToggle = rec->Register("command_id", (void*)"SneakPeak_Toggle");
  if (!g_cmdToggle) return 0;

  g_cmdLoadItem = rec->Register("command_id", (void*)"SneakPeak_LoadSelectedItem");
  g_cmdTrackView = rec->Register("command_id", (void*)"SneakPeak_ToggleTrackView");

  static gaccel_register_t accelToggle = {{0, 0, 0}, "SneakPeak: Open/Close SneakPeak"};
  accelToggle.accel.cmd = static_cast<unsigned short>(g_cmdToggle);
  rec->Register("gaccel", &accelToggle);

  static gaccel_register_t accelLoad = {{0, 0, 0}, "SneakPeak: Load Selected Item"};
  accelLoad.accel.cmd = static_cast<unsigned short>(g_cmdLoadItem);
  rec->Register("gaccel", &accelLoad);

  static gaccel_register_t accelTrackView = {{0, 0, 0}, "SneakPeak: Toggle Track View"};
  accelTrackView.accel.cmd = static_cast<unsigned short>(g_cmdTrackView);
  rec->Register("gaccel", &accelTrackView);

  rec->Register("hookcommand", (void*)hookCommandProc);
  rec->Register("toggleaction", (void*)toggleActionCallback);
  rec->Register("accelerator", &g_accelReg);
  rec->Register("atexit", (void*)onAtExit);

  g_plugin_register("timer", (void*)(void(*)())startupTimerFunc);

  DBG("[SneakPeak] Plugin loaded successfully\n");
  return 1;
}

} // extern "C"
