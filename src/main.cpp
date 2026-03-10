// EditView — REAPER Extension for Item Waveform Editing
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
#define REAPERAPI_WANT_GetMediaItemTake_Peaks

// Audio accessor
#define REAPERAPI_WANT_CreateTakeAudioAccessor
#define REAPERAPI_WANT_DestroyAudioAccessor
#define REAPERAPI_WANT_GetAudioAccessorSamples

// Transport
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_GetPlayPosition2
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_OnPlayButton
#define REAPERAPI_WANT_OnStopButton

// Markers
#define REAPERAPI_WANT_CountProjectMarkers
#define REAPERAPI_WANT_EnumProjectMarkers3
#define REAPERAPI_WANT_AddProjectMarker2
#define REAPERAPI_WANT_DeleteProjectMarkerByIndex
#define REAPERAPI_WANT_SetProjectMarkerByIndex2

// Source / destructive editing
#define REAPERAPI_WANT_PCM_Source_CreateFromFile
#define REAPERAPI_WANT_PCM_Source_Destroy
#define REAPERAPI_WANT_SetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceFileName
#define REAPERAPI_WANT_SetMediaItemInfo_Value

// Undo
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2

// UI refresh
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_UpdateTimeline

// Item manipulation
#define REAPERAPI_WANT_SplitMediaItem
#define REAPERAPI_WANT_DeleteTrackMediaItem
#define REAPERAPI_WANT_GetMediaItemTrack
#define REAPERAPI_WANT_GetMediaItem_Track

// Time selection
#define REAPERAPI_WANT_GetSet_LoopTimeRange2

// UI / dialogs
#define REAPERAPI_WANT_GetUserInputs

// Theme
#define REAPERAPI_WANT_GetThemeColor

// LICE (bitmap rendering)
#define REAPERAPI_WANT_LICE_CreateBitmap
#define REAPERAPI_WANT_LICE__Destroy
#define REAPERAPI_WANT_LICE__GetBits
#define REAPERAPI_WANT_LICE__GetRowSpan
#define REAPERAPI_WANT_LICE__resize
#define REAPERAPI_WANT_LICE__GetDC

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#include "globals.h"
#include "edit_view.h"
#include "theme.h"
#include "debug.h"
#include <memory>

static std::unique_ptr<EditView> g_editView;
static int g_cmdToggle = 0;
static int g_cmdLoadItem = 0;
static MediaItem* g_lastSelectedItem = nullptr;

// Keyboard accelerator for docked window — intercepts Ctrl+Z/X/C/V etc.
static int translateAccelEditView(MSG* msg, accelerator_register_t* ctx)
{
  if (!g_editView || !g_editView->IsVisible() || !g_editView->GetHwnd()) return 0;

  HWND focus = GetFocus();
  HWND ourHwnd = g_editView->GetHwnd();
  if (focus != ourHwnd && GetParent(focus) != ourHwnd) return 0;

  if (msg->message == WM_KEYDOWN) {
    // Forward to our window's WM_KEYDOWN handler
    SendMessage(ourHwnd, WM_KEYDOWN, msg->wParam, msg->lParam);
    return 1; // eat the keystroke
  }

  return 0;
}

static accelerator_register_t g_accelReg = { translateAccelEditView, true, nullptr };

static void pollSelectionTimer()
{
  if (!g_editView || !g_editView->IsVisible()) return;
  if (!g_CountSelectedMediaItems || !g_GetSelectedMediaItem) return;

  int count = g_CountSelectedMediaItems(nullptr);
  MediaItem* item = (count > 0) ? g_GetSelectedMediaItem(nullptr, 0) : nullptr;

  if (item != g_lastSelectedItem) {
    g_lastSelectedItem = item;
    g_editView->LoadSelectedItem();
  }
}

static bool hookCommandProc(int command, int flag)
{
  if (command == g_cmdToggle) {
    if (!g_editView) g_editView = std::make_unique<EditView>();
    if (!g_editView->GetHwnd()) {
      g_editView->Create();
      g_editView->LoadSelectedItem();
    } else {
      g_editView->Toggle();
    }
    return true;
  }
  if (command == g_cmdLoadItem) {
    if (g_editView && g_editView->GetHwnd()) g_editView->LoadSelectedItem();
    return true;
  }
  return false;
}

static int toggleActionCallback(int command)
{
  if (command == g_cmdToggle) {
    return (g_editView && g_editView->IsVisible()) ? 1 : 0;
  }
  return -1;
}

static void onAtExit()
{
  DBG("[EditView] onAtExit\n");
  if (g_editView) {
    if (g_SetExtState) {
      g_SetExtState("EditView", "was_visible",
                     g_editView->IsVisible() ? "1" : "0", true);
    }
    g_editView->Destroy();
    g_editView.reset();
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
    const char* vis = g_GetExtState("EditView", "was_visible");
    if (vis && vis[0] == '1') wasVisible = true;
  }

  if (wasVisible) {
    if (!g_editView) g_editView = std::make_unique<EditView>();
    if (!g_editView->GetHwnd()) {
      g_editView->Create();
      g_editView->LoadSelectedItem();
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
    if (g_editView) {
      g_editView->Destroy();
      g_editView.reset();
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
  g_GetMediaItemTake_Peaks = GetMediaItemTake_Peaks;

  g_CreateTakeAudioAccessor = CreateTakeAudioAccessor;
  g_DestroyAudioAccessor = DestroyAudioAccessor;
  g_GetAudioAccessorSamples = GetAudioAccessorSamples;

  g_GetPlayState = GetPlayState;
  g_GetPlayPosition2 = GetPlayPosition2;
  g_GetCursorPosition = GetCursorPosition;
  g_SetEditCurPos = SetEditCurPos;
  g_OnPlayButton = OnPlayButton;
  g_OnStopButton = OnStopButton;

  g_CountProjectMarkers = CountProjectMarkers;
  g_EnumProjectMarkers3 = EnumProjectMarkers3;
  g_AddProjectMarker2 = AddProjectMarker2;
  g_DeleteProjectMarkerByIndex = DeleteProjectMarkerByIndex;
  g_SetProjectMarkerByIndex2 = SetProjectMarkerByIndex2;

  g_SplitMediaItem = SplitMediaItem;
  g_DeleteTrackMediaItem = DeleteTrackMediaItem;
  g_GetMediaItemTrack = GetMediaItemTrack;
  g_GetMediaItem_Track = GetMediaItem_Track;

  g_PCM_Source_CreateFromFile = PCM_Source_CreateFromFile;
  g_PCM_Source_Destroy = PCM_Source_Destroy;
  g_SetMediaItemTake_Source = SetMediaItemTake_Source;
  g_GetMediaSourceFileName = GetMediaSourceFileName;
  g_SetMediaItemInfo_Value = SetMediaItemInfo_Value;

  g_Undo_BeginBlock2 = Undo_BeginBlock2;
  g_Undo_EndBlock2 = Undo_EndBlock2;

  g_UpdateArrange = UpdateArrange;
  g_UpdateTimeline = UpdateTimeline;

  g_GetSet_LoopTimeRange2 = GetSet_LoopTimeRange2;

  g_GetUserInputs = GetUserInputs;

  // LICE
  g_LICE_CreateBitmap = LICE_CreateBitmap;
  g_LICE__Destroy = LICE__Destroy;
  g_LICE__GetBits = LICE__GetBits;
  g_LICE__GetRowSpan = LICE__GetRowSpan;
  g_LICE__resize = LICE__resize;
  g_LICE__GetDC = LICE__GetDC;

  // Theme colors
  Theme_SetGetThemeColor((void*)GetThemeColor);
  Theme_Init();

  // Register actions
  g_cmdToggle = rec->Register("command_id", (void*)"EditView_Toggle");
  if (!g_cmdToggle) return 0;

  g_cmdLoadItem = rec->Register("command_id", (void*)"EditView_LoadSelectedItem");

  static gaccel_register_t accelToggle = {{0, 0, 0}, "EditView: Open/Close Edit View"};
  accelToggle.accel.cmd = static_cast<unsigned short>(g_cmdToggle);
  rec->Register("gaccel", &accelToggle);

  static gaccel_register_t accelLoad = {{0, 0, 0}, "EditView: Load Selected Item"};
  accelLoad.accel.cmd = static_cast<unsigned short>(g_cmdLoadItem);
  rec->Register("gaccel", &accelLoad);

  rec->Register("hookcommand", (void*)hookCommandProc);
  rec->Register("toggleaction", (void*)toggleActionCallback);
  rec->Register("accelerator", &g_accelReg);
  rec->Register("atexit", (void*)onAtExit);

  g_plugin_register("timer", (void*)(void(*)())startupTimerFunc);

  DBG("[EditView] Plugin loaded successfully\n");
  return 1;
}

} // extern "C"
