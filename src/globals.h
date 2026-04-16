// globals.h — REAPER API function pointers for SneakPeak
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
class TrackEnvelope;

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


// Audio accessor API
extern AudioAccessor* (*g_CreateTakeAudioAccessor)(MediaItem_Take* take);
extern void (*g_DestroyAudioAccessor)(AudioAccessor* accessor);
extern int (*g_GetAudioAccessorSamples)(AudioAccessor* accessor, int samplerate, int numchannels, double starttime_sec, int numsamplesperchannel, double* samplebuffer);
extern bool (*g_AudioAccessorStateChanged)(AudioAccessor*);
extern bool (*g_AudioAccessorValidateState)(AudioAccessor*);

// Transport / playback
extern int (*g_GetPlayState)();
extern double (*g_GetPlayPosition)();
extern double (*g_GetPlayPosition2)();
extern double (*g_GetCursorPosition)();
extern void (*g_SetEditCurPos)(double time, bool moveview, bool seekplay);
extern void (*g_OnPlayButton)();
extern void (*g_OnStopButton)();

// Preview playback (standalone mode)
#ifndef _REAPER_PLUGIN_H_
struct _REAPER_preview_register_t;
typedef struct _REAPER_preview_register_t preview_register_t;
#endif
extern int (*g_PlayPreview)(preview_register_t* preview);
extern int (*g_StopPreview)(preview_register_t* preview);
extern int (*g_StartPreviewFade)(ReaProject*, preview_register_t*, double, int);

// Markers
extern int (*g_EnumProjectMarkers3)(ReaProject* proj, int idx, bool* isrgnOut, double* posOut, double* rgnendOut, const char** nameOut, int* markrgnindexnumberOut, int* colorOut);
extern int (*g_AddProjectMarker2)(ReaProject* proj, bool isrgn, double pos, double rgnend, const char* name, int wantidx, int color);
extern bool (*g_DeleteProjectMarkerByIndex)(ReaProject* proj, int markrgnidx);
extern bool (*g_SetProjectMarkerByIndex2)(ReaProject* proj, int markrgnidx, bool isrgn, double pos, double rgnend, int IDnumber, const char* name, int color, int flags);

// Item manipulation (non-destructive)
extern MediaItem* (*g_SplitMediaItem)(MediaItem* item, double position);
extern bool (*g_DeleteTrackMediaItem)(MediaTrack* tr, MediaItem* it);
extern MediaTrack* (*g_GetMediaItem_Track)(MediaItem* item);
extern MediaItem* (*g_AddMediaItemToTrack)(MediaTrack* tr);
extern MediaItem_Take* (*g_AddTakeToMediaItem)(MediaItem* item);
extern void (*g_UpdateItemInProject)(MediaItem* item);

// Track items (for track follow)
extern int (*g_GetTrackNumMediaItems)(MediaTrack* tr);
extern MediaItem* (*g_GetTrackMediaItem)(MediaTrack* tr, int itemidx);
extern MediaTrack* (*g_GetSelectedTrack)(ReaProject* proj, int seltrackidx);
extern int (*g_CountSelectedTracks)(ReaProject* proj);

// Track properties
extern void* (*g_GetSetMediaTrackInfo)(MediaTrack* tr, const char* parmname, void* setNewValue);

// Source / destructive editing
extern PCM_source* (*g_PCM_Source_CreateFromFile)(const char* filename);
extern bool (*g_SetMediaItemTake_Source)(MediaItem_Take* take, PCM_source* source);
extern void (*g_GetMediaSourceFileName)(PCM_source* source, char* filenamebuf, int filenamebuf_sz);
extern bool (*g_SetMediaItemInfo_Value)(MediaItem* item, const char* parmname, double newvalue);

// Undo
extern void (*g_Undo_BeginBlock2)(ReaProject* proj);
extern void (*g_Undo_EndBlock2)(ReaProject* proj, const char* descchange, int extraflags);

// UI refresh
extern void (*g_UpdateArrange)();
extern void (*g_PreventUIRefresh)(int prevent_count);
extern void (*g_UpdateTimeline)();

// Time selection
extern void (*g_GetSet_LoopTimeRange2)(ReaProject* proj, bool isSet, bool isLoop, double* startOut, double* endOut, bool allowautoseek);

// Master track metering
extern MediaTrack* (*g_GetMasterTrack)(ReaProject* proj);
extern double (*g_Track_GetPeakInfo)(MediaTrack* track, int channel);
extern double (*g_Track_GetPeakHoldDB)(MediaTrack* track, int channel, bool clear);

// Project path
extern void (*g_GetProjectPathEx)(ReaProject* proj, char* bufOut, int bufOut_sz);
extern ReaProject* (*g_EnumProjects)(int idx, char* projfnOutOptional, int projfnOutOptional_sz);

// Tempo map (bars/beats ruler)
extern double (*g_TimeMap2_timeToBeats)(ReaProject* proj, double tpos, int* measuresOut, int* cmlOut, double* fullbeatsOut, int* cdenomOut);
extern double (*g_TimeMap2_beatsToTime)(ReaProject* proj, double tpos, const int* measuresIn);
extern void (*g_TimeMap_GetTimeSigAtTime)(ReaProject* proj, double time, int* timesig_numOut, int* timesig_denomOut, double* tempoOut);
extern double (*g_TimeMap_GetMeasureInfo)(ReaProject* proj, int measure, double* qn_startOut, double* qn_endOut, int* timesig_numOut, int* timesig_denomOut, double* tempoOut);

// Pointer validation
extern bool (*g_ValidatePtr2)(ReaProject* proj, void* pointer, const char* ctypename);

// Envelope API
extern TrackEnvelope* (*g_GetTakeEnvelopeByName)(MediaItem_Take* take, const char* envname);
extern int (*g_CountEnvelopePoints)(TrackEnvelope* envelope);
extern bool (*g_GetEnvelopePoint)(TrackEnvelope* envelope, int ptidx, double* timeOut, double* valueOut, int* shapeOut, double* tensionOut, bool* selectedOut);
extern int (*g_GetEnvelopeScalingMode)(TrackEnvelope* env);
extern double (*g_ScaleFromEnvelopeMode)(int scaling_mode, double val);
extern int (*g_Envelope_Evaluate)(TrackEnvelope* envelope, double time, double samplerate, int samplesRequested, double* valueOut, double* dVdSOut, double* ddVdSOut, double* dddVdSOut);
extern bool (*g_SetEnvelopePoint)(TrackEnvelope* envelope, int ptidx, double* timeInOptional, double* valueInOptional, int* shapeInOptional, double* tensionInOptional, bool* selectedInOptional, bool* noSortInOptional);
extern bool (*g_InsertEnvelopePointEx)(TrackEnvelope* envelope, int autoitem_idx, double time, double value, int shape, double tension, bool selected, bool* noSortInOptional);
extern bool (*g_DeleteEnvelopePointEx)(TrackEnvelope* envelope, int autoitem_idx, int ptidx);
extern bool (*g_DeleteEnvelopePointRange)(TrackEnvelope* envelope, double time_start, double time_end);
extern bool (*g_Envelope_SortPoints)(TrackEnvelope* envelope);
extern int (*g_GetEnvelopePointByTime)(TrackEnvelope* envelope, double time);
extern double (*g_ScaleToEnvelopeMode)(int scaling_mode, double val);
extern bool (*g_GetEnvelopeStateChunk)(TrackEnvelope* env, char* strNeedBig, int strNeedBig_sz, bool isundo);

// UI / dialogs
extern bool (*g_GetUserInputs)(const char* title, int num_inputs, const char* captions_csv, char* retvals_csv, int retvals_csv_sz);

// Normalization
extern double (*g_CalculateNormalization)(PCM_source*, int, double, double, double);

// Utility
inline void safe_strncpy(char* dst, const char* src, size_t dst_size) {
  if (!dst || dst_size == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = '\0';
}
