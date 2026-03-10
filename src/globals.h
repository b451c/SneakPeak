// globals.h — REAPER API function pointers for EditView
#pragma once

#include "platform.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <climits>

// Forward declarations
class AudioAccessor;
class ReaProject;
class MediaItem;
class MediaItem_Take;
class MediaTrack;
class PCM_source;

// Core API
extern void (*g_DockWindowAddEx)(HWND hwnd, const char* name, const char* identstr, bool allowShow);
extern void (*g_DockWindowRemove)(HWND);
extern void (*g_Main_OnCommand)(int, int);
extern const char* (*g_GetExtState)(const char*, const char*);
extern void (*g_SetExtState)(const char*, const char*, const char*, bool);
extern HWND g_reaperMainHwnd;
extern int (*g_plugin_register)(const char*, void*);

// Media item API
extern int (*g_CountSelectedMediaItems)(ReaProject* proj);
extern MediaItem* (*g_GetSelectedMediaItem)(ReaProject* proj, int selitem);
extern MediaItem_Take* (*g_GetActiveTake)(MediaItem* item);
extern PCM_source* (*g_GetMediaItemTake_Source)(MediaItem_Take* take);
extern double (*g_GetMediaItemInfo_Value)(MediaItem* item, const char* parmname);
extern void* (*g_GetSetMediaItemTakeInfo)(MediaItem_Take* tk, const char* parmname, void* setNewValue);
extern bool (*g_GetSetMediaItemTakeInfo_String)(MediaItem_Take* tk, const char* parmname, char* stringNeedBig, bool setNewValue);
extern int (*g_GetMediaItemTake_Peaks)(MediaItem_Take* take, double peakrate, double starttime, int numchannels, int numsamplesperchannel, int want_extra_type, double* buf);

// Audio accessor API
extern AudioAccessor* (*g_CreateTakeAudioAccessor)(MediaItem_Take* take);
extern void (*g_DestroyAudioAccessor)(AudioAccessor* accessor);
extern int (*g_GetAudioAccessorSamples)(AudioAccessor* accessor, int samplerate, int numchannels, double starttime_sec, int numsamplesperchannel, double* samplebuffer);

// Transport / playback
extern int (*g_GetPlayState)();
extern double (*g_GetPlayPosition2)();
extern double (*g_GetCursorPosition)();
extern void (*g_SetEditCurPos)(double time, bool moveview, bool seekplay);
extern void (*g_OnPlayButton)();
extern void (*g_OnStopButton)();

// Markers
extern int (*g_CountProjectMarkers)(ReaProject* proj, int* num_markersOut, int* num_regionsOut);
extern int (*g_EnumProjectMarkers3)(ReaProject* proj, int idx, bool* isrgnOut, double* posOut, double* rgnendOut, const char** nameOut, int* markrgnindexnumberOut, int* colorOut);
extern int (*g_AddProjectMarker2)(ReaProject* proj, bool isrgn, double pos, double rgnend, const char* name, int wantidx, int color);
extern bool (*g_DeleteProjectMarkerByIndex)(ReaProject* proj, int markrgnidx);
extern bool (*g_SetProjectMarkerByIndex2)(ReaProject* proj, int markrgnidx, bool isrgn, double pos, double rgnend, int IDnumber, const char* name, int color, int flags);

// Item manipulation (non-destructive)
extern MediaItem* (*g_SplitMediaItem)(MediaItem* item, double position);
extern bool (*g_DeleteTrackMediaItem)(MediaTrack* tr, MediaItem* it);
extern MediaTrack* (*g_GetMediaItemTrack)(MediaItem* item);
extern MediaTrack* (*g_GetMediaItem_Track)(MediaItem* item);

// Source / destructive editing
extern PCM_source* (*g_PCM_Source_CreateFromFile)(const char* filename);
extern void (*g_PCM_Source_Destroy)(PCM_source* src);
extern bool (*g_SetMediaItemTake_Source)(MediaItem_Take* take, PCM_source* source);
extern void (*g_GetMediaSourceFileName)(PCM_source* source, char* filenamebuf, int filenamebuf_sz);
extern bool (*g_SetMediaItemInfo_Value)(MediaItem* item, const char* parmname, double newvalue);

// Undo
extern void (*g_Undo_BeginBlock2)(ReaProject* proj);
extern void (*g_Undo_EndBlock2)(ReaProject* proj, const char* descchange, int extraflags);

// UI refresh
extern void (*g_UpdateArrange)();
extern void (*g_UpdateTimeline)();

// Time selection
extern void (*g_GetSet_LoopTimeRange2)(ReaProject* proj, bool isSet, bool isLoop, double* startOut, double* endOut, bool allowautoseek);

// UI / dialogs
extern bool (*g_GetUserInputs)(const char* title, int num_inputs, const char* captions_csv, char* retvals_csv, int retvals_csv_sz);

// LICE bitmap API
class LICE_IBitmap;
extern LICE_IBitmap* (*g_LICE_CreateBitmap)(int mode, int w, int h);
extern void (*g_LICE__Destroy)(LICE_IBitmap* bm);
extern void* (*g_LICE__GetBits)(LICE_IBitmap* bm);
extern int (*g_LICE__GetRowSpan)(LICE_IBitmap* bm);
extern bool (*g_LICE__resize)(LICE_IBitmap* bm, int w, int h);
extern HDC (*g_LICE__GetDC)(LICE_IBitmap* bm);

// Utility
inline void safe_strncpy(char* dst, const char* src, size_t dst_size) {
  if (!dst || dst_size == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = '\0';
}
