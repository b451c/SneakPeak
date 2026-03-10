// globals.cpp — REAPER API function pointer definitions
#include "globals.h"

// Core API
void (*g_DockWindowAddEx)(HWND, const char*, const char*, bool) = nullptr;
void (*g_DockWindowRemove)(HWND) = nullptr;
void (*g_Main_OnCommand)(int, int) = nullptr;
const char* (*g_GetExtState)(const char*, const char*) = nullptr;
void (*g_SetExtState)(const char*, const char*, const char*, bool) = nullptr;
HWND g_reaperMainHwnd = nullptr;
int (*g_plugin_register)(const char*, void*) = nullptr;

// Media item API
int (*g_CountSelectedMediaItems)(ReaProject*) = nullptr;
MediaItem* (*g_GetSelectedMediaItem)(ReaProject*, int) = nullptr;
MediaItem_Take* (*g_GetActiveTake)(MediaItem*) = nullptr;
PCM_source* (*g_GetMediaItemTake_Source)(MediaItem_Take*) = nullptr;
double (*g_GetMediaItemInfo_Value)(MediaItem*, const char*) = nullptr;
void* (*g_GetSetMediaItemTakeInfo)(MediaItem_Take*, const char*, void*) = nullptr;
bool (*g_GetSetMediaItemTakeInfo_String)(MediaItem_Take*, const char*, char*, bool) = nullptr;


// Audio accessor API
AudioAccessor* (*g_CreateTakeAudioAccessor)(MediaItem_Take*) = nullptr;
void (*g_DestroyAudioAccessor)(AudioAccessor*) = nullptr;
int (*g_GetAudioAccessorSamples)(AudioAccessor*, int, int, double, int, double*) = nullptr;

// Transport
int (*g_GetPlayState)() = nullptr;
double (*g_GetPlayPosition2)() = nullptr;
double (*g_GetCursorPosition)() = nullptr;
void (*g_SetEditCurPos)(double, bool, bool) = nullptr;
void (*g_OnPlayButton)() = nullptr;
void (*g_OnStopButton)() = nullptr;

// Markers
int (*g_EnumProjectMarkers3)(ReaProject*, int, bool*, double*, double*, const char**, int*, int*) = nullptr;
int (*g_AddProjectMarker2)(ReaProject*, bool, double, double, const char*, int, int) = nullptr;
bool (*g_DeleteProjectMarkerByIndex)(ReaProject*, int) = nullptr;
bool (*g_SetProjectMarkerByIndex2)(ReaProject*, int, bool, double, double, int, const char*, int, int) = nullptr;

// Item manipulation (non-destructive)
MediaItem* (*g_SplitMediaItem)(MediaItem*, double) = nullptr;
bool (*g_DeleteTrackMediaItem)(MediaTrack*, MediaItem*) = nullptr;
MediaTrack* (*g_GetMediaItem_Track)(MediaItem*) = nullptr;

// Source / destructive editing
PCM_source* (*g_PCM_Source_CreateFromFile)(const char*) = nullptr;
bool (*g_SetMediaItemTake_Source)(MediaItem_Take*, PCM_source*) = nullptr;
void (*g_GetMediaSourceFileName)(PCM_source*, char*, int) = nullptr;
bool (*g_SetMediaItemInfo_Value)(MediaItem*, const char*, double) = nullptr;

// Undo
void (*g_Undo_BeginBlock2)(ReaProject*) = nullptr;
void (*g_Undo_EndBlock2)(ReaProject*, const char*, int) = nullptr;

// UI / dialogs
bool (*g_GetUserInputs)(const char*, int, const char*, char*, int) = nullptr;

// UI refresh
void (*g_UpdateArrange)() = nullptr;
void (*g_UpdateTimeline)() = nullptr;

// Time selection
void (*g_GetSet_LoopTimeRange2)(ReaProject*, bool, bool, double*, double*, bool) = nullptr;
