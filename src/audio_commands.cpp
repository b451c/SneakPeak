// ============================================================================
// audio_commands.cpp — Clipboard, editing, undo, and audio processing for SneakPeak
//
// Cut/copy/paste, delete, normalize, fade, reverse, gain, DC remove,
// LUFS normalization, undo system, selection helpers, marker navigation.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "audio_ops.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <algorithm>


// --- Selection sample range helper ---

void SneakPeak::GetSelectionSampleRange(int& startFrame, int& endFrame) const
{
  if (!m_waveform.HasSelection()) {
    startFrame = 0;
    endFrame = m_waveform.GetAudioSampleCount();
    return;
  }
  WaveformSelection sel = m_waveform.GetSelection();
  double sr = (double)m_waveform.GetSampleRate();
  startFrame = std::max(0, (int)(sel.startTime * sr));
  endFrame = std::min(m_waveform.GetAudioSampleCount(), (int)(sel.endTime * sr));
  if (endFrame <= startFrame) {
    startFrame = 0;
    endFrame = m_waveform.GetAudioSampleCount();
  }
}


// --- Undo ---

void SneakPeak::UndoSave()
{
  m_hasUndo = true;
}

void SneakPeak::UndoRestore()
{
  if (m_waveform.IsStandaloneMode()) {
    StandaloneUndoRestore();
    return;
  }
  // Trigger REAPER's native undo
  // Action 40029 = Edit: Undo
  if (g_Main_OnCommand) {
    g_Main_OnCommand(40029, 0);
    // Reload to reflect undo changes
    if (m_workingSet.active) {
      RefreshWorkingSet();
    } else if (m_waveform.IsTimelineView()) {
      m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
      RefreshTimelineView();
    } else {
      m_waveform.ClearItem();
      LoadSelectedItem();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::StandaloneUndoSave()
{
  const auto& data = m_waveform.GetAudioData();
  if (data.empty()) return;
  if ((int)m_standaloneUndoStack.size() >= MAX_STANDALONE_UNDO)
    m_standaloneUndoStack.erase(m_standaloneUndoStack.begin());
  m_standaloneUndoStack.push_back(data);
  m_hasUndo = true;
  m_previewCacheDirty = true;
}

void SneakPeak::StandaloneUndoRestore()
{
  if (m_standaloneUndoStack.empty()) return;
  m_waveform.GetAudioData() = std::move(m_standaloneUndoStack.back());
  m_standaloneUndoStack.pop_back();

  // Recalculate duration from restored data
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int newFrames = (nch > 0) ? (int)m_waveform.GetAudioData().size() / nch : 0;
  double newDur = (sr > 0) ? (double)newFrames / (double)sr : 0.0;
  m_waveform.SetAudioSampleCount(newFrames);
  m_waveform.SetItemDuration(newDur);

  m_waveform.ClearSelection();
  m_waveform.ClearStandaloneFade(); // clear non-destructive fade on undo
  m_waveform.Invalidate();
  m_hasUndo = !m_standaloneUndoStack.empty();
  m_dirty = true;
  m_previewCacheDirty = true;
  UpdateTitle();
  InvalidateRect(m_hwnd, nullptr, FALSE);
  DBG("[SneakPeak] Standalone undo (stack=%d, frames=%d, dur=%.3f)\n",
      (int)m_standaloneUndoStack.size(), newFrames, newDur);
}

// --- Marker Navigation ---

void SneakPeak::NavigateToMarker(bool forward)
{
  if (!m_waveform.HasItem() || !g_EnumProjectMarkers3 || !g_SetEditCurPos) return;

  double itemPos = m_waveform.GetItemPosition();
  double itemEnd = m_waveform.RelTimeToAbsTime(m_waveform.GetItemDuration());
  double cursorAbs = m_waveform.RelTimeToAbsTime(m_waveform.GetCursorTime());

  double bestTime = -1.0;
  double bestDist = 1e30;

  int idx = 0;
  bool isRgn;
  double pos, rgnEnd;
  const char* name;
  int num;
  while (g_EnumProjectMarkers3(nullptr, idx, &isRgn, &pos, &rgnEnd, &name, &num, nullptr)) {
    idx++;
    // Only consider markers/region starts within item bounds
    if (pos < itemPos || pos > itemEnd) continue;

    if (forward && pos > cursorAbs + 0.0001) {
      double d = pos - cursorAbs;
      if (d < bestDist) { bestDist = d; bestTime = pos; }
    } else if (!forward && pos < cursorAbs - 0.0001) {
      double d = cursorAbs - pos;
      if (d < bestDist) { bestDist = d; bestTime = pos; }
    }
  }

  if (bestTime >= 0.0) {
    g_SetEditCurPos(bestTime, false, false);
    m_waveform.SetCursorTime(m_waveform.AbsTimeToRelTime(bestTime));
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

// --- Loop Playback ---

void SneakPeak::DoLoopSelection()
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (!g_SetEditCurPos || !g_OnPlayButton) return;

  WaveformSelection sel = m_waveform.GetSelection();
  double s = m_waveform.RelTimeToAbsTime(std::min(sel.startTime, sel.endTime));

  // Play from selection start
  g_SetEditCurPos(s, false, false);
  m_startedPlayback = true;
  m_playGraceTicks = PLAY_GRACE_TICKS;
  g_OnPlayButton();
}

// --- Write back to disk and refresh ---

void SneakPeak::WriteAndRefresh()
{
  if (!m_waveform.HasItem() || !m_waveform.GetTake()) return;

  std::string path = AudioEngine::GetSourceFilePath(m_waveform.GetTake());
  if (path.empty()) return;

  // Block non-WAV destructive editing
  {
    std::string ext;
    auto dotPos = path.find_last_of('.');
    if (dotPos != std::string::npos)
      ext = path.substr(dotPos + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext != "wav" && ext != "wave") {
      MessageBox(m_hwnd, "Destructive editing only supports WAV files.\nConvert source to WAV first.",
                 "SneakPeak", MB_OK | MB_ICONWARNING);
      return;
    }
  }

  const auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();

  if (!AudioEngine::WriteWavFile(path, data.data(), frames, nch, sr,
                                 m_wavBitsPerSample, m_wavAudioFormat)) {
    MessageBox(m_hwnd, "Failed to write WAV file.", "SneakPeak", MB_OK | MB_ICONERROR);
    return;
  }
  AudioEngine::RefreshItemSource(m_waveform.GetItem(), m_waveform.GetTake());

  m_waveform.Invalidate();
  m_dirty = true;
  UpdateTitle();
}

// --- Sync SneakPeak selection to REAPER time selection ---

void SneakPeak::SyncSelectionToReaper()
{
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsStandaloneMode()) return; // no REAPER time selection in standalone
  if (!g_GetSet_LoopTimeRange2) return;
  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    double dur = m_waveform.GetItemDuration();
    double selMin = std::max(0.0, std::min(sel.startTime, sel.endTime));
    double selMax = std::min(dur, std::max(sel.startTime, sel.endTime));
    double s = m_waveform.RelTimeToAbsTime(selMin);
    double e = m_waveform.RelTimeToAbsTime(selMax);
    if (s > e) std::swap(s, e);
    g_GetSet_LoopTimeRange2(nullptr, true, false, &s, &e, false);
  } else {
    // Clear time selection
    double s = 0.0, e = 0.0;
    g_GetSet_LoopTimeRange2(nullptr, true, false, &s, &e, false);
  }
  if (g_UpdateTimeline) g_UpdateTimeline();
}

// --- Clipboard operations ---

void SneakPeak::DoCopy()
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;

  // Sync selection to REAPER so native copy works on the right range
  SyncSelectionToReaper();

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  int nch = m_waveform.GetNumChannels();
  int selFrames = endF - startF;
  if (selFrames <= 0) return;

  // Internal clipboard - fill samples first, set numFrames last
  if (m_waveform.IsMultiItemActive()) {
    // Multi-item view only: mix all layers in selected range
    m_waveform.GetMultiItemView().GetMixedAudio(startF, endF, nch, s_clipboard.samples);
  } else {
    // Single-item / timeline / SET: copy from m_audioData
    const auto& data = m_waveform.GetAudioData();
    size_t srcOffset = (size_t)startF * nch;
    size_t copyLen = (size_t)selFrames * nch;
    if (srcOffset + copyLen > data.size()) return;
    s_clipboard.samples.resize(copyLen);
    std::copy(data.begin() + (long)srcOffset,
              data.begin() + (long)(srcOffset + copyLen),
              s_clipboard.samples.begin());
  }
  s_clipboard.numChannels = nch;
  s_clipboard.sampleRate = m_waveform.GetSampleRate();
  s_clipboard.numFrames = selFrames;

  // Also trigger REAPER's native copy (40060 = Copy selected area of items)
  if (g_Main_OnCommand) g_Main_OnCommand(40060, 0);

  DBG("[SneakPeak] Copied %d frames to clipboard\n", selFrames);
}

void SneakPeak::DoCut()
{
  // Copy + ripple delete (standard waveform editor behavior: cut closes the gap)
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;

  DoCopy();
  DoDelete(true); // ripple
}

void SneakPeak::DoPaste()
{
  if (!m_waveform.HasItem() || s_clipboard.numFrames <= 0) return;
  // Standalone mode: destructive paste (no REAPER track)
  if (m_waveform.IsStandaloneMode()) {
    DoPasteDestructive();
    return;
  }

  // --- Non-destructive insert-paste (all modes) ---
  // 1. Find item under cursor, resolve its track
  // 2. Split that item at cursor
  // 3. Ripple all subsequent items on that track right by clipboard duration
  // 4. Insert new item in the gap
  // 5. Rebuild view

  if (!g_AddMediaItemToTrack || !g_AddTakeToMediaItem || !g_PCM_Source_CreateFromFile ||
      !g_GetMediaItem_Track || !g_GetSetMediaItemTakeInfo || !g_SplitMediaItem ||
      !g_GetTrackNumMediaItems || !g_GetTrackMediaItem) return;

  // Resolve absolute cursor position and track from segments
  // (RelTimeToAbsTime may be wrong in multi-item/concatenated views)
  MediaTrack* track = nullptr;
  double cursorRel = m_waveform.GetCursorTime();
  double absPos = 0.0;
  if (m_workingSet.active) {
    track = m_workingSet.track;
    absPos = m_waveform.RelTimeToAbsTime(cursorRel);
  } else {
    // Find segment containing cursor, compute absPos from segment data
    bool found = false;
    for (const auto& seg : m_waveform.GetSegments()) {
      if (!seg.item) continue;
      if (cursorRel >= seg.relativeOffset - 0.001 &&
          cursorRel <= seg.relativeOffset + seg.duration + 0.001) {
        absPos = seg.position + (cursorRel - seg.relativeOffset);
        track = g_GetMediaItem_Track(seg.item);
        found = true;
        break;
      }
    }
    if (!found) {
      // Cursor outside any segment - use RelTimeToAbsTime as fallback
      absPos = m_waveform.RelTimeToAbsTime(cursorRel);
      if (m_waveform.GetItem())
        track = g_GetMediaItem_Track(m_waveform.GetItem());
    }
  }
  if (!track) return;

  // Write clipboard to temp WAV
  char tempPath[512];
  snprintf(tempPath, sizeof(tempPath), "/tmp/sneakpeak_paste_%d_%lld.wav",
           (int)getpid(), (long long)time(nullptr));
  if (!AudioEngine::WriteWavFile(tempPath, s_clipboard.samples.data(),
      s_clipboard.numFrames, s_clipboard.numChannels, s_clipboard.sampleRate, 32, 3))
    return;

  double clipDur = (double)s_clipboard.numFrames / (double)s_clipboard.sampleRate;

  // Compute original range of items we're managing (for view rebuild)
  double origStart, origEnd;
  if (m_waveform.IsTimelineOrMultiItem() || m_waveform.IsTrackView()) {
    const auto& segs = m_waveform.GetSegments();
    origStart = segs.front().position;
    origEnd = segs.back().position + segs.back().duration;
  } else {
    origStart = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_POSITION");
    origEnd = origStart + g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_LENGTH");
  }

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  // Find and split the item under cursor (scan track, not just first segment)
  int cnt = g_GetTrackNumMediaItems(track);
  for (int i = 0; i < cnt; i++) {
    MediaItem* mi = g_GetTrackMediaItem(track, i);
    if (!mi) continue;
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
    if (absPos > pos + 0.001 && absPos < end - 0.001) {
      g_SplitMediaItem(mi, absPos);
      break;
    }
  }

  // Ripple: shift all items at or after cursor right by clipboard duration
  cnt = g_GetTrackNumMediaItems(track); // re-count after split
  for (int i = cnt - 1; i >= 0; i--) { // reverse to avoid double-shift
    MediaItem* mi = g_GetTrackMediaItem(track, i);
    if (!mi) continue;
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    if (pos >= absPos - 0.0001)
      g_SetMediaItemInfo_Value(mi, "D_POSITION", pos + clipDur);
  }

  // Create new item in the gap
  MediaItem* newItem = g_AddMediaItemToTrack(track);
  if (newItem) {
    MediaItem_Take* newTake = g_AddTakeToMediaItem(newItem);
    if (newTake) {
      PCM_source* src = g_PCM_Source_CreateFromFile(tempPath);
      if (src) g_GetSetMediaItemTakeInfo(newTake, "P_SOURCE", src);
    }
    g_SetMediaItemInfo_Value(newItem, "D_POSITION", absPos);
    g_SetMediaItemInfo_Value(newItem, "D_LENGTH", clipDur);
  }

  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Paste", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  // Rebuild view: collect all items in expanded range from track
  m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
  double newEnd = origEnd + clipDur;
  std::vector<MediaItem*> rebuilt;
  cnt = g_GetTrackNumMediaItems(track);
  for (int i = 0; i < cnt; i++) {
    MediaItem* mi = g_GetTrackMediaItem(track, i);
    if (!mi) continue;
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
    if (pos >= origStart - 0.001 && end <= newEnd + 0.001)
      rebuilt.push_back(mi);
  }

  // Rebuild appropriate view mode
  if (m_workingSet.active) {
    m_workingSet.items = rebuilt;
    m_workingSet.endPos += clipDur;
    RefreshWorkingSet();
  } else if (m_waveform.IsMultiItemActive()) {
    // Multi-item: reload from REAPER selection (items may have shifted)
    LoadSelectedItem();
  } else if (rebuilt.size() >= 2) {
    m_waveform.ClearItem();
    m_waveform.LoadTimelineView(rebuilt);
    { std::vector<MediaItem*> si;
      for (const auto& s : m_waveform.GetSegments()) if (s.item) si.push_back(s.item);
      if (!si.empty()) m_gainPanel.ShowBatch(si);
    }
  }

  // Select pasted region in SneakPeak
  if (m_waveform.HasItem()) {
    double relPos = m_waveform.AbsTimeToRelTime(absPos);
    m_waveform.StartSelection(relPos);
    m_waveform.UpdateSelection(relPos + clipDur);
    m_waveform.EndSelection();
    m_waveform.SetCursorTime(relPos);
  }

  // Force REAPER to rebuild peaks for new item
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_UpdateTimeline) g_UpdateTimeline();
  // Force REAPER to show waveform in pasted item
  if (newItem && g_UpdateItemInProject) g_UpdateItemInProject(newItem);
  if (g_Main_OnCommand) g_Main_OnCommand(40047, 0); // Peaks: Build any missing peaks
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoPasteDestructive()
{
  if (s_clipboard.numChannels != m_waveform.GetNumChannels()) return;

  int ret = MessageBox(m_hwnd,
    "Paste modifies the audio file on disk. Continue?",
    "SneakPeak - Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  UndoSave();

  int nch = m_waveform.GetNumChannels();
  double cursorTime = m_waveform.GetCursorTime();
  int insertFrame = std::max(0, std::min(m_waveform.GetAudioSampleCount(),
                   (int)(cursorTime * (double)m_waveform.GetSampleRate())));

  auto& data = m_waveform.GetAudioData();
  size_t insertPos = (size_t)insertFrame * nch;
  data.insert(data.begin() + (long)insertPos,
              s_clipboard.samples.begin(), s_clipboard.samples.end());

  int newFrames = m_waveform.GetAudioSampleCount() + s_clipboard.numFrames;
  m_waveform.SetAudioSampleCount(newFrames);
  double newDur = (double)newFrames / (double)m_waveform.GetSampleRate();
  m_waveform.SetItemDuration(newDur);

  if (g_SetMediaItemInfo_Value)
    g_SetMediaItemInfo_Value(m_waveform.GetItem(), "D_LENGTH", newDur);

  WriteAndRefresh();

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Paste", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoDelete(bool ripple)
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (m_waveform.IsStandaloneMode()) { DoDeleteStandalone(); return; }
  DoDeleteNonDestructive(ripple);
}

void SneakPeak::DoDeleteStandalone()
{
  StandaloneUndoSave();
  auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int totalFrames = m_waveform.GetAudioSampleCount();

  WaveformSelection sel = m_waveform.GetSelection();
  int startFrame = (int)(std::min(sel.startTime, sel.endTime) * sr);
  int endFrame = (int)(std::max(sel.startTime, sel.endTime) * sr);
  startFrame = std::max(0, std::min(totalFrames, startFrame));
  endFrame = std::max(0, std::min(totalFrames, endFrame));

  if (endFrame > startFrame) {
    size_t startSample = (size_t)startFrame * nch;
    size_t endSample = (size_t)endFrame * nch;
    data.erase(data.begin() + startSample, data.begin() + endSample);

    int newFrames = (int)data.size() / nch;

    // Short crossfade at the splice point to avoid clicks (~10ms each side)
    int fadeLen = std::min(sr / 100, std::min(startFrame, newFrames - startFrame));
    if (fadeLen > 1) {
      for (int f = 0; f < fadeLen; f++) {
        double t = (double)f / (double)fadeLen;
        double fadeOut = 0.5 * (1.0 + cos(t * M_PI));       // 1→0
        double fadeIn = 0.5 * (1.0 - cos(t * M_PI));        // 0→1
        int leftFrame = startFrame - fadeLen + f;
        int rightFrame = startFrame + f;
        if (leftFrame < 0 || rightFrame >= newFrames) break;
        for (int ch = 0; ch < nch; ch++) {
          size_t li = (size_t)leftFrame * nch + ch;
          size_t ri = (size_t)rightFrame * nch + ch;
          double blended = data[li] * fadeOut + data[ri] * fadeIn;
          data[li] = blended;
        }
      }
      // Remove the right side of the crossfade (now blended into left)
      size_t spliceStart = (size_t)startFrame * nch;
      size_t spliceEnd = (size_t)(startFrame + fadeLen) * nch;
      if (spliceEnd <= data.size())
        data.erase(data.begin() + spliceStart, data.begin() + spliceEnd);
      newFrames = (int)data.size() / nch;
    }
    double newDur = (double)newFrames / (double)sr;
    m_waveform.SetAudioSampleCount(newFrames);
    m_waveform.SetItemDuration(newDur);

    // Clamp view to new duration
    if (m_waveform.GetViewStart() + m_waveform.GetViewDuration() > newDur) {
      double vs = std::max(0.0, newDur - m_waveform.GetViewDuration());
      m_waveform.SetViewStart(vs);
      if (m_waveform.GetViewDuration() > newDur)
        m_waveform.SetViewDuration(newDur);
    }
    // Place cursor at delete point
    double cursorTime = (double)startFrame / (double)sr;
    m_waveform.SetCursorTime(cursorTime);
    m_waveform.ClearSelection();
    m_waveform.Invalidate();
    m_minimap.Invalidate();
    m_dirty = true;
    UpdateTitle();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::DoDeleteNonDestructive(bool ripple)
{
  // Split item at selection edges, delete middle piece
  if (!g_SplitMediaItem || !g_DeleteTrackMediaItem || !g_GetMediaItem_Track) return;

  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);

  double splitStart = m_waveform.RelTimeToAbsTime(selStart);
  double splitEnd = m_waveform.RelTimeToAbsTime(selEnd);
  double deletedDuration = splitEnd - splitStart;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  MediaTrack* track = nullptr;
  std::vector<MediaItem*> survivors; // surviving items after split+delete (for timeline view)

  if (m_waveform.IsTrackView() && m_workingSet.track) {
    // Working set: handle cross-segment selection (may span multiple items)
    track = m_workingSet.track;
    int count = g_GetTrackNumMediaItems(track);

    // Collect items overlapping [splitStart, splitEnd]
    std::vector<MediaItem*> overlap;
    for (int i = 0; i < count; i++) {
      MediaItem* mi = g_GetTrackMediaItem(track, i);
      if (!mi) continue;
      double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
      double len = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
      if (pos + len > splitStart && pos < splitEnd)
        overlap.push_back(mi);
    }

    for (MediaItem* mi : overlap) {
      double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
      double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");

      if (pos >= splitStart && end <= splitEnd) {
        // Entire item inside selection - delete whole item
        g_DeleteTrackMediaItem(track, mi);
      } else if (pos < splitStart && end > splitEnd) {
        // Selection inside one item - split both edges, delete middle
        g_SplitMediaItem(mi, splitEnd);
        MediaItem* mid = g_SplitMediaItem(mi, splitStart);
        if (mid) g_DeleteTrackMediaItem(track, mid);
      } else if (pos < splitStart) {
        // Item starts before selection - trim right portion
        MediaItem* right = g_SplitMediaItem(mi, splitStart);
        if (right) g_DeleteTrackMediaItem(track, right);
      } else {
        // Item starts inside selection - trim left portion
        g_SplitMediaItem(mi, splitEnd);
        g_DeleteTrackMediaItem(track, mi);
      }
    }
  } else if (m_waveform.IsTimelineOrMultiItem()) {
    // Timeline/Multi-item view: handle delete across segments (no ripple)
    track = g_GetMediaItem_Track(m_waveform.GetItem());
    if (track) {
      int count = g_GetTrackNumMediaItems(track);
      std::vector<MediaItem*> overlap;
      for (int i = 0; i < count; i++) {
        MediaItem* mi = g_GetTrackMediaItem(track, i);
        if (!mi) continue;
        double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
        double len = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
        if (pos + len > splitStart && pos < splitEnd)
          overlap.push_back(mi);
      }
      for (MediaItem* mi : overlap) {
        double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
        double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
        if (pos >= splitStart && end <= splitEnd) {
          g_DeleteTrackMediaItem(track, mi);
        } else if (pos < splitStart && end > splitEnd) {
          MediaItem* rightPart = g_SplitMediaItem(mi, splitEnd);
          MediaItem* mid = g_SplitMediaItem(mi, splitStart);
          if (mid) g_DeleteTrackMediaItem(track, mid);
          survivors.push_back(mi);        // left survives
          if (rightPart) survivors.push_back(rightPart); // right survives
        } else if (pos < splitStart) {
          MediaItem* right = g_SplitMediaItem(mi, splitStart);
          if (right) g_DeleteTrackMediaItem(track, right);
          survivors.push_back(mi);        // left portion survives
        } else {
          MediaItem* rightPart = g_SplitMediaItem(mi, splitEnd);
          g_DeleteTrackMediaItem(track, mi);
          if (rightPart) survivors.push_back(rightPart); // right portion survives
        }
      }
    }
  } else {
    // Single item: split at selection edges, delete middle
    MediaItem* item = m_waveform.GetItem();
    if (!item) {
      if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Delete", -1);
      if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
      return;
    }
    track = g_GetMediaItem_Track(item);
    double itemPos = g_GetMediaItemInfo_Value(item, "D_POSITION");
    double itemEnd = itemPos + g_GetMediaItemInfo_Value(item, "D_LENGTH");
    bool atStart = (splitStart - itemPos) < 0.0001;
    bool atEnd = (itemEnd - splitEnd) < 0.0001;

    if (atStart && atEnd) {
      g_DeleteTrackMediaItem(track, item);
    } else if (atStart) {
      MediaItem* right = g_SplitMediaItem(item, splitEnd);
      if (track) g_DeleteTrackMediaItem(track, item);
      if (right) survivors.push_back(right);
    } else if (atEnd) {
      MediaItem* right = g_SplitMediaItem(item, splitStart);
      if (right && track) g_DeleteTrackMediaItem(track, right);
      survivors.push_back(item);
    } else {
      MediaItem* rightPart = g_SplitMediaItem(item, splitEnd);
      MediaItem* mid = g_SplitMediaItem(item, splitStart);
      if (mid && track) g_DeleteTrackMediaItem(track, mid);
      survivors.push_back(item);
      if (rightPart) survivors.push_back(rightPart);
    }
  }

  // Ripple edit: pull all subsequent items left by deleted duration
  // Always ripple in SET mode; optional via Shift+Delete in other modes
  if ((m_workingSet.active || ripple) && track && g_GetTrackNumMediaItems && g_GetTrackMediaItem &&
      g_SetMediaItemInfo_Value && g_GetMediaItemInfo_Value && deletedDuration > 0.0) {
    int count = g_GetTrackNumMediaItems(track);
    for (int i = 0; i < count; i++) {
      MediaItem* mi = g_GetTrackMediaItem(track, i);
      if (!mi) continue;
      double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
      if (pos >= splitStart) {
        g_SetMediaItemInfo_Value(mi, "D_POSITION", pos - deletedDuration);
      }
    }
    // Shrink working set range to match
    m_workingSet.endPos -= deletedDuration;
  }

  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr,
      ripple ? "SneakPeak: Ripple Delete" : "SneakPeak: Delete (non-destructive)", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  m_waveform.ClearSelection();
  m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS; // suppress timeline exit for ~150ms after edit

  // Track view: refresh to show updated track (items re-collapse)
  if (m_workingSet.active) {
    // Rebuild items list from track (split/delete created new items not in original list)
    if (track && g_GetTrackNumMediaItems && g_GetTrackMediaItem) {
      m_workingSet.items.clear();
      int cnt = g_GetTrackNumMediaItems(track);
      for (int i = 0; i < cnt; i++) {
        MediaItem* mi = g_GetTrackMediaItem(track, i);
        if (!mi) continue;
        double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
        double len = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
        if (pos + len > m_workingSet.startPos && pos < m_workingSet.endPos)
          m_workingSet.items.push_back(mi);
      }
    }
    RefreshWorkingSet();
  } else if (m_waveform.IsTimelineOrMultiItem()) {
    // Rebuild view from surviving items on track
    // Collect valid segment items + split survivors, then find all track items in their span
    double savedViewStart = m_waveform.GetViewStart();
    double savedViewDur = m_waveform.GetViewDuration();

    std::vector<MediaItem*> known; // known surviving items
    for (const auto& seg : m_waveform.GetSegments()) {
      if (!seg.item) continue;
      if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, seg.item, "MediaItem*")) continue;
      known.push_back(seg.item);
    }
    for (auto* s : survivors) {
      if (!s) continue;
      if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, s, "MediaItem*")) continue;
      bool found = false;
      for (auto* e : known) if (e == s) { found = true; break; }
      if (!found) known.push_back(s);
    }

    // Determine actual span from surviving items' current positions
    double tlStart = 1e30, tlEnd = -1e30;
    for (auto* mi : known) {
      double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
      double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
      if (pos < tlStart) tlStart = pos;
      if (end > tlEnd) tlEnd = end;
    }

    // Collect all track items within that span (includes any we missed)
    std::vector<MediaItem*> items;
    if (track && g_GetTrackNumMediaItems && g_GetTrackMediaItem && tlEnd > tlStart) {
      int cnt = g_GetTrackNumMediaItems(track);
      for (int i = 0; i < cnt; i++) {
        MediaItem* mi = g_GetTrackMediaItem(track, i);
        if (!mi) continue;
        double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
        double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
        if (pos >= tlStart - 0.001 && end <= tlEnd + 0.001 && end > pos)
          items.push_back(mi);
      }
    }
    DBG("[SneakPeak] Timeline refresh: %d items\n", (int)items.size());
    m_waveform.ClearItem();
    if (items.size() >= 2) {
      m_waveform.LoadTimelineView(items);
      { std::vector<MediaItem*> segItems;
        for (const auto& seg : m_waveform.GetSegments()) if (seg.item) segItems.push_back(seg.item);
        if (!segItems.empty()) m_gainPanel.ShowBatch(segItems);
      }
      double dur = m_waveform.GetItemDuration();
      if (dur > 0) {
        double vd = std::min(savedViewDur, dur);
        double vs = std::min(savedViewStart, std::max(0.0, dur - vd));
        m_waveform.SetViewStart(vs);
        m_waveform.SetViewDuration(vd);
      }
      m_waveform.Invalidate();
    } else if (!items.empty()) {
      m_waveform.SetItem(items[0]);
    } else {
      LoadSelectedItem();
    }
  } else {
    double savedViewStart = m_waveform.GetViewStart();
    double savedViewDur = m_waveform.GetViewDuration();
    double savedCursor = m_waveform.GetCursorTime();

    DBG("[SneakPeak] DoDelete: %d survivors, entering timeline view=%d\n",
        (int)survivors.size(), survivors.size() >= 2 ? 1 : 0);
    m_waveform.ClearItem();
    if (survivors.size() >= 2) {
      m_waveform.LoadTimelineView(survivors);
      // Switch gain panel to batch mode (offset from 0, not absolute dB)
      { std::vector<MediaItem*> segItems;
        for (const auto& seg : m_waveform.GetSegments()) if (seg.item) segItems.push_back(seg.item);
        if (!segItems.empty()) m_gainPanel.ShowBatch(segItems);
      }
      DBG("[SneakPeak] LoadTimelineView done: hasItem=%d dur=%.3f timelineActive=%d\n",
          m_waveform.HasItem(), m_waveform.GetItemDuration(), m_waveform.IsTimelineView());
    } else {
      LoadSelectedItem();
    }

    // Restore zoom position, clamped to new duration
    if (m_waveform.HasItem()) {
      double dur = m_waveform.GetItemDuration();
      if (dur > 0) {
        double vd = std::min(savedViewDur, dur);
        double vs = std::min(savedViewStart, std::max(0.0, dur - vd));
        m_waveform.SetViewStart(vs);
        m_waveform.SetViewDuration(vd);
      }
      m_waveform.SetCursorTime(std::min(savedCursor, dur));
      m_waveform.Invalidate();
    }
  }

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoSilence()
{
  if (!m_waveform.HasItem()) return;

  // --- Standalone mode ---
  if (m_waveform.IsStandaloneMode()) {
    auto& data = m_waveform.GetAudioData();
    int nch = m_waveform.GetNumChannels();
    int sr = m_waveform.GetSampleRate();
    int totalFrames = m_waveform.GetAudioSampleCount();
    int fadeFrames = std::min(sr / 200, 50); // ~5ms crossfade to avoid clicks

    if (m_waveform.HasSelection()) {
      // Mode 1: Replace selection with silence + edge crossfades
      WaveformSelection sel = m_waveform.GetSelection();
      int startFrame = std::max(0, std::min(totalFrames, (int)(std::min(sel.startTime, sel.endTime) * sr)));
      int endFrame = std::max(0, std::min(totalFrames, (int)(std::max(sel.startTime, sel.endTime) * sr)));
      if (endFrame <= startFrame) return;

      StandaloneUndoSave();

      // Zero out the selection region
      int selFrames = endFrame - startFrame;
      memset(data.data() + (size_t)startFrame * nch, 0, (size_t)selFrames * nch * sizeof(double));

      // Crossfade at left edge (existing audio → silence)
      int fadeLenL = std::min(fadeFrames, startFrame);
      fadeLenL = std::min(fadeLenL, selFrames);
      for (int f = 0; f < fadeLenL; f++) {
        double t = (double)f / (double)fadeLenL;
        double gain = 0.5 * (1.0 + cos(t * M_PI)); // 1→0
        int frame = startFrame - fadeLenL + f;
        for (int ch = 0; ch < nch; ch++)
          data[(size_t)frame * nch + ch] *= gain;
      }

      // Crossfade at right edge (silence → existing audio)
      int fadeLenR = std::min(fadeFrames, totalFrames - endFrame);
      fadeLenR = std::min(fadeLenR, selFrames);
      for (int f = 0; f < fadeLenR; f++) {
        double t = (double)f / (double)fadeLenR;
        double gain = 0.5 * (1.0 - cos(t * M_PI)); // 0→1
        for (int ch = 0; ch < nch; ch++)
          data[(size_t)(endFrame + f) * nch + ch] *= gain;
      }
    } else {
      // Mode 2: Insert silence at cursor position
      double cursorTime = m_waveform.GetCursorTime();
      int insertFrame = std::max(0, std::min(totalFrames, (int)(cursorTime * sr)));

      // Ask user for silence duration via REAPER's GetUserInputs
      if (!g_GetUserInputs) return;
      char buf[64] = "1.0";
      if (!g_GetUserInputs("Insert Silence", 1, "Duration (seconds):", buf, sizeof(buf)))
        return;

      double silenceSec = atof(buf);
      if (silenceSec <= 0.0 || silenceSec > 3600.0) return;

      int silenceFrames = (int)(silenceSec * sr);
      if (silenceFrames <= 0) return;

      StandaloneUndoSave();

      // Insert zero samples at cursor
      size_t insertSample = (size_t)insertFrame * nch;
      size_t insertCount = (size_t)silenceFrames * nch;
      data.insert(data.begin() + insertSample, insertCount, 0.0);

      int newFrames = (int)data.size() / nch;
      double newDur = (double)newFrames / (double)sr;
      m_waveform.SetAudioSampleCount(newFrames);
      m_waveform.SetItemDuration(newDur);

      // Crossfade at left edge (before insert → silence)
      int fadeLenL = std::min(fadeFrames, insertFrame);
      for (int f = 0; f < fadeLenL; f++) {
        double t = (double)f / (double)fadeLenL;
        double gain = 0.5 * (1.0 + cos(t * M_PI));
        int frame = insertFrame - fadeLenL + f;
        for (int ch = 0; ch < nch; ch++)
          data[(size_t)frame * nch + ch] *= gain;
      }

      // Crossfade at right edge (silence → after insert)
      int rightStart = insertFrame + silenceFrames;
      int fadeLenR = std::min(fadeFrames, newFrames - rightStart);
      for (int f = 0; f < fadeLenR; f++) {
        double t = (double)f / (double)fadeLenR;
        double gain = 0.5 * (1.0 - cos(t * M_PI));
        for (int ch = 0; ch < nch; ch++)
          data[(size_t)(rightStart + f) * nch + ch] *= gain;
      }

      // Move cursor to end of inserted silence
      m_waveform.SetCursorTime((double)(insertFrame + silenceFrames) / (double)sr);
    }

    m_dirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // --- REAPER mode: non-destructive split + volume 0 ---
  if (!m_waveform.HasSelection()) return;
  if (!g_SplitMediaItem || !g_SetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  double splitStart = m_waveform.RelTimeToAbsTime(selStart);
  double splitEnd = m_waveform.RelTimeToAbsTime(selEnd);

  // Split at end first
  g_SplitMediaItem(item, splitEnd);
  // Split at start — middlePart is the silence region
  MediaItem* middlePart = g_SplitMediaItem(item, splitStart);

  // Set middle part volume to 0 (silence)
  if (middlePart) {
    g_SetMediaItemInfo_Value(middlePart, "D_VOL", 0.0);
  }

  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Silence (non-destructive)", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  m_waveform.ClearItem();
  LoadSelectedItem();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Processing ---

void SneakPeak::DoNormalize()
{
  // Non-destructive (REAPER) or destructive (standalone)
  if (!m_waveform.HasItem()) return;

  if (m_waveform.IsStandaloneMode()) {
    StandaloneUndoSave();
    auto& data = m_waveform.GetAudioData();
    int nch = m_waveform.GetNumChannels();
    int frames = m_waveform.GetAudioSampleCount();
    if (frames > 0 && nch > 0)
      AudioOps::Normalize(data.data(), frames, nch, 0.989); // -0.1dB
    m_dirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (!g_SetMediaItemInfo_Value) return;

  const auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int totalSamples = (int)data.size();
  if (totalSamples == 0 || nch == 0) return;

  // Find peak across all channels
  double peak = 0.0;
  for (int i = 0; i < totalSamples; i++) {
    double v = fabs(data[i]);
    if (v > peak) peak = v;
  }
  if (peak < 1e-10) return; // silence

  MediaItem* item = m_waveform.GetItem();

  // Target: peak * newVol = 0.989 (-0.1dB)
  // newVol = 0.989 / peak (raw audio peak, D_VOL sets the final level)
  double targetPeak = 0.989; // -0.1 dB
  double newVol = targetPeak / peak;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Normalize (non-destructive)", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoFadeIn()
{
  if (!m_waveform.HasItem()) return;

  if (m_waveform.IsStandaloneMode()) {
    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    int nch = m_waveform.GetNumChannels();
    int selFrames = endF - startF;
    if (selFrames <= 0) return;
    StandaloneUndoSave();
    auto& data = m_waveform.GetAudioData();
    AudioOps::FadeIn(data.data() + (size_t)startF * nch, selFrames, nch);
    m_dirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Non-destructive: set item fade-in length via D_FADEINLEN
  // With selection: fade from item start to selection end
  if (!g_SetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double fadeLen;

  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    fadeLen = std::max(sel.startTime, sel.endTime); // from item start to selection end
  } else {
    fadeLen = m_waveform.GetItemDuration();
  }

  if (fadeLen < 0.001) return;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_FADEINLEN", fadeLen);
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Fade In", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoFadeOut()
{
  if (!m_waveform.HasItem()) return;

  if (m_waveform.IsStandaloneMode()) {
    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    int nch = m_waveform.GetNumChannels();
    int selFrames = endF - startF;
    if (selFrames <= 0) return;
    StandaloneUndoSave();
    auto& data = m_waveform.GetAudioData();
    AudioOps::FadeOut(data.data() + (size_t)startF * nch, selFrames, nch);
    m_dirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Non-destructive: set item fade-out length via D_FADEOUTLEN
  // With selection: fade from selection start to item end
  if (!g_SetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double fadeLen;

  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    double selStart = std::min(sel.startTime, sel.endTime);
    fadeLen = m_waveform.GetItemDuration() - selStart; // from selection start to item end
  } else {
    fadeLen = m_waveform.GetItemDuration();
  }

  if (fadeLen < 0.001) return;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_FADEOUTLEN", fadeLen);
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Fade Out", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoReverse()
{
  // Destructive — no REAPER non-destructive reverse available
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsMultiItem()) {
    MessageBox(m_hwnd, "Reverse is not supported in multi-item view.", "SneakPeak", MB_OK);
    return;
  }

  int ret = MessageBox(m_hwnd,
    "Reverse modifies the audio file on disk. Continue?",
    "SneakPeak — Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  UndoSave();

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  int nch = m_waveform.GetNumChannels();
  int selFrames = endF - startF;

  auto& data = m_waveform.GetAudioData();
  AudioOps::Reverse(data.data() + (size_t)startF * nch, selFrames, nch);

  WriteAndRefresh();

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Reverse (destructive)", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoGain(double factor)
{
  if (!m_waveform.HasItem()) return;

  if (m_waveform.IsStandaloneMode()) {
    StandaloneUndoSave();
    auto& data = m_waveform.GetAudioData();
    int nch = m_waveform.GetNumChannels();
    int sr = m_waveform.GetSampleRate();

    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    int selFrames = endF - startF;
    bool isPartial = m_waveform.HasSelection() && (startF > 0 || endF < m_waveform.GetAudioSampleCount());

    // Apply gain to selection range
    if (selFrames > 0 && nch > 0) {
      size_t offset = (size_t)startF * nch;
      AudioOps::Gain(data.data() + offset, selFrames, nch, factor);

      // Crossfade at edges to avoid clicks (~5ms each side)
      if (isPartial) {
        int fadeLen = std::min(sr / 200, selFrames / 2); // ~5ms
        if (fadeLen > 1) {
          // Fade-in at selection start
          for (int f = 0; f < fadeLen && startF + f < endF; f++) {
            double t = (double)f / (double)fadeLen;
            // Already gained, so undo gain and apply blended
            for (int ch = 0; ch < nch; ch++) {
              size_t idx = (size_t)(startF + f) * nch + ch;
              data[idx] = data[idx] / factor * (1.0 + t * (factor - 1.0));
            }
          }
          // Fade-out at selection end
          for (int f = 0; f < fadeLen && endF - 1 - f >= startF; f++) {
            double t = (double)f / (double)fadeLen;
            for (int ch = 0; ch < nch; ch++) {
              size_t idx = (size_t)(endF - 1 - f) * nch + ch;
              data[idx] = data[idx] / factor * (1.0 + t * (factor - 1.0));
            }
          }
        }
      }
    }
    m_dirty = true;
    m_previewCacheDirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // REAPER mode: selection-aware gain
  if (!g_SetMediaItemInfo_Value || !g_GetMediaItemInfo_Value) return;

  if (m_waveform.HasSelection()) {
    // Partial selection: destructive gain on selection only
    if (m_waveform.IsMultiItem()) return; // not supported for multi-item yet

    if (g_PreventUIRefresh) g_PreventUIRefresh(1);
    if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
    UndoSave();

    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    int nch = m_waveform.GetNumChannels();
    int sr = m_waveform.GetSampleRate();
    int selFrames = endF - startF;
    auto& data = m_waveform.GetAudioData();

    if (selFrames > 0 && nch > 0) {
      size_t offset = (size_t)startF * nch;
      AudioOps::Gain(data.data() + offset, selFrames, nch, factor);

      // Crossfade at edges (~5ms)
      bool isPartial = startF > 0 || endF < m_waveform.GetAudioSampleCount();
      if (isPartial) {
        int fadeLen = std::min(sr / 200, selFrames / 2);
        if (fadeLen > 1) {
          for (int f = 0; f < fadeLen && startF + f < endF; f++) {
            double t = (double)f / (double)fadeLen;
            for (int ch = 0; ch < nch; ch++) {
              size_t idx = (size_t)(startF + f) * nch + ch;
              data[idx] = data[idx] / factor * (1.0 + t * (factor - 1.0));
            }
          }
          for (int f = 0; f < fadeLen && endF - 1 - f >= startF; f++) {
            double t = (double)f / (double)fadeLen;
            for (int ch = 0; ch < nch; ch++) {
              size_t idx = (size_t)(endF - 1 - f) * nch + ch;
              data[idx] = data[idx] / factor * (1.0 + t * (factor - 1.0));
            }
          }
        }
      }
    }

    WriteAndRefresh();

    char desc[64];
    snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB (selection)", 20.0 * log10(factor));
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
    if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
  } else {
    // No selection: non-destructive D_VOL on whole item
    MediaItem* item = m_waveform.GetItem();
    if (!item) return;
    double curVol = g_GetMediaItemInfo_Value(item, "D_VOL");
    double newVol = curVol * factor;

    if (g_PreventUIRefresh) g_PreventUIRefresh(1);
    if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
    g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
    if (g_UpdateArrange) g_UpdateArrange();

    char desc[64];
    snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB", 20.0 * log10(factor));
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
    if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
  }

  m_waveform.Invalidate();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoDCRemove()
{
  // Destructive — must modify audio data
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsMultiItem()) {
    MessageBox(m_hwnd, "DC Remove is not supported in multi-item view.", "SneakPeak", MB_OK);
    return;
  }

  int ret = MessageBox(m_hwnd,
    "DC Offset Remove modifies the audio file on disk. Continue?",
    "SneakPeak — Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  UndoSave();

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  int nch = m_waveform.GetNumChannels();
  int selFrames = endF - startF;

  auto& data = m_waveform.GetAudioData();
  AudioOps::DCOffsetRemove(data.data() + (size_t)startF * nch, selFrames, nch);

  WriteAndRefresh();

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: DC Offset Remove (destructive)", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoNormalizeLUFS(double targetLufs)
{
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsStandaloneMode()) return;
  if (!g_CalculateNormalization || !g_SetMediaItemInfo_Value) return;

  MediaItem_Take* take = m_waveform.GetTake();
  if (!take) return;

  PCM_source* src = g_GetMediaItemTake_Source ? g_GetMediaItemTake_Source(take) : nullptr;
  if (!src) return;

  // normalizeTo=0 (LUFS-I)
  double gainDb = g_CalculateNormalization(src, 0, targetLufs, 0.0, 0.0);

  double gainLin = pow(10.0, gainDb / 20.0);
  if (gainLin < 0.001 || gainLin > 100.0) return; // sanity

  MediaItem* item = m_waveform.GetItem();
  if (!item) return;

  double curVol = g_GetMediaItemInfo_Value(item, "D_VOL");
  double newVol = curVol * gainLin;

  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
  if (g_UpdateArrange) g_UpdateArrange();
  char desc[64];
  snprintf(desc, sizeof(desc), "SneakPeak: Normalize to %.0f LUFS", targetLufs);
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::ApplyDynamicsToEnvelope()
{
  if (!m_waveform.HasItem() || m_waveform.IsStandaloneMode() || !m_waveform.GetTake()) return;

  // Ensure analysis is current
  if (!m_dynamics.HasResults() && m_waveform.GetAudioSampleCount() > 0) {
    double ivDb = 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
    m_dynamics.Analyze(m_waveform.GetAudioData().data(),
                       m_waveform.GetAudioSampleCount(),
                       m_waveform.GetNumChannels(),
                       m_waveform.GetSampleRate(),
                       ivDb, m_dynamics.GetParams());
  }
  if (!m_dynamics.HasResults()) return;

  auto compRaw = m_dynamics.ComputeCompression();
  if (compRaw.empty()) return;

  // Simplify curve: RDP reduces ~60000 points to ~200-500 essential shape points
  auto comp = DynamicsEngine::SimplifyCurve(compRaw, 0.3); // 0.3 dB tolerance
  if (comp.empty()) return;

  if (!g_GetTakeEnvelopeByName || !g_InsertEnvelopePointEx ||
      !g_Envelope_SortPoints || !g_ScaleToEnvelopeMode || !g_GetEnvelopeScalingMode)
    return;

  TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
  if (!env) {
    ShowToast("Enable Volume envelope on item first");
    return;
  }

  int scalingMode = g_GetEnvelopeScalingMode(env);
  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  bool noSort = true;
  for (const auto& cp : comp) {
    double gainLinear = pow(10.0, cp.dbAdjust / 20.0);
    double rawVal = g_ScaleToEnvelopeMode(scalingMode, gainLinear);
    g_InsertEnvelopePointEx(env, -1, cp.time, rawVal, 0, 0.0, false, &noSort);
  }
  g_Envelope_SortPoints(env);

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Apply Dynamics", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
  if (g_UpdateArrange) g_UpdateArrange();
  m_dynamicsVisible = true;
  char toast[64];
  snprintf(toast, sizeof(toast), "Applied %d points (from %d)", (int)comp.size(), (int)compRaw.size());
  ShowToast(toast);
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

