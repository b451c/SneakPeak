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
#include "item_split_ops.h"
#include "wav_inplace.h"
#include "audio_stream.h"
#include "spectral_repair.h"
#include "debug.h"
#include "reaper_plugin.h"
#include "ui_theme.h"   // dynui::kMeterFloorDefaultSel (meter-floor pref migration)

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <mutex>


// --- Selection sample range helper ---

void SneakPeak::GetSelectionSampleRange(int& startFrame, int& endFrame) const
{
  // Multi-item view keeps its samples per layer (no shared buffer, sample
  // count 0): its range is the timeline span at the view rate.
  double sr = (double)m_waveform.GetSampleRate();
  const int total = m_waveform.IsMultiItemActive() ? (int)(m_waveform.GetItemDuration() * sr)
                                                   : m_waveform.GetAudioSampleCount();
  if (!m_waveform.HasSelection()) {
    startFrame = 0;
    endFrame = total;
    return;
  }
  WaveformSelection sel = m_waveform.GetSelection();
  startFrame = std::max(0, (int)(sel.startTime * sr));
  endFrame = std::min(total, (int)(sel.endTime * sr));
  if (endFrame <= startFrame) {
    startFrame = 0;
    endFrame = total;
  }
}


// --- Undo ---

bool SneakPeak::UndoSave()
{
  // Destructive ITEM edits rewrite the source FILE - REAPER's native undo
  // cannot restore that, so these edits were effectively one-way (user
  // report 2026-07-02; the confirm prompt was the only guard). Snapshot the
  // pre-edit FILE as a byte copy in the temp dir - never the working buffer:
  // on long items that buffer is DOWNSAMPLED and writing it back on undo
  // would destroy the source (finding F6). Single level.
  // A failed copy (temp dir missing, disk full) used to be logged and the
  // edit went ahead with no way back (audit A1.3): now it cancels the edit,
  // and the copy goes to the free slot first so the previous snapshot is
  // dropped only once the new one exists.
  if (!m_waveform.IsStandaloneMode() && m_waveform.GetTake()) {
    const std::string path = AudioEngine::GetSourceFilePath(m_waveform.GetTake());
    const std::string buf = UndoSnapshotPath();
    if (!AudioEngine::CopyFileInto(path, buf)) {
      DBG("[SneakPeak] undo snapshot failed: %s\n", buf.c_str());
      AudioEngine::RemoveFile(buf);   // whatever a partial copy left behind
      char msg[sizeof(m_toastText)];
      snprintf(msg, sizeof(msg), "Could not create the pre-edit copy in %s - the edit was cancelled",
               AudioEngine::TempDir().c_str());
      ShowToast(msg);
      return false;
    }
    DiscardItemUndo();
    m_itemUndoSlot ^= 1;
    m_itemUndoPath = path;
    m_itemUndoFile = buf;
  }
  m_hasUndo = true;
  return true;
}

void SneakPeak::DiscardItemUndo()
{
  if (!m_itemUndoFile.empty()) AudioEngine::RemoveFile(m_itemUndoFile);
  m_itemUndoFile.clear();
  m_itemUndoPath.clear();
}

std::string SneakPeak::UndoSnapshotPath() const
{
  char buf[512];
  snprintf(buf, sizeof(buf), "%s/sneakpeak_undo_%d_%c.wav", AudioEngine::TempDir().c_str(),
           AudioEngine::ProcessId(), m_itemUndoSlot ? 'b' : 'a');
  return buf;
}

void SneakPeak::UndoRestore()
{
  if (m_waveform.IsStandaloneMode()) {
    StandaloneUndoRestore();
    return;
  }
  if (DestructiveJobBusy()) return;   // F5: the file is being rewritten
  // Destructive-edit restore: if the snapshot belongs to the CURRENT take's
  // source file, copy the pre-edit file back (same inode - F7) and reload
  // from disk. REAPER's own undo point for the edit stays in its history as
  // a no-op label - we deliberately do NOT pop it (the user may have made
  // project changes since; popping would undo those instead).
  DBG("[SneakPeak] UndoRestore: hasUndo=%d undoFile='%s' pathMatch=%d\n",
      m_hasUndo ? 1 : 0, m_itemUndoFile.c_str(),
      (m_waveform.GetTake() &&
       AudioEngine::GetSourceFilePath(m_waveform.GetTake()) == m_itemUndoPath) ? 1 : 0);
  if (m_hasUndo && !m_itemUndoFile.empty() && m_waveform.GetTake() &&
      AudioEngine::GetSourceFilePath(m_waveform.GetTake()) == m_itemUndoPath) {
    AbortItemAudioLoad();
    AbortExportPump();
    JoinDynamicsWorker(true);   // the trace job holds accessors on the file being restored
    m_waveform.ReleaseTakeAccessors();
#ifdef _WIN32
    // Same file-locking bracket as BeginDestructiveWrite (F22): Windows keeps
    // the source open through REAPER's decoder, so the snapshot copy-back
    // failed with a sharing violation and undo showed "Undo failed".
    if (g_Main_OnCommand) g_Main_OnCommand(40440, 0);  // Item: set selected media offline
    const bool restored = AudioEngine::CopyFileInto(m_itemUndoFile, m_itemUndoPath);
    if (g_Main_OnCommand) g_Main_OnCommand(40439, 0);  // Item: set selected media online
#else
    const bool restored = AudioEngine::CopyFileInto(m_itemUndoFile, m_itemUndoPath);
#endif
    DBG("[SneakPeak] UndoRestore: restore copy %s\n", restored ? "ok" : "FAILED");
    if (!restored) {
      m_waveform.RecreateLiveAccessor();
      ShowToast("Undo failed - check the file permissions");
      return;
    }
    AudioEngine::RefreshItemSource(m_waveform.GetItem(), m_waveform.GetTake());
    DiscardItemUndo();
    m_hasUndo = false;
    m_waveform.ClearItem();
    LoadSelectedItem();
    ShowToast("Destructive edit undone");
    Invalidate();
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
    Invalidate();
  }
}

void SneakPeak::StandaloneUndoSave()
{
  // Every Standalone buffer edit snapshots first: the worker must not read the
  // buffer while it changes and the engine's cached trace must go (A4.3, A7.4).
  JoinDynamicsWorker(true);
  const auto& data = m_waveform.GetAudioData();
  if (data.empty()) return;
  if ((int)m_standaloneUndoStack.size() >= MAX_STANDALONE_UNDO)
    m_standaloneUndoStack.erase(m_standaloneUndoStack.begin());
  StandaloneUndoEntry e;
  e.full = true;
  e.data = data;
  m_standaloneUndoStack.push_back(std::move(e));
  m_standaloneRedoStack.clear(); // a new edit invalidates the redo branch
  m_hasUndo = true;
  m_previewCacheDirty = true;
  m_standaloneBufferSerial++;
}

// Zero-copy variant for the background limiter apply: the caller REPLACES the
// whole buffer right after, so the old buffer MOVES into the undo slot
// instead of being copied (a full copy of a podcast-length file would stall
// the swap for seconds). Same bookkeeping as StandaloneUndoSave.
void SneakPeak::StandaloneUndoPushFull(std::vector<double>&& oldData)
{
  // Every Standalone buffer edit snapshots first: the worker must not read the
  // buffer while it changes and the engine's cached trace must go (A4.3, A7.4).
  JoinDynamicsWorker(true);
  if (oldData.empty()) return;
  if ((int)m_standaloneUndoStack.size() >= MAX_STANDALONE_UNDO)
    m_standaloneUndoStack.erase(m_standaloneUndoStack.begin());
  StandaloneUndoEntry e;
  e.full = true;
  e.data = std::move(oldData);
  m_standaloneUndoStack.push_back(std::move(e));
  m_standaloneRedoStack.clear();
  m_hasUndo = true;
  m_previewCacheDirty = true;
  m_standaloneBufferSerial++;
}

// Range snapshot (STA-2): for bounded edits that do NOT change the buffer
// length. Only [startFrame, startFrame + numFrames) is saved - a heal on a
// 30-min file costs megabytes instead of gigabytes per undo slot.
void SneakPeak::StandaloneUndoSaveRange(int startFrame, int numFrames)
{
  // Every Standalone buffer edit snapshots first: the worker must not read the
  // buffer while it changes and the engine's cached trace must go (A4.3, A7.4).
  JoinDynamicsWorker(true);
  const auto& data = m_waveform.GetAudioData();
  if (data.empty()) return;
  const int nch = std::max(1, m_waveform.GetNumChannels());
  const int total = (int)(data.size() / (size_t)nch);
  startFrame = std::max(0, std::min(total, startFrame));
  numFrames = std::max(0, std::min(total - startFrame, numFrames));
  if (numFrames <= 0) return;

  if ((int)m_standaloneUndoStack.size() >= MAX_STANDALONE_UNDO)
    m_standaloneUndoStack.erase(m_standaloneUndoStack.begin());
  StandaloneUndoEntry e;
  e.full = false;
  e.startFrame = startFrame;
  const size_t off = (size_t)startFrame * nch;
  const size_t len = (size_t)numFrames * nch;
  e.data.assign(data.begin() + off, data.begin() + off + len);
  m_standaloneUndoStack.push_back(std::move(e));
  m_standaloneRedoStack.clear();
  m_hasUndo = true;
  m_previewCacheDirty = true;
  m_standaloneBufferSerial++;
}

// Swap `entry` with the live buffer, pushing the exact inverse onto
// `inverseStack` (undo pushes onto redo and vice versa). Full entries swap the
// whole buffer; range entries swap only their slice (range edits never change
// the buffer length by contract, the clamp is defense in depth).
void SneakPeak::StandaloneApplyUndoEntry(StandaloneUndoEntry& entry,
                                         std::vector<StandaloneUndoEntry>& inverseStack)
{
  auto& data = m_waveform.GetAudioData();
  const int nch = std::max(1, m_waveform.GetNumChannels());
  StandaloneUndoEntry inv;
  if (entry.full) {
    inv.full = true;
    inv.data = std::move(data);
    data = std::move(entry.data);
  } else {
    inv.full = false;
    inv.startFrame = entry.startFrame;
    size_t off = (size_t)entry.startFrame * (size_t)nch;
    size_t len = entry.data.size();
    if (off > data.size()) off = data.size();
    if (off + len > data.size()) len = data.size() - off;
    inv.data.assign(data.begin() + off, data.begin() + off + len);
    std::copy(entry.data.begin(), entry.data.begin() + len, data.begin() + off);
  }
  if ((int)inverseStack.size() >= MAX_STANDALONE_UNDO)
    inverseStack.erase(inverseStack.begin());
  inverseStack.push_back(std::move(inv));
  m_standaloneBufferSerial++;
}

// Shared tail of standalone undo/redo: recalc duration, drop stale
// selection/fade, invalidate every audio-derived view (incl. the spectrogram -
// it renders the buffer we just swapped).
void SneakPeak::StandaloneFinishRestore(const char* what)
{
  (void)what; // DBG-only
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int newFrames = (nch > 0) ? (int)m_waveform.GetAudioData().size() / nch : 0;
  double newDur = (sr > 0) ? (double)newFrames / (double)sr : 0.0;
  m_waveform.SetAudioSampleCount(newFrames);
  m_waveform.SetItemDuration(newDur);

  m_waveform.ClearSelection();
  m_waveform.ClearStandaloneFade(); // clear non-destructive fade on undo/redo
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  ResetSpectrum();
  InvalidateLimiterPreview();       // the GR preview renders the buffer we just swapped
  m_hasUndo = !m_standaloneUndoStack.empty();
  m_dirty = true;
  m_previewCacheDirty = true;
  UpdateTitle();
  Invalidate();
  DBG("[SneakPeak] Standalone %s (undo=%d redo=%d, frames=%d, dur=%.3f)\n",
      what, (int)m_standaloneUndoStack.size(), (int)m_standaloneRedoStack.size(),
      newFrames, newDur);
}

void SneakPeak::StandaloneUndoRestore()
{
  if (m_standaloneUndoStack.empty()) return;
  JoinDynamicsWorker(true);   // the buffer is about to be replaced (A4.3)
  StandaloneApplyUndoEntry(m_standaloneUndoStack.back(), m_standaloneRedoStack);
  m_standaloneUndoStack.pop_back();
  StandaloneFinishRestore("undo");
}

void SneakPeak::StandaloneRedoRestore()
{
  if (m_standaloneRedoStack.empty()) return;
  JoinDynamicsWorker(true);
  // No redo clear here - only a NEW edit in StandaloneUndoSave* cuts the branch
  StandaloneApplyUndoEntry(m_standaloneRedoStack.back(), m_standaloneUndoStack);
  m_standaloneRedoStack.pop_back();
  StandaloneFinishRestore("redo");
}

void SneakPeak::RedoRestore()
{
  if (m_waveform.IsStandaloneMode()) {
    StandaloneRedoRestore();
    return;
  }
  if (DestructiveJobBusy()) return;   // F5
  // Trigger REAPER's native redo (action 40030 = Edit: Redo) and reload the
  // view the same way UndoRestore does.
  if (g_Main_OnCommand) {
    g_Main_OnCommand(40030, 0);
    if (m_workingSet.active) {
      RefreshWorkingSet();
    } else if (m_waveform.IsTimelineView()) {
      m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
      RefreshTimelineView();
    } else {
      m_waveform.ClearItem();
      LoadSelectedItem();
    }
    Invalidate();
  }
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
    Invalidate();
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

// Destructive-write bracket shared by the buffer write and the in-place file
// ops: WAV sources only, and our own readers (live accessor, retained cache,
// background loader) are dropped while the file changes under them. F7: every
// write lands in the SAME inode so REAPER's pooled decoders serve the new
// audio at once.
// The preconditions of an in-place rewrite of the take's file, as one short
// reason (UX audit 2026-08-29, F2). Every destructive entry point evaluates
// it BEFORE its confirm prompt and the context menu / limiter panel show it
// on the greyed control, so the user is never refused after a Yes. A take
// reversed in REAPER, or trimmed to a section of its file, plays through a
// SECTION source whose parent is the WAV: GetSourceFilePath names the parent
// and the item-to-file mapping (offset, playrate, direction) no longer
// describes what plays, so an in-place edit would land on the wrong region
// of the parent (audit A1.2). Non-WAV sources have no in-place editor; Edit
// Copy makes a WAV of the item in Standalone.
std::string SneakPeak::DestructiveTargetReason() const
{
  if (m_waveform.IsMultiItem()) return "not available in Multi-item view";
  if (m_destructiveJob.active) return m_destructiveJob.verb + " in progress - wait or press Esc";
  MediaItem_Take* take = m_waveform.GetTake();
  if (!m_waveform.HasItem() || !take) return "no item loaded";
  if (AudioEngine::IsSectionSource(take)) return "the take plays a section or reversed source";
  const std::string path = AudioEngine::GetSourceFilePath(take);
  if (path.empty()) return "the source has no file on disk";
  std::string ext;
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) ext = path.substr(dot + 1);
  for (auto& c : ext) c = (char)tolower((unsigned char)c);
  if (ext != "wav" && ext != "wave") {
    for (auto& c : ext) c = (char)toupper((unsigned char)c);
    return "the source is " + ext + " - WAV only (use Edit Copy)";
  }
  return std::string();
}

bool SneakPeak::DestructiveTargetOk()
{
  const std::string reason = DestructiveTargetReason();
  if (reason.empty()) return true;
  char msg[sizeof(m_toastText)];
  snprintf(msg, sizeof(msg), "Cannot rewrite the file: %s", reason.c_str());
  ShowToast(msg);
  return false;
}

// The Hard Limiter's greyed-Apply reason (F2/F3): the single-take views only
// (SET / master have no one take to map the range through), then the shared
// preconditions, then the range cap - WavInplace::Limit holds the item's
// window (or the selection) in memory whole (kLimiterMaxBytes).
std::string SneakPeak::LimiterTargetReason() const
{
  if (!SingleItemViewOk()) return "not available in this view";
  const std::string reason = DestructiveTargetReason();
  if (!reason.empty()) return reason;
  int64_t s0, s1;
  GetSelectionSourceRange(s0, s1);
  const int64_t srcCh = std::max(1, m_waveform.GetSrcChannels());
  const int64_t rate = std::max(1, m_waveform.GetSourceSampleRate());
  if ((s1 - s0) * srcCh * (int64_t)sizeof(double) > kLimiterMaxBytes) {
    char buf[96];
    snprintf(buf, sizeof(buf), "range too long for the limiter - select up to %.0f min here",
             (double)(kLimiterMaxBytes / (int64_t)sizeof(double)) / (double)(srcCh * rate) / 60.0);
    return buf;
  }
  return std::string();
}

bool SneakPeak::BeginDestructiveWrite(std::string& path)
{
  if (!DestructiveTargetOk()) return false;   // the entry point refused already: last line of defence
  path = AudioEngine::GetSourceFilePath(m_waveform.GetTake());
  AbortItemAudioLoad();
  AbortExportPump();
  JoinDynamicsWorker(true);   // the trace job holds accessors on the file being rewritten
  m_waveform.ReleaseTakeAccessors();
#ifdef _WIN32
  // Windows keeps the source file open through REAPER's own take decoder and
  // refuses an in-place overwrite while it is held ("target is in use"), so the
  // destructive edit silently failed and popped the "Failed to write" box
  // (forum #105 platform). Take the selected item's media offline for the
  // duration of the write - FinishDestructiveWrite brings those same items back
  // (a background write lets the selection move meanwhile). macOS and Linux
  // overwrite an open file fine, so this is Windows-only. The item we edit
  // is REAPER's selected item in ITEM mode (the selection poll loads it).
  TakeSelectionOffline();
#endif
  return true;
}

// The pre-edit copy UndoSave took, back over the take's file (UndoRestore
// keeps its own copy-back: it brackets the file offline itself).
bool SneakPeak::RestoreFromSnapshot()
{
  return m_hasUndo && !m_itemUndoFile.empty() && m_waveform.GetTake() &&
         AudioEngine::GetSourceFilePath(m_waveform.GetTake()) == m_itemUndoPath &&
         AudioEngine::CopyFileInto(m_itemUndoFile, m_itemUndoPath);
}

void SneakPeak::EndDestructiveWrite(bool written)
{
  // Rollback (audit A1.4): a whole-file write that fails part-way (a disk
  // that fills up) leaves a truncated file; the snapshot taken moments ago
  // goes back - inside the Windows offline bracket, like the write itself.
  // (The in-place ops roll back on their worker - destructive_job.cpp.)
  FinishDestructiveWrite(written, !written && RestoreFromSnapshot());
}

void SneakPeak::FinishDestructiveWrite(bool written, bool restored)
{
  BringOfflineItemsBackOnline();   // Windows: the items BeginDestructiveWrite took offline
  if (!written) {
    if (!restored) {
      m_waveform.RecreateLiveAccessor();
      char msg[640];
      snprintf(msg, sizeof(msg), "Write failed and the file could NOT be restored.\n%s%s",
               m_itemUndoFile.empty() ? "There is no pre-edit copy." : "The pre-edit copy is at:\n",
               m_itemUndoFile.c_str());
      MessageBox(m_hwnd, msg, "SneakPeak", MB_OK | MB_ICONERROR);
      return;
    }
    AudioEngine::RefreshItemSource(m_waveform.GetItem(), m_waveform.GetTake());
    DiscardItemUndo();   // the file IS the snapshot again: nothing to undo
    m_hasUndo = false;
    m_waveform.ClearItem();
    LoadSelectedItem();  // the display buffer already carries the edit - reload from disk
    ShowToast("Write failed - the file was restored from the pre-edit copy");
    return;
  }
  AudioEngine::RefreshItemSource(m_waveform.GetItem(), m_waveform.GetTake());
  m_waveform.RecreateLiveAccessor();
  if (g_AudioAccessorValidateState && m_waveform.GetLiveAccessor())
    { std::lock_guard<std::mutex> lk(AudioStream::ApiLock());
      g_AudioAccessorValidateState(m_waveform.GetLiveAccessor()); }   // our own write is not an external change

  ResetSpectrum();   // no external-change reload follows: the edited display buffer recomputes here
  m_waveform.Invalidate();
  m_dirty = true;
  UpdateTitle();
}

// The working buffer is capped at 2 channels and folded to 1 by the take's
// mono channel modes (audio_stream.h FoldedChannels), so a whole-file write
// of it over a 6-channel file drops channels 3-6 and a mono-mode stereo item
// collapses its file to mono (audit A1.1, P0): the channel count is part of
// "covers the whole file", not a detail the caller may skip.
bool SneakPeak::BufferCoversWholeFile(const std::string& path, WavInfo& srcInfo) const
{
  if (!AudioEngine::ReadWavHeader(path, srcInfo) || srcInfo.bitsPerSample <= 0) return false;
  return m_waveform.GetSampleRate() == srcInfo.sampleRate &&
         m_waveform.GetNumChannels() == srcInfo.numChannels &&
         m_waveform.GetTakeOffset() == 0.0 && m_waveform.GetTakePlayrate() == 1.0 &&
         std::abs((int64_t)m_waveform.GetAudioSampleCount() - (int64_t)srcInfo.numFrames) <= 1;
}

// The whole-file write's own preconditions, with the refusal toast: never a
// downsampled buffer (F6), never a trimmed, offset or rate-changed item (F12).
// Paste asks before its prompt (F2); WriteAndRefresh re-checks after the
// accessors are released.
bool SneakPeak::WholeFileWriteOk(WavInfo& srcInfo)
{
  if (!m_waveform.IsItemBufferDownsampled() &&
      BufferCoversWholeFile(AudioEngine::GetSourceFilePath(m_waveform.GetTake()), srcInfo))
    return true;
  ShowToast(m_waveform.IsItemBufferDownsampled()
                ? "Item too long for this edit - the file was not changed"
                : "This edit needs the item to cover the whole source file - the file was not changed");
  return false;
}

void SneakPeak::WriteAndRefresh()
{
  std::string path;
  if (!BeginDestructiveWrite(path)) return;

  const auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();

  // The buffer covers the ITEM and this write replaces the WHOLE file, so it
  // is only valid when the two coincide: never a downsampled buffer (10M-frame
  // cap - F6) and never a trimmed, offset or rate-changed item, which would
  // truncate the source to the item's window (F12). Reverse / Gain / DC Remove
  // edit the file in place instead (WriteAndRefreshInplace).
  WavInfo srcInfo;
  if (!WholeFileWriteOk(srcInfo)) {
    m_waveform.RecreateLiveAccessor();
    return;
  }
  // Write back in the SOURCE file's own format. m_wavBitsPerSample tracks
  // standalone loads only (default 16): using it here silently re-encoded a
  // 24-bit or float item as 16-bit PCM on Reverse/DC Remove.
  EndDestructiveWrite(AudioEngine::WriteWavFile(path, data.data(), frames, nch, sr,
                                                srcInfo.bitsPerSample, srcInfo.audioFormat));
}

// The selection (whole item when none) in FILE frames: item time -> source
// time through the take's start offset and playrate, at the source's own rate.
void SneakPeak::GetSelectionSourceRange(int64_t& startFrame, int64_t& endFrame) const
{
  double t0 = 0.0, t1 = m_waveform.GetItemDuration();
  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    t0 = std::max(0.0, std::min(sel.startTime, sel.endTime));
    t1 = std::min(m_waveform.GetItemDuration(), std::max(sel.startTime, sel.endTime));
    if (t1 <= t0) { t0 = 0.0; t1 = m_waveform.GetItemDuration(); }
  }
  const double rate = m_waveform.GetTakePlayrate() > 0.0 ? m_waveform.GetTakePlayrate() : 1.0;
  const double sr = (double)m_waveform.GetSourceSampleRate();
  const double off = m_waveform.GetTakeOffset();
  startFrame = (int64_t)((off + t0 * rate) * sr + 0.5);
  endFrame = (int64_t)((off + t1 * rate) * sr + 0.5);
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

bool SneakPeak::DoCopy()
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return false;

  if (!m_waveform.IsStandaloneMode() && DestructiveJobBusy()) return false;   // F5: reads the file
  // Sync selection to REAPER so native copy works on the right range
  SyncSelectionToReaper();

  if (!m_waveform.IsStandaloneMode() && !m_waveform.IsMultiItemActive()) {
    // Single-item / timeline / SET: stream the selection from the source at
    // its own rate (8e SliceSamples, item volume baked like every export) -
    // the working buffer is downsampled on long items (F11: an 8 kHz
    // clipboard) and absent on lazy ones (8g). Bounded by the buffer cap.
    WaveformSelection sel = m_waveform.GetSelection();
    const double t0 = std::max(0.0, std::min(sel.startTime, sel.endTime));
    const double t1 = std::min(m_waveform.GetItemDuration(), std::max(sel.startTime, sel.endTime));
    const int nch = std::max(1, m_waveform.GetNumChannels());
    const int srcRate = m_waveform.GetSourceSampleRate();
    if (t1 <= t0 || srcRate <= 0) return false;
    if ((int64_t)((t1 - t0) * srcRate) * nch * (int64_t)sizeof(double) > WaveformView::kMaxBufferBytes) {
      const int maxMin = (int)(WaveformView::kMaxBufferBytes / (nch * (int64_t)sizeof(double)) / srcRate / 60);
      char msg[96];
      snprintf(msg, sizeof(msg), "Selection too long to copy (about %d min max at this rate)", maxMin);
      ShowToast(msg);
      return false;
    }
    std::vector<double> out;
    int outNch = 0, outRate = 0;
    if (!SliceSamples(t0, t1, out, &outNch, &outRate) || outNch <= 0) {
      ShowToast("Could not read the item audio - nothing copied");
      return false;
    }
    s_clipboard.numFrames = 0;   // fill samples first, set numFrames last
    s_clipboard.samples = std::move(out);
    s_clipboard.numChannels = outNch;
    s_clipboard.sampleRate = outRate;
    s_clipboard.numFrames = (int)(s_clipboard.samples.size() / (size_t)outNch);
    if (g_Main_OnCommand) g_Main_OnCommand(40060, 0);   // REAPER's native copy too
    DBG("[SneakPeak] Copied %d frames @ %d Hz to clipboard (streamed)\n", s_clipboard.numFrames, outRate);
    return true;
  }
  if (!RequireItemAudio("Copy")) return false;   // multi-item layers (eager); Standalone is always ready

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  int nch = m_waveform.GetNumChannels();
  int selFrames = endF - startF;
  if (selFrames <= 0) return false;

  // Internal clipboard - fill samples first, set numFrames last
  if (m_waveform.IsMultiItemActive()) {
    // Multi-item view only: mix all layers in selected range
    m_waveform.GetMultiItemView().GetMixedAudio(startF, endF, nch, s_clipboard.samples);
  } else {
    // Standalone: copy from the full-rate buffer
    const auto& data = m_waveform.GetAudioData();
    size_t srcOffset = (size_t)startF * nch;
    size_t copyLen = (size_t)selFrames * nch;
    if (srcOffset + copyLen > data.size()) return false;
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
  return true;
}

void SneakPeak::DoCut()
{
  // Copy + ripple delete (standard waveform editor behavior: cut closes the gap)
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (!m_waveform.IsStandaloneMode() && DestructiveJobBusy()) return;   // F5: item pinned

  // A refused copy (over the buffer cap, unreadable audio) must not delete:
  // the selection would be gone and the clipboard stale (audit A2.1).
  if (DoCopy()) DoDelete(true); // ripple
}

void SneakPeak::DoPaste()
{
  if (!m_waveform.HasItem() || s_clipboard.numFrames <= 0) return;
  // Standalone mode: destructive paste (no REAPER track)
  if (m_waveform.IsStandaloneMode()) {
    DoPasteDestructive();
    return;
  }
  if (DestructiveJobBusy()) return;   // F5: item pinned

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

  // The pasted clip becomes project media, so it lives in the project's
  // recording path (GetProjectPathEx: REAPER's default recording folder while
  // the project is unsaved) - the OS temp folder gets purged and the saved
  // project would lose the clip (audit A2.2). The name carries the source's
  // base name and a per-session counter (two pastes within one second used
  // to overwrite each other's media).
  static int s_pasteSerial = 0;
  char clipPath[512];
  {
    std::string base = "clip";
    const std::string src = AudioEngine::GetSourceFilePath(m_waveform.GetTake());
    const size_t slash = src.find_last_of("/\\");
    const std::string fn = slash == std::string::npos ? src : src.substr(slash + 1);
    const size_t dot = fn.find_last_of('.');
    if (!fn.empty()) base = dot == std::string::npos ? fn : fn.substr(0, dot);
    char dir[512] = {};
    if (g_GetProjectPathEx) g_GetProjectPathEx(nullptr, dir, sizeof(dir));
    const std::string projDir = dir[0] ? dir : AudioEngine::TempDir();
    snprintf(clipPath, sizeof(clipPath), "%s/sneakpeak_paste_%s_%d_%d.wav", projDir.c_str(),
             base.c_str(), AudioEngine::ProcessId(), ++s_pasteSerial);
  }
  if (!AudioEngine::WriteWavFile(clipPath, s_clipboard.samples.data(),
      s_clipboard.numFrames, s_clipboard.numChannels, s_clipboard.sampleRate, 32, 3)) {
    ShowToast("Could not write the pasted clip into the project folder - nothing pasted");
    return;
  }

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

  // A locked item under the cursor: nothing is split or moved (A6.4)
  if (AnyItemLocked(track, absPos - 0.001, absPos + 0.001)) {
    ShowToast("Item is locked in REAPER - unlock it to edit here");
    return;
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
  // (locked items stay, as under REAPER's own ripple)
  cnt = g_GetTrackNumMediaItems(track); // re-count after split
  for (int i = cnt - 1; i >= 0; i--) { // reverse to avoid double-shift
    MediaItem* mi = g_GetTrackMediaItem(track, i);
    if (!mi || ItemLocked(mi)) continue;
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    if (pos >= absPos - 0.0001)
      g_SetMediaItemInfo_Value(mi, "D_POSITION", pos + clipDur);
  }

  // Create new item in the gap
  MediaItem* newItem = g_AddMediaItemToTrack(track);
  if (newItem) {
    MediaItem_Take* newTake = g_AddTakeToMediaItem(newItem);
    if (newTake) {
      PCM_source* src = g_PCM_Source_CreateFromFile(clipPath);
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
  Invalidate();
}

void SneakPeak::DoPasteDestructive()
{
  if (!RequireItemAudio("Paste")) return;
  if (s_clipboard.numChannels != m_waveform.GetNumChannels()) return;
  if (m_waveform.IsStandaloneMode()) {
    // Standalone: an in-memory edit with its own undo stack - no file prompt,
    // no item, no ITEM-mode snapshot (audit A4.4: UndoSave took none here and
    // the item path wrote D_LENGTH to a null item).
    JoinDynamicsWorker(true);
    StandaloneUndoSave();
    const int nch = m_waveform.GetNumChannels();
    const int insertFrame = std::max(0, std::min(m_waveform.GetAudioSampleCount(),
                             (int)(m_waveform.GetCursorTime() * (double)m_waveform.GetSampleRate())));
    auto& data = m_waveform.GetAudioData();
    data.insert(data.begin() + (long)((size_t)insertFrame * nch),
                s_clipboard.samples.begin(), s_clipboard.samples.end());
    const int newFrames = m_waveform.GetAudioSampleCount() + s_clipboard.numFrames;
    m_waveform.SetAudioSampleCount(newFrames);
    m_waveform.SetItemDuration((double)newFrames / (double)m_waveform.GetSampleRate());
    m_dirty = true;
    m_previewCacheDirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    m_minimap.Invalidate();
    Invalidate();
    return;
  }
  if (!DestructiveTargetOk()) return;
  WavInfo srcInfo;
  if (!WholeFileWriteOk(srcInfo)) return;   // F2: before the prompt, not after the Yes

  int ret = MessageBox(m_hwnd,
    "Paste modifies the audio file on disk. Continue?",
    "SneakPeak - Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  if (!UndoSave()) return;   // no pre-edit copy = no edit (A1.3)
  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

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

  Invalidate();
}

void SneakPeak::DoDelete(bool ripple)
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (m_waveform.IsStandaloneMode()) { DoDeleteStandalone(); return; }
  if (DestructiveJobBusy()) return;   // F5: item pinned
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
    Invalidate();
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
  bool lockedSkipped = false;        // A6.4: locked items are left alone, toast once

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
      if (pos + len > splitStart && pos < splitEnd) {
        if (ItemLocked(mi)) { lockedSkipped = true; continue; }
        overlap.push_back(mi);
      }
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
        if (pos + len > splitStart && pos < splitEnd) {
          if (ItemLocked(mi)) { lockedSkipped = true; continue; }
          overlap.push_back(mi);
        }
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

    if (ItemLocked(item)) {
      lockedSkipped = true;
      deletedDuration = 0.0;   // nothing to ripple
    } else if (atStart && atEnd) {
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
      if (!mi || ItemLocked(mi)) continue;
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
  if (lockedSkipped) ShowToast("Item is locked in REAPER - unlock it to edit here");

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

  Invalidate();
}

void SneakPeak::DoSilence()
{
  if (!m_waveform.HasItem()) return;
  if (!m_waveform.IsStandaloneMode() && DestructiveJobBusy()) return;   // F5: item pinned

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

      // Bounded edit (STA-2): the zeroed selection plus the edge crossfades
      // (each at most fadeFrames long) - never more than that is touched.
      StandaloneUndoSaveRange(startFrame - fadeFrames,
                              (endFrame - startFrame) + 2 * fadeFrames);

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
    Invalidate();
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
  Invalidate();
}

// --- Processing ---

void SneakPeak::DoNormalize()
{
  // Non-destructive (REAPER) or destructive (standalone)
  if (!m_waveform.HasItem()) return;
  if (!m_waveform.IsStandaloneMode() && DestructiveJobBusy()) return;   // F5: streams the file

  if (m_waveform.IsStandaloneMode()) {
    if (!RequireItemAudio("Normalize")) return;   // standalone edits its buffer
    StandaloneUndoSave();
    auto& data = m_waveform.GetAudioData();
    int nch = m_waveform.GetNumChannels();
    int frames = m_waveform.GetAudioSampleCount();
    if (frames > 0 && nch > 0)
      AudioOps::Normalize(data.data(), frames, nch, 0.989); // -0.1dB
    m_dirty = true;
    UpdateTitle();
    ResetSpectrum();
    m_waveform.Invalidate();
    Invalidate();
    return;
  }

  if (!g_SetMediaItemInfo_Value) return;

  double peak = 0.0;
  if (m_waveform.ItemBufferIsLazy() || m_waveform.IsItemBufferDownsampled() ||
      m_waveform.GetAudioSampleCount() == 0) {
    // F17: a long item's working buffer is downsampled (transients read low, so
    // the item normalized too hot) or, since 8g, absent. Stream the TRUE peak
    // from the source at full rate - the same synchronous class as the in-place
    // edits (the accessor reads an hour of audio in under a second). Raw peak,
    // no item volume baked in, matching the buffer path (D_VOL is the knob this
    // command sets).
    AudioStream stream;
    if (!m_waveform.OpenStream(stream, 0.0, m_waveform.GetItemDuration(), false)) return;
    std::vector<double> chunk((size_t)262144 * (size_t)stream.Channels());
    while (stream.Remaining() > 0) {
      const int n = (int)std::min<int64_t>(262144, stream.Remaining());
      if (!stream.Read(chunk.data(), n)) return;
      const size_t cnt = (size_t)n * (size_t)stream.Channels();
      for (size_t i = 0; i < cnt; ++i) {
        const double v = fabs(chunk[i]);
        if (v > peak) peak = v;
      }
    }
  } else {
    if (!RequireItemAudio("Normalize")) return;
    const auto& data = m_waveform.GetAudioData();
    int nch = m_waveform.GetNumChannels();
    int totalSamples = (int)data.size();
    if (totalSamples == 0 || nch == 0) return;
    for (int i = 0; i < totalSamples; i++) {
      double v = fabs(data[i]);
      if (v > peak) peak = v;
    }
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

  Invalidate();
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
    Invalidate();
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

  Invalidate();
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
    Invalidate();
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

  Invalidate();
}

void SneakPeak::DoReverse()
{
  // Destructive — no REAPER non-destructive reverse available. Needs no
  // samples: the file is edited in place (8b); the buffer edit below is
  // display-only and a no-op while a lazy item has none (8g).
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsStandaloneMode()) {   // in-memory edit, own undo stack (A4.4)
    JoinDynamicsWorker(true);
    StandaloneUndoSave();
    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    const int nch = m_waveform.GetNumChannels();
    if (endF > startF && nch > 0)
      AudioOps::Reverse(m_waveform.GetAudioData().data() + (size_t)startF * nch, endF - startF, nch);
    m_dirty = true;
    m_previewCacheDirty = true;
    UpdateTitle();
    ResetSpectrum();
    m_waveform.Invalidate();
    Invalidate();
    return;
  }

  if (!DestructiveTargetOk()) return;
  int ret = MessageBox(m_hwnd,
    "Reverse modifies the audio file on disk. Continue?",
    "SneakPeak - Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  // F5: the file edit, its pre-edit copy and REAPER's refresh run through the
  // job (destructive_job.cpp); the display edit lands with them on success.
  StartDestructiveJob("Reverse", "Reversing", "SneakPeak: Reverse (destructive)",
    [](const std::string& p, int64_t a, int64_t b, const WavInplace::Progress* prog) {
      return WavInplace::Reverse(p, a, b, prog);
    },
    [this, startF, endF]() {
      const int nch = m_waveform.GetNumChannels();
      const int e = std::min(endF, m_waveform.GetAudioSampleCount());
      if (e > startF && nch > 0)
        AudioOps::Reverse(m_waveform.GetAudioData().data() + (size_t)startF * nch, e - startF, nch);
    });
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
    ResetSpectrum();   // the spectrogram is on a fixed dBFS scale: gain shows
    m_waveform.Invalidate();
    Invalidate();
    return;
  }

  // REAPER mode: selection-aware gain
  if (!g_SetMediaItemInfo_Value || !g_GetMediaItemInfo_Value) return;

  if (m_waveform.HasSelection()) {
    // Partial selection: destructive gain on selection only
    if (!DestructiveTargetOk()) return;   // Multi-item view is one of its reasons
    // F1 (UX audit 2026-08-29): the only destructive command that did not ask.
    char prompt[96];
    snprintf(prompt, sizeof(prompt),
             "Gain %+.1f dB on the selection modifies the audio file on disk. Continue?",
             20.0 * log10(factor));
    if (MessageBox(m_hwnd, prompt, "SneakPeak - Destructive Operation",
                   MB_YESNO | MB_ICONWARNING) != IDYES) return;

    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    const int fade = m_waveform.GetSourceSampleRate() / 200;   // ~5 ms at the file's rate
    char desc[64];
    snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB (selection)", 20.0 * log10(factor));
    // F5: file edit + pre-edit copy + refresh through the job (destructive_job.cpp).
    StartDestructiveJob("Gain", "Applying gain", desc,
      [factor, fade](const std::string& p, int64_t a, int64_t b, const WavInplace::Progress* prog) {
        return WavInplace::Gain(p, a, b, factor, fade, prog);
      },
      [this, factor, startF, endF]() {   // the display, with the edge crossfade (~5 ms)
        auto& data = m_waveform.GetAudioData();
        const int nch = m_waveform.GetNumChannels();
        const int sr = m_waveform.GetSampleRate();
        const int e = std::min(endF, m_waveform.GetAudioSampleCount());
        const int selFrames = e - startF;
        if (selFrames <= 0 || nch <= 0) return;
        AudioOps::Gain(data.data() + (size_t)startF * nch, selFrames, nch, factor);
        const bool isPartial = startF > 0 || e < m_waveform.GetAudioSampleCount();
        if (!isPartial) return;
        const int fadeLen = std::min(sr / 200, selFrames / 2);
        if (fadeLen <= 1) return;
        for (int f = 0; f < fadeLen && startF + f < e; f++) {
          double t = (double)f / (double)fadeLen;
          for (int ch = 0; ch < nch; ch++) {
            size_t idx = (size_t)(startF + f) * nch + ch;
            data[idx] = data[idx] / factor * (1.0 + t * (factor - 1.0));
          }
        }
        for (int f = 0; f < fadeLen && e - 1 - f >= startF; f++) {
          double t = (double)f / (double)fadeLen;
          for (int ch = 0; ch < nch; ch++) {
            size_t idx = (size_t)(e - 1 - f) * nch + ch;
            data[idx] = data[idx] / factor * (1.0 + t * (factor - 1.0));
          }
        }
      });
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
  Invalidate();
}

void SneakPeak::DoDCRemove()
{
  // Destructive — edits the file in place (8b); no samples needed (8g)
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsStandaloneMode()) {   // in-memory edit, own undo stack (A4.4)
    JoinDynamicsWorker(true);
    StandaloneUndoSave();
    int startF, endF;
    GetSelectionSampleRange(startF, endF);
    const int nch = m_waveform.GetNumChannels();
    if (endF > startF && nch > 0)
      AudioOps::DCOffsetRemove(m_waveform.GetAudioData().data() + (size_t)startF * nch, endF - startF, nch);
    m_dirty = true;
    m_previewCacheDirty = true;
    UpdateTitle();
    ResetSpectrum();
    m_waveform.Invalidate();
    Invalidate();
    return;
  }

  if (!DestructiveTargetOk()) return;
  int ret = MessageBox(m_hwnd,
    "DC Offset Remove modifies the audio file on disk. Continue?",
    "SneakPeak - Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  // F5: file edit + pre-edit copy + refresh through the job (destructive_job.cpp).
  StartDestructiveJob("DC Offset Remove", "Removing DC offset", "SneakPeak: DC Offset Remove (destructive)",
    [](const std::string& p, int64_t a, int64_t b, const WavInplace::Progress* prog) {
      return WavInplace::DCRemove(p, a, b, prog);
    },
    [this, startF, endF]() {
      const int nch = m_waveform.GetNumChannels();
      const int e = std::min(endF, m_waveform.GetAudioSampleCount());
      if (e > startF && nch > 0)
        AudioOps::DCOffsetRemove(m_waveform.GetAudioData().data() + (size_t)startF * nch, e - startF, nch);
    });
}

void SneakPeak::DoNormalizeLUFS(double targetLufs)
{
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsStandaloneMode()) return;
  if (DestructiveJobBusy()) return;   // F5: REAPER would measure a file mid-edit
  if (!g_CalculateNormalization || !g_SetMediaItemInfo_Value) return;   // REAPER measures the source

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

  Invalidate();
}

// --- Spectral Repair (v2.3.0 INC-5): standalone destructive v1 ---
// Both commands operate on the in-memory standalone buffer with a
// StandaloneUndoSave snapshot first; the DSP lives in spectral_repair.cpp.
// The spectrogram recomputes async on the next paint after ClearSpectrum().

void SneakPeak::DoSpectralHeal(double strength)
{
  if (!m_waveform.HasItem() || !m_waveform.IsStandaloneMode()) return;
  if (!m_waveform.HasSelection() || !m_spectral.HasFreqSelection()) return;

  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);
  if (selEnd - selStart > SPECTRAL_HEAL_MAX_SEC) {
    ShowToast("Heal is limited to 10 s selections");
    return;
  }

  auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0 || nch <= 0 || sr <= 0) return;

  // Bounded edit (STA-2): the heal resynthesizes at most selection +- one FFT
  // window (2048) around the selected span - snapshot only that range. The
  // depth check below keeps the failure pop honest (a degenerate range saves
  // nothing, so there would be nothing of ours to pop).
  const size_t undoDepth = m_standaloneUndoStack.size();
  {
    int u0 = std::max(0, (int)(selStart * sr + 0.5) - 2048);
    int u1 = std::min(frames, (int)(selEnd * sr + 0.5) + 2048);
    StandaloneUndoSaveRange(u0, u1 - u0);
  }
  SpectralHealResult r = StftRepairRect(data.data(), frames, nch, sr,
                                        selStart, selEnd,
                                        m_spectral.GetFreqSelLow(),
                                        m_spectral.GetFreqSelHigh(), strength);
  if (!r.ok) {
    if (m_standaloneUndoStack.size() > undoDepth)
      m_standaloneUndoStack.pop_back(); // buffer untouched - drop our slot
    m_hasUndo = !m_standaloneUndoStack.empty();
    ShowToast("Heal failed: selection too short or no surrounding context");
    return;
  }

  char buf[96];
  double atten = (fabs(r.avgAttenDb) < 0.05) ? 0.0 : r.avgAttenDb; // no '-0.0'
  snprintf(buf, sizeof(buf), "Healed selection (avg %.1f dB in band)", atten);
  ShowToast(buf);

  m_dirty = true;
  UpdateTitle();
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  ResetSpectrum();
  Invalidate();
}

// v2.4.0 INC-L1: destructive true-peak hard limit on the standalone buffer.
// Runs in the BACKGROUND on a copy (a 30-min podcast takes ~35 s of DSP - a
// synchronous apply would freeze the window): the worker limits the copy with
// title progress, and LimiterApplyTick swaps the result in only if the live
// buffer is untouched (edit serial + tab + length), else it is discarded with
// a toast - the user's newer edits always win. Whole file -> zero-copy full
// undo (old buffer moves into the slot); selection -> range snapshot + 20 ms
// envelope handoff ramps at the edges.
void SneakPeak::DoApplyLimiter()
{
  if (!SingleItemViewOk()) return;
  if (!m_waveform.IsStandaloneMode()) {   // INC-L2 / F3: in-place rewrite of the item's window
    DoApplyLimiterItem();
    return;
  }
  if (m_limApplyBusy.load()) {
    ShowToast("Limiter is already running...");
    return;
  }

  const int nch = m_waveform.GetNumChannels();
  const int sr = m_waveform.GetSampleRate();
  const int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0 || nch <= 0 || sr <= 0) return;

  int s0 = 0, s1 = frames, ramp = 0;
  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    const double a = std::min(sel.startTime, sel.endTime);
    const double b = std::max(sel.startTime, sel.endTime);
    s0 = std::max(0, std::min(frames, (int)(a * sr + 0.5)));
    s1 = std::max(s0, std::min(frames, (int)(b * sr + 0.5)));
    if (s1 - s0 < 64) {
      ShowToast("Selection too short to limit");
      return;
    }
    ramp = (int)(0.020 * sr + 0.5);   // 20 ms handoff into untouched audio
  }

  if (m_limApplyThread.joinable()) m_limApplyThread.join();
  m_limApplyOut = m_waveform.GetAudioData();   // worker owns this copy
  m_limApplyParams = m_limiterPanel.GetParams();
  m_limApplyS0 = s0;
  m_limApplyS1 = s1;
  m_limApplyFrames = frames;
  m_limApplySerial = m_standaloneBufferSerial;
  m_limApplyFileIdx = m_activeFileIdx;
  m_limApplyCancel.store(false);
  m_limApplyPct.store(0);
  m_limApplyDone.store(false);
  m_limApplyBusy.store(true);
  m_limiterPanel.SetApplyProgress(0);   // Apply button becomes the progress bar
  Invalidate();
  m_limApplyThread = std::thread(&SneakPeak::LimiterApplyThread, this, nch, sr,
                                 s0, s1, ramp);
}

// INC-D1: destructive Dynamics apply in STANDALONE - the classic offline
// compressor. The engine's per-point dbAdjust (comp GR + makeup + gate +
// de-ess: exactly what the envelope Apply writes in ITEM mode) is converted
// to linear gain per point and interpolated per-sample into the buffer -
// points sit ~ms apart, so linear-gain interpolation between them is
// audibly identical to linear-dB and an order of magnitude cheaper. One
// full undo snapshot (the whole file changes). Engine untouched: baselines
// stay byte-identical.
void SneakPeak::DoApplyDynamicsStandalone()
{
  if (!m_waveform.HasItem() || !m_waveform.IsStandaloneMode()) return;
  const int nch = m_waveform.GetNumChannels();
  const int sr = m_waveform.GetSampleRate();
  const int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0 || nch <= 0 || sr <= 0) return;

  auto& data = m_waveform.GetAudioData();
  // Fresh analysis with the panel's params (knob edits keep them in sync).
  m_dynamics.SetParams(m_dynamicsPanel.GetParams());
  m_dynamics.Analyze(data.data(), frames, nch, sr, 0.0, m_dynamics.GetParams());
  const std::vector<DynamicsEngine::CompressPoint> pts =
      m_dynamics.ComputeCompression();
  if (pts.empty()) {
    ShowToast("Nothing to apply");
    return;
  }

  std::vector<double> out(data.size());
  const DynamicsParams& p = m_dynamics.GetParams();
  const std::vector<double>& dsGRs = m_dynamics.GetDeEssGRs();
  const bool split = p.dsEnable && p.dsSplit && dsGRs.size() == pts.size();
  if (split) {
    // Split-band (v2.5.0 row 15): the de-ess share of the curve cuts only the
    // detector band; comp + gate + makeup stay a wideband gain. Both lanes
    // share the analysis grid (one point per step).
    std::vector<double> gWide(pts.size()), gBand(pts.size());
    for (size_t i = 0; i < pts.size(); i++) {
      gWide[i] = pow(10.0, (pts[i].dbAdjust - dsGRs[i]) / 20.0);
      gBand[i] = pow(10.0, dsGRs[i] / 20.0);
    }
    DeEssApplySplit(data.data(), out.data(), frames, nch, (double)sr, p.dsMode,
                    p.dsFreqHz, p.dsQ, gWide, gBand, m_dynamics.GetTrace()->samplesPerStep);
  } else {
    // Per-point linear gains, then a per-sample lerp between neighbours.
    std::vector<double> gains(pts.size());
    for (size_t i = 0; i < pts.size(); i++)
      gains[i] = pow(10.0, pts[i].dbAdjust / 20.0);

    size_t k = 0;
    for (int i = 0; i < frames; i++) {
      const double t = (double)i / (double)sr;
      while (k + 1 < pts.size() && pts[k + 1].time <= t) k++;
      double g = gains[k];
      if (k + 1 < pts.size() && pts[k + 1].time > pts[k].time) {
        double a = (t - pts[k].time) / (pts[k + 1].time - pts[k].time);
        if (a < 0.0) a = 0.0;
        if (a > 1.0) a = 1.0;
        g += (gains[k + 1] - gains[k]) * a;
      }
      for (int c = 0; c < nch; c++) out[(size_t)i * nch + c] = data[(size_t)i * nch + c] * g;
    }
  }

  const double avgGr = m_dynamics.GetAvgGainReduction();
  StandaloneUndoPushFull(std::move(data));   // zero-copy: old buffer -> undo (+serial bump)
  data = std::move(out);

  char buf[96];
  snprintf(buf, sizeof(buf),
           split ? "Dynamics applied (avg GR %.1f dB, de-ess split-band)"
                 : "Dynamics applied (avg GR %.1f dB)", avgGr);
  ShowToast(buf);
  m_dirty = true;
  UpdateTitle();
  // The buffer changed: refresh everything that renders or caches it, and
  // re-analyze so the curves show the processed signal.
  m_dynamics.Analyze(data.data(), frames, nch, sr, 0.0, m_dynamics.GetParams());
  m_dynamics.ComputeCompression();
  RefreshDynamicsAvgGr();
  m_previewCacheDirty = true;
  m_osPreviewDirty = true;
  InvalidateLimiterPreview();
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  ResetSpectrum();
  Invalidate();
}

// INC-L2 / F3 (UX audit 2026-08-29): ITEM-mode Apply limits the item's window
// of the source file IN PLACE through the F5 job (WavInplace::Limit - the
// pre-edit copy, the DSP and any rollback on a worker, Esc cancels), like
// Reverse / Gain / DC: trimmed items, downsampled and lazy buffers included;
// a selection limits that range with 20 ms handoff ramps. NOT an envelope
// effect (locked: a dBTP ceiling needs per-sample gain). The preconditions
// are the panel's greyed-Apply reasons (LimiterTargetReason), re-checked
// here; the confirm is the only dialog. Nothing above the ceiling with 0 dB
// gain leaves the file, the undo slot and REAPER's undo history alone.
void SneakPeak::DoApplyLimiterItem()
{
  if (m_waveform.IsStandaloneMode()) return;
  {
    const std::string reason = LimiterTargetReason();
    if (!reason.empty()) {
      char msg[sizeof(m_toastText)];
      snprintf(msg, sizeof(msg), "Cannot rewrite the file: %s", reason.c_str());
      ShowToast(msg);
      return;
    }
  }
  int64_t s0, s1;
  GetSelectionSourceRange(s0, s1);
  if (s1 - s0 < 64) {
    ShowToast("Selection too short to limit");
    return;
  }
  const int ramp = (int)(0.020 * m_waveform.GetSourceSampleRate() + 0.5);   // handoff into the untouched file

  int ret = MessageBox(m_hwnd,
    "Hard Limiter modifies the audio file on disk. Continue?",
    "SneakPeak - Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

  // The display: a full-rate buffer that maps 1:1 onto the range takes the
  // worker's processed samples; otherwise (lazy, downsampled, folded or
  // rate-changed) the buffer is dropped and reloads from the edited file
  // through the OnTimer pump (the open panel keeps a lazy item loading).
  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  const bool direct = m_waveform.GetAudioSampleCount() > 0 && !m_waveform.IsItemBufferDownsampled() &&
                      m_waveform.GetNumChannels() == m_waveform.GetSrcChannels() &&
                      TakeChanMode(m_waveform.GetTake()) == 0 && m_waveform.GetTakePlayrate() == 1.0 &&
                      std::abs((int64_t)(endF - startF) - (s1 - s0)) <= 1;
  const LimiterParams p = m_limiterPanel.GetParams();
  auto out = std::make_shared<std::vector<double>>();   // the processed range, for the display
  auto res = std::make_shared<LimiterResult>();
  bool* unchanged = &m_destructiveJob.unchanged;
  StartDestructiveJob("Hard Limiter", "Limiting", "SneakPeak: Hard Limiter (destructive)",
    [p, ramp, direct, out, res, unchanged](const std::string& path, int64_t a, int64_t b,
                                            const WavInplace::Progress* prog) {
      if (!WavInplace::Limit(path, a, b, p, ramp, *res, direct ? out.get() : nullptr, prog)) return false;
      *unchanged = p.gainDb == 0.0 && res->maxGainReductionDb == 0.0;
      return true;
    },
    [this, p, startF, out, res]() {
      if (p.gainDb == 0.0 && res->maxGainReductionDb == 0.0) {
        ShowToast("Nothing above the ceiling - audio unchanged");
        return;
      }
      auto& data = m_waveform.GetAudioData();
      const size_t at = (size_t)startF * (size_t)m_waveform.GetNumChannels();
      if (!out->empty() && at < data.size())
        std::copy_n(out->begin(), std::min(out->size(), data.size() - at), data.begin() + (ptrdiff_t)at);
      else
        m_waveform.ReloadAfterExternalChange();   // drop the stale buffer; the pump refills it
      char buf[96];
      snprintf(buf, sizeof(buf), "Limited: out %.1f %s, max GR %.1f dB",
               res->outputPeakDb, p.truePeak ? "dBTP" : "dBFS", res->maxGainReductionDb);
      ShowToast(buf);
      m_minimap.Invalidate();
      InvalidateLimiterPreview();   // recompute the GR band for the limited audio
    });
}

void SneakPeak::LimiterApplyThread(int nch, int sr, int s0, int s1, int ramp)
{
  LimiterProgress prog;
  prog.user = this;
  prog.fn = [](void* user, double frac) -> bool {
    SneakPeak* self = (SneakPeak*)user;
    self->m_limApplyPct.store((int)(frac * 100.0 + 0.5));
    return !self->m_limApplyCancel.load();
  };
  m_limApplyResult =
      LimiterProcess(m_limApplyOut.data() + (size_t)s0 * (size_t)nch, s1 - s0,
                     nch, sr, m_limApplyParams, ramp, &prog);
  m_limApplyDone.store(true);   // busy stays set until the tick finalizes
}

// OnTimer: title progress while the apply worker runs; swap/discard on finish.
void SneakPeak::LimiterApplyTick()
{
  if (!m_limApplyBusy.load()) return;
  if (!m_limApplyDone.load()) {
    // Progress where the user is looking: the panel's Apply button. The
    // title is a secondary cue (docked windows have no title bar at all).
    m_limiterPanel.SetApplyProgress(m_limApplyPct.load());
    Invalidate();
    if (m_hwnd) {
      char t[64];
      snprintf(t, sizeof(t), "SneakPeak: Limiting... %d%%", m_limApplyPct.load());
      SetWindowText(m_hwnd, t);
    }
    return;
  }
  m_limApplyDone.store(false);
  if (m_limApplyThread.joinable()) m_limApplyThread.join();
  m_limApplyBusy.store(false);
  m_limiterPanel.SetApplyProgress(-1);   // button back to "Apply"

  const LimiterResult r = m_limApplyResult;
  const LimiterParams p = m_limApplyParams;
  const bool stale = m_standaloneBufferSerial != m_limApplySerial ||
                     m_activeFileIdx != m_limApplyFileIdx ||
                     !m_waveform.IsStandaloneMode() ||
                     m_waveform.GetAudioSampleCount() != m_limApplyFrames;
  if (!r.ok || stale ||
      (p.gainDb == 0.0 && r.maxGainReductionDb == 0.0)) {
    // Nothing swapped in - the live buffer was never touched, so no undo slot
    // exists to pop. Report why and restore the title.
    if (!r.ok)
      ShowToast(r.cancelled ? "Limiter cancelled"
                            : "Limiter failed: empty buffer");
    else if (stale)
      ShowToast("Limiter result discarded - audio changed meanwhile");
    else
      ShowToast("Nothing above the ceiling - audio unchanged");
    m_limApplyOut = std::vector<double>();
    UpdateTitle();
    Invalidate();
    return;
  }

  JoinDynamicsWorker(true);   // the live buffer is swapped below (A4.3)
  auto& data = m_waveform.GetAudioData();
  if (m_limApplyS0 == 0 && m_limApplyS1 == m_limApplyFrames) {
    StandaloneUndoPushFull(std::move(data));   // zero-copy: old buffer -> undo
  } else {
    StandaloneUndoSaveRange(m_limApplyS0, m_limApplyS1 - m_limApplyS0);
  }
  data = std::move(m_limApplyOut);             // processed copy becomes live
  m_limApplyOut = std::vector<double>();

  char buf[96];
  snprintf(buf, sizeof(buf), "Limited: out %.1f %s, max GR %.1f dB",
           r.outputPeakDb, p.truePeak ? "dBTP" : "dBFS", r.maxGainReductionDb);
  ShowToast(buf);

  m_dirty = true;
  UpdateTitle();
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  ResetSpectrum();
  InvalidateLimiterPreview();   // recompute the GR band for the limited buffer
  Invalidate();
}

void SneakPeak::DoRepairClicks()
{
  if (!m_waveform.HasItem() || !m_waveform.IsStandaloneMode()) return;
  if (!m_waveform.HasSelection()) return;

  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);
  if (selEnd - selStart > CLICK_REPAIR_MAX_SEC) {
    ShowToast("Click repair is limited to 4 s selections");
    return;
  }

  auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0 || nch <= 0 || sr <= 0) return;

  // Bounded edit (STA-2): click repair only ever modifies samples inside the
  // selection (mask-limited) - snapshot that range with a small safety margin.
  // Depth check keeps the failure pop honest (degenerate range saves nothing).
  const size_t undoDepth = m_standaloneUndoStack.size();
  {
    int u0 = std::max(0, (int)(selStart * sr + 0.5) - 4);
    int u1 = std::min(frames, (int)(selEnd * sr + 0.5) + 4);
    StandaloneUndoSaveRange(u0, u1 - u0);
  }
  ClickRepairResult r = RepairClicksAR(data.data(), frames, nch, sr,
                                       selStart, selEnd, 2.0); // K per paper
  if (!r.ok || r.samplesRepaired == 0) {
    if (m_standaloneUndoStack.size() > undoDepth)
      m_standaloneUndoStack.pop_back(); // buffer untouched - drop our slot
    m_hasUndo = !m_standaloneUndoStack.empty();
    ShowToast(r.ok ? "No clicks found in the selection"
                   : "Click repair failed: selection too short");
    return;
  }

  char buf[96];
  if (r.clicksSkipped > 0)
    snprintf(buf, sizeof(buf), "Repaired %d click%s (%d skipped: over 23 ms)",
             r.clicksRepaired, r.clicksRepaired == 1 ? "" : "s", r.clicksSkipped);
  else
    snprintf(buf, sizeof(buf), "Repaired %d click%s",
             r.clicksRepaired, r.clicksRepaired == 1 ? "" : "s");
  ShowToast(buf);

  m_dirty = true;
  UpdateTitle();
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  ResetSpectrum();
  Invalidate();
}

void SneakPeak::ApplyDynamicsToEnvelope()
{
  if (!m_waveform.HasItem() || m_waveform.IsStandaloneMode() || !m_waveform.GetTake()) return;

  // Self-ensure the target take volume envelope(s) exist before writing. This makes
  // Live (and Apply) work on items that have no envelope yet - including a session
  // where Live was restored ON - without the user first enabling the envelope by hand.
  // Done lazily (only on an actual write, not on panel open) and BEFORE the undo/refresh
  // transaction below; EnsureVolumeEnvelope no-ops once the envelope is present.
  if (g_GetTakeEnvelopeByName) {
    auto& ensSegs = m_waveform.GetSegments();
    if ((m_waveform.IsTimelineView() || m_waveform.IsTrackView()) && ensSegs.size() > 1) {
      for (const auto& seg : ensSegs)
        if (seg.take && seg.item && !g_GetTakeEnvelopeByName(seg.take, "Volume"))
          EnsureVolumeEnvelope(seg.take, seg.item);
    } else if (m_waveform.GetItem() && !g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume")) {
      EnsureVolumeEnvelope(m_waveform.GetTake(), m_waveform.GetItem());
    }
  }

  // No analysis for the CURRENT params yet (trace still streaming, worker
  // busy or its result not swapped in): the apply is PENDING and fires from
  // TakeDynamicsResult when the up-to-date result lands.
  if (!m_dynamics.HasResults() || m_dynParamsDirty || m_dynWorker.busy.load() ||
      m_dynWorker.hasResult.load()) {
    m_dynApplyPending = true;
    RequestDynamicsAnalysis();
    return;
  }

  // The pipeline's worker built the curve for these knobs (simplified from
  // every trace point; TakeDynamicsResult swapped it in): reuse it instead of
  // simplifying millions of points on the main thread (A7.1). The fallback
  // (a Flush wrote the analysis synchronously) builds it here.
  if (m_dynamics.EnvelopeCurve().empty()) m_dynamics.BuildEnvelopeCurve(m_dynamics.ComputeCompression());
  std::vector<DynamicsEngine::CompressPoint> comp = m_dynamics.EnvelopeCurve();
  const int rawCount = (int)m_dynamics.EnvelopeCurveSource();
  if (comp.empty()) return;

  if (!g_GetTakeEnvelopeByName || !g_InsertEnvelopePointEx ||
      !g_Envelope_SortPoints || !g_ScaleToEnvelopeMode || !g_GetEnvelopeScalingMode)
    return;

  bool liveSession = m_dynamicsPanel.IsLive() && m_dynamicsPanel.LiveUndoOpen();
  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (!liveSession && g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  // Take-envelope ceiling clamp (v2.3.0 Up mode): boosts can exceed the
  // envelope's MAXVAL (default 2.0 = +6 dB) and REAPER would clamp silently on
  // render - clamp HERE and toast once so the written curve matches what is
  // heard. MAXVAL read like waveform_rendering.cpp QueryEnvelopeMaxGain.
  int clampedPts = 0;
  double clampCeilLin = 2.0;
  auto envMaxGain = [&](TrackEnvelope* env) -> double {
    if (!env || !g_GetEnvelopeStateChunk) return 2.0;
    char buf[512]; // header only, not full chunk
    if (!g_GetEnvelopeStateChunk(env, buf, sizeof(buf), false)) return 2.0;
    const char* p = strstr(buf, "\nMAXVAL ");
    if (!p) p = strstr(buf, " MAXVAL ");
    if (!p) return 2.0;
    double mv = 0.0;
    if (sscanf(p + 8, "%lf", &mv) == 1 && mv > 0.0) return mv;
    return 2.0;
  };

  // Helper: apply compression points to a single envelope
  auto applyToEnv = [&](TrackEnvelope* env, const std::vector<DynamicsEngine::CompressPoint>& pts) {
    if (pts.empty()) return;
    int scalingMode = g_GetEnvelopeScalingMode(env);
    const double maxGain = envMaxGain(env);
    double tStart = pts.front().time;
    double tEnd = pts.back().time;

    // Clear existing points and insert guard points
    if (g_Envelope_Evaluate && g_DeleteEnvelopePointRange) {
      double valStart = 0.0, valEnd = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
      g_Envelope_Evaluate(env, tStart, 44100.0, 0, &valStart, &d1, &d2, &d3);
      g_Envelope_Evaluate(env, tEnd, 44100.0, 0, &valEnd, &d1, &d2, &d3);
      g_DeleteEnvelopePointRange(env, tStart - 0.0001, tEnd + 0.0001);
      bool noSortGuard = true;
      if (tStart > 0.001)
        g_InsertEnvelopePointEx(env, -1, tStart - 0.0001, valStart, 0, 0.0, false, &noSortGuard);
      g_InsertEnvelopePointEx(env, -1, tEnd + 0.0001, valEnd, 0, 0.0, false, &noSortGuard);
    }

    bool noSort = true;
    for (const auto& cp : pts) {
      double gainLinear = pow(10.0, cp.dbAdjust / 20.0);
      if (gainLinear > maxGain) {
        gainLinear = maxGain;      // envelope ceiling (see clamp note above)
        clampedPts++;
        clampCeilLin = maxGain;
      }
      double rawVal = g_ScaleToEnvelopeMode(scalingMode, gainLinear);
      g_InsertEnvelopePointEx(env, -1, cp.time, rawVal, 0, 0.0, false, &noSort);
    }
    g_Envelope_SortPoints(env);
  };

  auto& segs = m_waveform.GetSegments();
  bool isMultiSeg = (m_waveform.IsTimelineView() || m_waveform.IsTrackView()) && segs.size() > 1;
  bool anyEnv = false;

  if (isMultiSeg) {
    // The curve's gain at any view time (linear in dB between points, flat
    // outside). Every segment gets the curve's value at BOTH of its edges:
    // applyToEnv only rewrites the span its points cover, so a plateau that
    // crosses a segment boundary used to leave the rest of the take at the
    // envelope's old value (0 dB) - a step mid-item on every segment (A7.8).
    auto gainAt = [&](double t) -> double {
      if (t <= comp.front().time) return comp.front().dbAdjust;
      if (t >= comp.back().time) return comp.back().dbAdjust;
      auto hi = std::lower_bound(comp.begin(), comp.end(), t,
          [](const DynamicsEngine::CompressPoint& p, double tt) { return p.time < tt; });
      const auto& b = *hi;
      const auto& a = *(hi - 1);
      const double span = b.time - a.time;
      return span > 0.0 ? a.dbAdjust + (b.dbAdjust - a.dbAdjust) * (t - a.time) / span : b.dbAdjust;
    };
    // Group compression points by segment, convert to segment-relative time
    for (size_t si = 0; si < segs.size(); si++) {
      const auto& seg = segs[si];
      if (!seg.take) continue;
      TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
      if (!env) continue;
      anyEnv = true;

      double segStart = seg.relativeOffset;
      double segEnd = segStart + seg.duration;
      std::vector<DynamicsEngine::CompressPoint> segPts;
      DynamicsEngine::CompressPoint edge;
      edge.time = 0.0;
      edge.dbAdjust = gainAt(segStart);
      segPts.push_back(edge);
      for (const auto& cp : comp) {
        if (cp.time > segStart && cp.time < segEnd) {
          DynamicsEngine::CompressPoint sp;
          sp.time = (cp.time - segStart) * seg.playrate; // segment-relative take-envelope time
          sp.dbAdjust = cp.dbAdjust;
          segPts.push_back(sp);
        }
      }
      edge.time = seg.duration * seg.playrate;
      edge.dbAdjust = gainAt(segEnd);
      segPts.push_back(edge);
      applyToEnv(env, segPts);
    }
  } else {
    TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
    if (env) {
      anyEnv = true;
      double rate = m_waveform.GetTakePlayrate();
      if (rate != 1.0)
        for (auto& cp : comp) cp.time *= rate; // take-envelope time = item time * playrate
      applyToEnv(env, comp);
    }
  }

  if (!anyEnv) {
    if (!liveSession && g_Undo_EndBlock2)
      g_Undo_EndBlock2(nullptr, "SneakPeak: Apply Dynamics", -1);
    if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
    ShowToast("Enable Volume envelope on item first");
    return;
  }

  if (!liveSession && g_Undo_EndBlock2)
    g_Undo_EndBlock2(nullptr, "SneakPeak: Apply Dynamics", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
  if (g_UpdateArrange) g_UpdateArrange();
  m_dynamicsVisible = true;
  if (!liveSession) {
    char toast[96];
    if (clampedPts > 0)
      snprintf(toast, sizeof(toast), "Applied %d points - %d at the +%.1f dB ceiling",
               (int)comp.size(), clampedPts, 20.0 * log10(clampCeilLin));
    else if (m_dynamics.EnvelopeCurveEpsilon() > DynamicsEngine::kEnvelopeEpsilonDb)
      snprintf(toast, sizeof(toast), "Envelope simplified to %d points (tolerance %.2f dB)",
               (int)comp.size(), m_dynamics.EnvelopeCurveEpsilon());
    else
      snprintf(toast, sizeof(toast), "Applied %d points (from %d)", (int)comp.size(), rawCount);
    ShowToast(toast);
  }
  Invalidate();
}

void SneakPeak::SaveDynamicsToItem()
{
  if (!g_GetSetMediaItemInfo_String || !m_waveform.GetItem()) return;
  char buf[256];
  DynamicsParamsToString(m_dynamicsPanel.GetParams(), buf, sizeof(buf));
  g_GetSetMediaItemInfo_String(m_waveform.GetItem(), PEXT_DYNAMICS_KEY, buf, true);
}

bool SneakPeak::LoadDynamicsFromItem()
{
  if (!g_GetSetMediaItemInfo_String || !m_waveform.GetItem()) return false;
  char buf[256] = {};
  if (!g_GetSetMediaItemInfo_String(m_waveform.GetItem(), PEXT_DYNAMICS_KEY, buf, false))
    return false;
  if (!buf[0]) return false;
  DynamicsParams loaded;
  if (!DynamicsParamsFromString(buf, loaded)) return false;
  m_dynamicsPanel.Show(loaded, m_dynamics.GetAveragePeakDb());
  RefreshDynamicsAvgGr();
  RestoreDynamicsViewPrefs();
  return true;
}

// Dyn/Env/GR overlay toggles are treated as GLOBAL user view-prefs: set once, they
// follow the user on subsequent panel opens. Show() resets them to "all on"; these
// two helpers persist/restore them via ExtState (Live and A-B are deliberately NOT
// persisted - they are per-session action state). Saved on toggle, restored on open.
void SneakPeak::RestoreDynamicsViewPrefs()
{
  if (!g_GetExtState) return;
  auto rd = [&](const char* key, bool def) {
    const char* v = g_GetExtState("SneakPeak", key);
    return (v && v[0]) ? (v[0] != '0') : def;
  };
  m_dynamicsPanel.SetShowDyn(rd("dyn_show_dyn", true));
  // Env shares the EXISTING "show_vol_env" key (the waveform overlay, the right-click
  // "Show Volume Envelope" menu, and the startup restore all read/write it; rendering
  // syncs panel->waveform each frame). Using a separate key would give Env two
  // un-reconciled sources of truth that silently revert each other. Default true keeps
  // the prior "panel open -> env on" behaviour while honouring an explicit "0".
  m_dynamicsPanel.SetShowEnv(rd("show_vol_env", true));
  m_dynamicsPanel.SetShowGR (rd("dyn_show_gr",  true));
  // Live persists too (user request). Default OFF. A/B (bypass) stays ephemeral.
  m_dynamicsPanel.SetLiveMode(rd("dyn_live", false));
  // Meter-scale dB-floor selector (premium View tab). Stored as an index into
  // dynui::kMeterFloorOptDb. v2.3.0 prepended -96 (gate thresholds reach -90 now),
  // shifting the legacy indices by +1 - the pref moved to dyn_meter_floor2, with a
  // one-time migration from the old key (old 0/1/2 = -60/-36/-24 -> new 1/2/3).
  const char* mf2 = g_GetExtState("SneakPeak", "dyn_meter_floor2");
  const char* mf = g_GetExtState("SneakPeak", "dyn_meter_floor");
  m_dynamicsPanel.SetMeterFloor((mf2 && mf2[0]) ? atoi(mf2)
                              : (mf && mf[0])   ? atoi(mf) + 1
                                                : dynui::kMeterFloorDefaultSel);
  // Compact mode (premium View tab). Default off (normal layout).
  m_dynamicsPanel.SetCompact(rd("dyn_compact", false));
  // Panel size (free-resize scale) + position: reopen where/how the user left it.
  // Scale is stored x1000 as an integer (locale-independent); offsets are plain ints.
  // GetRect clamps to the window on use, so a stale offset can never go off-screen.
  const char* us = g_GetExtState("SneakPeak", "dyn_ui_scale");
  if (us && us[0]) m_dynamicsPanel.SetUiScale(atoi(us) / 1000.0);
#ifdef SNEAKPEAK_BLEND2D_PANEL
  // Reload coherence (v2.2.0): the panel draws at g_uiScale * grip, capped at
  // EFF_SCALE_MAX. If the persisted grip would pin the panel at the cap under the
  // current global scale, shrink the grip so resize travel stays live immediately.
  if (g_uiScale > 0.0 && g_uiScale * m_dynamicsPanel.GetUiScale() > DynamicsPanel::EFF_SCALE_MAX)
    m_dynamicsPanel.SetUiScale(DynamicsPanel::EFF_SCALE_MAX / g_uiScale);
#endif
  const char* ox = g_GetExtState("SneakPeak", "dyn_off_x");
  const char* oy = g_GetExtState("SneakPeak", "dyn_off_y");
  m_dynamicsPanel.SetPanelOffset(ox && ox[0] ? atoi(ox) : 0, oy && oy[0] ? atoi(oy) : 0);
  // If Live was restored ON, write the envelope NOW so it already reflects the dynamics
  // (otherwise it sits flat until the first param nudge). Runs only when Live is on, so
  // browsing items with Live off never modifies the project. Self-ensures the envelope
  // and opens its own undo block.
  if (m_dynamicsPanel.IsLive())
    ApplyDynamicsToEnvelope();
}

void SneakPeak::SaveDynamicsViewPrefs()
{
  if (!g_SetExtState) return;
  g_SetExtState("SneakPeak", "dyn_show_dyn", m_dynamicsPanel.GetShowDyn() ? "1" : "0", true);
  g_SetExtState("SneakPeak", "show_vol_env", m_dynamicsPanel.GetShowEnv() ? "1" : "0", true);  // shared with the menu/waveform/startup
  g_SetExtState("SneakPeak", "dyn_show_gr",  m_dynamicsPanel.GetShowGR()  ? "1" : "0", true);
  g_SetExtState("SneakPeak", "dyn_live",     m_dynamicsPanel.IsLive()     ? "1" : "0", true);
  char mf[8];
  snprintf(mf, sizeof(mf), "%d", m_dynamicsPanel.GetMeterFloor());
  g_SetExtState("SneakPeak", "dyn_meter_floor2", mf, true);  // new indexing since v2.3.0 (-96 at 0)
  g_SetExtState("SneakPeak", "dyn_compact", m_dynamicsPanel.GetCompact() ? "1" : "0", true);
}

// Persist the premium panel size (free-resize scale, stored x1000 as an integer so
// it's locale-independent) + position offsets. Called by the host after a resize or
// panel-drag completes (DynamicsPanel::GeomChanged()).
void SneakPeak::SaveDynamicsGeom()
{
  if (!g_SetExtState) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", (int)(m_dynamicsPanel.GetUiScale() * 1000.0 + 0.5));
  g_SetExtState("SneakPeak", "dyn_ui_scale", buf, true);
  snprintf(buf, sizeof(buf), "%d", m_dynamicsPanel.GetPanelOffsetX());
  g_SetExtState("SneakPeak", "dyn_off_x", buf, true);
  snprintf(buf, sizeof(buf), "%d", m_dynamicsPanel.GetPanelOffsetY());
  g_SetExtState("SneakPeak", "dyn_off_y", buf, true);
}

// --- Hard Limiter session params + geometry (v2.4.0 INC-L1) -----------------
// Same locale-safe encoding as the dynamics geometry: doubles stored x1000 as
// integers, bools as "1"/"0". Saved on drag release / pill click / editor
// commit / preset apply / panel close - session defaults, not per-item state.

void SneakPeak::SaveLimiterParams()
{
  if (!g_SetExtState) return;
  const LimiterParams& p = m_limiterPanel.GetParams();
  char buf[16];
  auto wr = [&](const char* key, double v) {
    snprintf(buf, sizeof(buf), "%d", (int)std::lround(v * 1000.0));
    g_SetExtState("SneakPeak", key, buf, true);
  };
  wr("lim_gain", p.gainDb);
  wr("lim_ceiling", p.ceilingDb);
  wr("lim_attack", p.attackMs);
  wr("lim_hold", p.holdMs);
  wr("lim_release", p.releaseMs);
  g_SetExtState("SneakPeak", "lim_truepeak", p.truePeak ? "1" : "0", true);
  g_SetExtState("SneakPeak", "lim_link", p.link ? "1" : "0", true);
}

void SneakPeak::RestoreLimiterParams()
{
  // Panel offsets restore regardless; params fall back to preset 0 on first run.
  if (g_GetExtState) {
    const char* ox = g_GetExtState("SneakPeak", "lim_off_x");
    const char* oy = g_GetExtState("SneakPeak", "lim_off_y");
    m_limiterPanel.SetPanelOffset(ox && ox[0] ? atoi(ox) : 0,
                                  oy && oy[0] ? atoi(oy) : 0);
    const char* g = g_GetExtState("SneakPeak", "lim_gain");
    if (g && g[0]) {
      auto rd = [&](const char* key, double def) {
        const char* v = g_GetExtState("SneakPeak", key);
        return (v && v[0]) ? (double)atoi(v) / 1000.0 : def;
      };
      auto rb = [&](const char* key, bool def) {
        const char* v = g_GetExtState("SneakPeak", key);
        return (v && v[0]) ? (v[0] != '0') : def;
      };
      LimiterParams p;
      p.gainDb = rd("lim_gain", 0.0);
      p.ceilingDb = rd("lim_ceiling", -1.0);
      p.attackMs = rd("lim_attack", 5.0);
      p.holdMs = rd("lim_hold", 10.0);
      p.releaseMs = rd("lim_release", 60.0);
      p.truePeak = rb("lim_truepeak", true);
      p.link = rb("lim_link", true);
      m_limiterPanel.SetParams(p);   // clamps each field to its knob range
      return;
    }
  }
  m_limiterPanel.ApplyPreset(0);     // first run: "Game Asset -1 dBTP"
  m_limiterPanel.ClearParamsChanged();
}

void SneakPeak::SaveLimiterGeom()
{
  if (!g_SetExtState) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", m_limiterPanel.GetPanelOffsetX());
  g_SetExtState("SneakPeak", "lim_off_x", buf, true);
  snprintf(buf, sizeof(buf), "%d", m_limiterPanel.GetPanelOffsetY());
  g_SetExtState("SneakPeak", "lim_off_y", buf, true);
}

// --- One-Shot Prep (v2.4 INC-B1 + B2) -----------------------------------------
// RUN = per slice: trim silence -> edge micro-fades -> normalize -> write WAV
// next to the source (naming pattern, collision appends _x). The loaded buffer
// is NEVER touched - this is an exporter. LUFS-I uses REAPER's own measurement
// (CalculateNormalization on a temp file), Peak is a plain scan, TP-safe runs
// the limiter engine at the target ceiling.

// Single-buffer eligibility (INC-B3/L2): the One-Shot Factory, Edit Copy and
// the Hard Limiter all operate on the single loaded buffer, so Standalone and
// plain ITEM mode qualify - SET/timeline/multi-item (segmented buffers) and
// master mode do not.
bool SneakPeak::SingleItemViewOk() const
{
  if (!m_waveform.HasItem() || m_masterMode) return false;
  if (m_waveform.IsStandaloneMode()) return true;
  return !m_waveform.IsMultiItem() && !m_workingSet.active;
}

// INC-PK1: ITEM mode qualifies only once the buffer is in (the limiter and the
// One-Shot preview read raw samples); 8g: Edit Copy and the One-Shot run stream
// the source and gate on SingleItemViewOk / RequireItemAudio instead.
bool SneakPeak::SingleBufferModeOk() const
{
  return SingleItemViewOk() &&
         (m_waveform.IsStandaloneMode() || m_waveform.GetAudioSampleCount() > 0);
}

// Kept bounds of one slice: the trim scan (any channel above the threshold
// keeps the frame) + keep-padding, clamped to the slice. Shared by the
// exporter and the live preview so what is drawn is exactly what Run writes.
// False = nothing above the threshold in this slice.
bool SneakPeak::OneShotTrimBounds(const OneShotParams& p, const double* data, int nch,
                                  int sr, int s0, int s1, int* a, int* b)
{
  if (s1 <= s0 || nch <= 0 || sr <= 0) return false;
  if (!p.trimEnable) {
    *a = s0;
    *b = s1;
    return true;
  }
  const double thr = pow(10.0, p.trimThreshDb / 20.0);
  int first = -1, last = -1;
  for (int i = s0; i < s1; i++) {
    bool hot = false;
    for (int c = 0; c < nch && !hot; c++)
      hot = fabs(data[(size_t)i * nch + c]) >= thr;
    if (hot) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (first < 0) return false;
  const int padFrames = (int)(p.trimPadMs * 0.001 * sr + 0.5);
  *a = std::max(s0, first - padFrames);
  *b = std::min(s1, last + 1 + padFrames);
  return true;
}

// Slice list for the active mode, sorted and non-overlapping. WHOLE = one
// slice spanning the file (byte-identical to the INC-B1 behavior).
std::vector<std::pair<int, int>> SneakPeak::OneShotBuildSlices(const OneShotParams& p)
{
  std::vector<std::pair<int, int>> out;
  const int frames = m_waveform.GetAudioSampleCount();
  const int sr = m_waveform.GetSampleRate();
  const int nch = m_waveform.GetNumChannels();
  if (frames <= 0 || sr <= 0 || nch <= 0) return out;

  if (p.sliceMode == 1) {   // By regions/markers: regions win, markers split
    if (!g_EnumProjectMarkers3) return out;
    const double itemPos = m_waveform.GetItemPosition();
    const double dur = (double)frames / sr;
    std::vector<int> marks;
    int idx = 0, mri = 0, color = 0;
    bool isrgn = false;
    double pos = 0.0, rgnend = 0.0;
    const char* name = nullptr;
    while (g_EnumProjectMarkers3(nullptr, idx, &isrgn, &pos, &rgnend, &name,
                                 &mri, &color)) {
      idx++;
      if (isrgn) {
        const double s = pos - itemPos, e = rgnend - itemPos;
        if (e <= 0.0 || s >= dur) continue;
        const int fs = std::max(0, (int)(s * sr + 0.5));
        const int fe = std::min(frames, (int)(e * sr + 0.5));
        if (fe > fs) out.push_back({ fs, fe });
      } else {
        const double t = pos - itemPos;
        if (t > 0.0 && t < dur) marks.push_back((int)(t * sr + 0.5));
      }
    }
    if (!out.empty()) {
      std::sort(out.begin(), out.end());
    } else if (!marks.empty()) {   // markers = split points -> N+1 spans
      std::sort(marks.begin(), marks.end());
      int prev = 0;
      for (int f : marks) {
        if (f > prev) out.push_back({ prev, f });
        prev = f;
      }
      if (prev < frames) out.push_back({ prev, frames });
    }
    return out;
  }

  if (p.sliceMode == 2) {   // By silence: gap > 150 ms below the threshold
    const auto& data = m_waveform.GetAudioData();
    const double thr = pow(10.0, p.trimThreshDb / 20.0);
    const int gapF = (int)(0.150 * sr + 0.5);
    const int minF = (int)(0.050 * sr + 0.5);   // min slice 50 ms
    int spanStart = -1, lastHot = -1;
    auto push = [&](int a, int b) {
      if (b - a >= minF) out.push_back({ a, b });
    };
    for (int i = 0; i < frames; i++) {
      bool hot = false;
      for (int c = 0; c < nch && !hot; c++)
        hot = fabs(data[(size_t)i * nch + c]) >= thr;
      if (!hot) continue;
      if (spanStart < 0) {
        spanStart = i;
      } else if (i - lastHot > gapF) {
        push(spanStart, lastHot + 1);
        spanStart = i;
      }
      lastHot = i;
    }
    if (spanStart >= 0) push(spanStart, lastHot + 1);
    // Give each slice its keep-padding here (the gaps are silence - padding
    // into them is the point), capped at the midpoint to the neighbour so
    // slices never overlap. The per-slice trim then reproduces these bounds.
    const int padF = p.trimEnable ? (int)(p.trimPadMs * 0.001 * sr + 0.5) : 0;
    for (size_t i = 0; i < out.size(); i++) {
      const int loCap = i == 0 ? 0 : (out[i - 1].second + out[i].first) / 2;
      const int hiCap = i + 1 == out.size()
                            ? frames
                            : (out[i].second + out[i + 1].first) / 2;
      out[i].first = std::max(loCap, out[i].first - padF);
      out[i].second = std::min(hiCap, out[i].second + padF);
    }
    return out;
  }

  out.push_back({ 0, frames });   // Whole file
  return out;
}

// {name} -> source basename. Counter tokens: {nn}/{n}/{nnn} (01-based) OR any
// digit token - {01} numbers from 01, {001} pads to three digits, {5} starts
// at 5 (users reach for {01} first; it must just work - user report
// 2026-07-02). Padding = the token's length, widened so the LAST file in the
// batch still sorts correctly (001..150, never 01..99,100). Unknown tokens
// keep their text ({test} -> "test") - literal braces never reach a filename.
static std::string ExpandOneShotPattern(const char* pat, const std::string& base,
                                        int idx1, int count)
{
  auto digitsOf = [](int v) {
    int d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
  };
  std::string out;
  const char* s = pat;
  while (*s) {
    if (*s != '{') {
      out += *s++;
      continue;
    }
    const char* e = strchr(s + 1, '}');
    if (!e) {   // stray '{': drop it, keep the rest
      s++;
      continue;
    }
    const std::string tok(s + 1, (size_t)(e - s - 1));
    s = e + 1;
    if (tok == "name") {
      out += base;
      continue;
    }
    bool allN = !tok.empty(), allDigit = !tok.empty();
    for (char c : tok) {
      if (c != 'n') allN = false;
      if (c < '0' || c > '9') allDigit = false;
    }
    char nn[16];
    if (allN && tok.size() <= 6) {
      const int w = std::min(9, std::max((int)tok.size(), digitsOf(count)));
      snprintf(nn, sizeof(nn), "%0*d", w, idx1);
      out += nn;
    } else if (allDigit && tok.size() <= 6) {
      const int start = atoi(tok.c_str());
      const int w = std::min(9, std::max((int)tok.size(), digitsOf(start + count - 1)));
      snprintf(nn, sizeof(nn), "%0*d", w, start + idx1 - 1);
      out += nn;
    } else {
      out += tok;   // unknown token: keep its text, braces never leak
    }
  }
  if (out.empty()) {
    char nn[16];
    snprintf(nn, sizeof(nn), "%0*d", std::min(9, std::max(2, digitsOf(count))), idx1);
    out = base + "_" + nn;
  }
  return out;
}

// One slice through the chain: trim -> fades -> normalize -> write. Returns
// 1 = written (note = "1.23 s, peak -0.3 dBFS"), 0 = skipped (note says why,
// toast-ready), -1 = abort the whole run (err says why).
int SneakPeak::OneShotExportSlice(const OneShotParams& p, int s0, int s1,
                                  const std::string& outPath, char* note,
                                  size_t noteSz, char* err, size_t errSz)
{
  note[0] = 0;
  err[0] = 0;

  // The slice is [s0, s1) of the WORKING buffer; the samples come from the
  // source at full rate (F11: that buffer is downsampled on long items), so
  // trim/fade/normalize below operate on the real audio.
  const double bufRate = (double)m_waveform.GetSampleRate();
  const double t0 = s0 / bufRate, t1 = s1 / bufRate;
  const int srcRate = m_waveform.IsStandaloneMode() ? m_waveform.GetSampleRate()
                                                    : m_waveform.GetSourceSampleRate();
  if (srcRate > 0 && (int64_t)((t1 - t0) * srcRate) > WaveformView::kMaxLoadFrames) {
    snprintf(note, noteSz, "Slice too long for One-Shot (over %d min)",
             WaveformView::kMaxLoadFrames / srcRate / 60);
    return 0;
  }
  std::vector<double> slice;
  int nch = 0, sr = 0;
  if (!SliceSamples(t0, t1, slice, &nch, &sr)) {
    snprintf(err, errSz, "Could not read the item audio - run aborted");
    return -1;
  }
  const int sliceFrames = (int)(slice.size() / (size_t)nch);

  int a = 0, b = 0;
  if (!OneShotTrimBounds(p, slice.data(), nch, sr, 0, sliceFrames, &a, &b)) {
    snprintf(note, noteSz, "Nothing above the trim threshold");
    return 0;
  }
  const int len = b - a;
  if (len < 16) {
    snprintf(note, noteSz, "Trimmed result too short");
    return 0;
  }

  std::vector<double> work(slice.begin() + (size_t)a * nch,
                           slice.begin() + (size_t)b * nch);
  slice.clear();
  slice.shrink_to_fit();

  // Edge micro-fades (linear in v1 - the click-killers).
  const int inF = std::min((int)(p.fadeInMs * 0.001 * sr + 0.5), len);
  const int outF = std::min((int)(p.fadeOutMs * 0.001 * sr + 0.5), len);
  if (inF > 0) AudioOps::FadeInShaped(work.data(), inF, nch, 0);
  if (outF > 0)
    AudioOps::FadeOutShaped(work.data() + (size_t)(len - outF) * nch, outF, nch, 0);

  // Normalize. PCM output (Standalone keeps the file's 16/24-bit format)
  // cannot hold anything above 0 dBFS: a LUFS-I gain that pushes a peak over
  // full scale is limited instead of clipped flat by the writer (A4.5).
  const bool pcmOut = m_waveform.IsStandaloneMode() && m_wavAudioFormat != 3;
  char normNote[48] = "";
  if (p.normMode == 1) {   // Peak dBFS
    double pk = 0.0;
    for (double v : work) pk = std::max(pk, fabs(v));
    if (pk > 1e-9) {
      const double g = pow(10.0, p.normTarget / 20.0) / pk;
      for (double& v : work) v *= g;
      snprintf(normNote, sizeof(normNote), ", peak %.1f dBFS", p.normTarget);
    }
  } else if (p.normMode == 2) {   // LUFS-I via REAPER's own measurement
    bool ok = false;
    if (g_PCM_Source_CreateFromFile && g_CalculateNormalization) {
      char tmpPath[512];
      snprintf(tmpPath, sizeof(tmpPath), "%s/sneakpeak_oneshot_%d.wav",
               AudioEngine::TempDir().c_str(), AudioEngine::ProcessId());
      if (AudioEngine::WriteWavFile(tmpPath, work.data(), len, nch, sr, 32, 3)) {
        if (PCM_source* src = g_PCM_Source_CreateFromFile(tmpPath)) {
          const double gainDb =
              g_CalculateNormalization(src, 0, p.normTarget, 0.0, 0.0);
          delete src;
          const double g = pow(10.0, gainDb / 20.0);
          if (g > 0.001 && g < 100.0) {
            for (double& v : work) v *= g;
            ok = true;
            if (pcmOut) {
              double pk = 0.0;
              for (double v : work) pk = std::max(pk, fabs(v));
              if (pk > 0.999) {
                LimiterParams lp;
                lp.ceilingDb = -0.1;
                LimiterProcess(work.data(), len, nch, sr, lp);
              }
            }
          }
        }
        AudioEngine::RemoveFile(tmpPath);
      }
    }
    if (!ok) {
      snprintf(err, errSz, "LUFS measurement failed - run aborted");
      return -1;
    }
    snprintf(normNote, sizeof(normNote), ", %.0f LUFS", p.normTarget);
  } else if (p.normMode == 3) {   // True-peak safe = the limiter engine
    LimiterParams lp;
    lp.ceilingDb = p.normTarget;
    LimiterResult r = LimiterProcess(work.data(), len, nch, sr, lp);
    if (!r.ok) {
      snprintf(err, errSz, "Limiter failed - run aborted");
      return -1;
    }
    snprintf(normNote, sizeof(normNote), ", %.1f dBTP safe", p.normTarget);
  }

  // Standalone keeps the loaded file's format; ITEM mode (INC-B3) writes
  // 32-bit float - m_wavBitsPerSample only tracks standalone loads, and float
  // is lossless for the in-memory doubles.
  const int outBits = m_waveform.IsStandaloneMode() ? m_wavBitsPerSample : 32;
  const int outFmt = m_waveform.IsStandaloneMode() ? m_wavAudioFormat : 3;
  if (!AudioEngine::WriteWavFile(outPath, work.data(), len, nch, sr,
                                 outBits, outFmt)) {
    snprintf(err, errSz, "Write failed - check the source folder permissions");
    return -1;
  }
  snprintf(note, noteSz, "%.2f s%s", (double)len / sr, normNote);
  return 1;
}

void SneakPeak::DoRunOneShot()
{
  if (!SingleItemViewOk() || !RequireItemAudio("One-Shot Factory")) return;   // slices = buffer frames
  const OneShotParams p = m_oneShotPanel.GetParams();
  if (p.sliceMode == 1 && m_waveform.IsStandaloneMode()) {   // A4.6
    ShowToast("REGIONS uses the project's regions - open the file as an item to slice by regions");
    return;
  }

  const std::vector<std::pair<int, int>> slices = OneShotBuildSlices(p);
  if (slices.empty()) {
    ShowToast(p.sliceMode == 1 ? "No regions or markers to slice by"
              : p.sliceMode == 2 ? "No audio above the threshold to slice"
                                 : "Nothing to export");
    return;
  }

  std::string dir, base;
  if (!OneShotSourceParts(&dir, &base)) {
    ShowToast("Item source has no file on disk - no output folder");
    return;
  }

  const int n = (int)slices.size();
  int written = 0, skipped = 0;
  char note[96] = "", err[96] = "", lastName[128] = "";
  for (int i = 0; i < n; i++) {
    if (n > 1) {   // STA-1-style progress in the title
      char title[128];
      snprintf(title, sizeof(title), "SneakPeak - One-Shot %d/%d...", i + 1, n);
      SetWindowText(m_hwnd, title);
    }
    const std::string name = ExpandOneShotPattern(p.pattern, base, i + 1, n);
    // Collision -> numbered suffix (name_2.wav, name_3.wav, ...): repeated
    // runs stay readable instead of growing _x chains (user report).
    std::string outPath = dir + "/" + name + ".wav";
    for (int suffix = 2;; suffix++) {
      FILE* probe = fopen(outPath.c_str(), "rb");
      if (!probe) break;   // free - use it
      fclose(probe);
      if (suffix > 99) {   // never overwrite; give up honestly
        outPath.clear();
        break;
      }
      char sfx[16];
      snprintf(sfx, sizeof(sfx), "_%d", suffix);
      outPath = dir + "/" + name + sfx + ".wav";
    }
    if (outPath.empty()) {
      snprintf(note, sizeof(note), "Output names exhausted");
      skipped++;
      continue;
    }
    const int r = OneShotExportSlice(p, slices[(size_t)i].first,
                                     slices[(size_t)i].second, outPath, note,
                                     sizeof(note), err, sizeof(err));
    if (r > 0) {
      written++;
      const size_t slash = outPath.find_last_of("/\\");
      snprintf(lastName, sizeof(lastName), "%s",
               slash == std::string::npos ? outPath.c_str()
                                          : outPath.c_str() + slash + 1);
    } else if (r == 0) {
      skipped++;
    } else {
      if (n > 1) UpdateTitle();
      ShowToast(err);
      return;
    }
  }
  if (n > 1) UpdateTitle();

  char buf[192];
  if (n == 1) {   // the INC-B1 single-file messages, preserved
    if (written == 1)
      snprintf(buf, sizeof(buf), "Written: %s (%s)", lastName, note);
    else
      snprintf(buf, sizeof(buf), "%s - no file written", note);
  } else if (skipped > 0) {
    snprintf(buf, sizeof(buf), "%d file%s written, %d slice%s skipped", written,
             written == 1 ? "" : "s", skipped, skipped == 1 ? "" : "s");
  } else {
    snprintf(buf, sizeof(buf), "%d file%s written", written,
             written == 1 ? "" : "s");
  }
  ShowToast(buf);
}

// Export destination parts: {name} = the source basename, dir = its folder.
// In ITEM mode (INC-B3) that is the item's media file - assets land next to
// it. False = the source has no file on disk (nothing to anchor the output).
bool SneakPeak::OneShotSourceParts(std::string* dir, std::string* base)
{
  const std::string srcPath =
      m_waveform.IsStandaloneMode()
          ? m_waveform.GetStandaloneFilePath()
          : AudioEngine::GetSourceFilePath(m_waveform.GetTake());
  if (srcPath.empty() && !m_waveform.IsStandaloneMode()) return false;
  *dir = ".";
  *base = "oneshot";
  const size_t slash = srcPath.find_last_of("/\\");
  std::string fname = slash == std::string::npos ? srcPath : srcPath.substr(slash + 1);
  if (slash != std::string::npos) *dir = srcPath.substr(0, slash);
  const size_t dot = fname.find_last_of('.');
  if (dot != std::string::npos && dot > 0) fname = fname.substr(0, dot);
  if (!fname.empty()) *base = fname;
  return true;
}

// OPEN FOLDER (user request 2026-07-02): reveal the export destination in the
// system file manager. ShellExecute "open" on a directory is the same proven
// cross-platform path the Support links use (SWELL maps it on macOS/Linux).
void SneakPeak::OpenOneShotFolder()
{
  if (!SingleItemViewOk()) return;
  std::string dir, base;
  if (!OneShotSourceParts(&dir, &base)) {
    ShowToast("Item source has no file on disk - no export folder");
    return;
  }
  ShellExecute(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// "Edit Copy in Standalone" (v2.4 INC-B4): the current ITEM's buffer written
// as {name}_edit.wav next to the item's media file, then opened as a new
// standalone tab - the one-command bridge from the timeline-first workflow
// into ALL the standalone-only tools (Loop Lab, Spectral Repair, Hard
// Limiter, destructive edits). The item itself is never modified; Replace
// Source in REAPER Timeline closes the round trip afterwards.
void SneakPeak::DoEditCopyStandalone()
{
  if (!SingleItemViewOk() || m_waveform.IsStandaloneMode()) return;   // streams (8e), no buffer
  if (m_exportPump.active) {
    ShowToast("Edit Copy already in progress");
    return;
  }
  if (DestructiveJobBusy()) return;   // F5: streams the file
  // The copy is written at the SOURCE rate through AudioStream (F11: the
  // working buffer is downsampled on long items). Standalone then loads it
  // fully into doubles, so refuse what would not fit the buffer cap.
  const int nch = m_waveform.GetNumChannels();
  const int sr = m_waveform.GetSourceSampleRate();
  const int64_t frames = (int64_t)(m_waveform.GetItemDuration() * sr + 0.5);
  if (frames <= 0 || nch <= 0 || sr <= 0) return;
  if (frames * nch * (int64_t)sizeof(double) > WaveformView::kMaxBufferBytes) {
    const int maxMin = (int)(WaveformView::kMaxBufferBytes / (nch * (int64_t)sizeof(double)) / sr / 60);
    char msg[128];
    snprintf(msg, sizeof(msg), "Item too long for Edit Copy (about %d min max at this rate)", maxMin);
    ShowToast(msg);
    return;
  }

  std::string dir, base;
  if (!OneShotSourceParts(&dir, &base)) {
    ShowToast("Item source has no file on disk - nowhere to put the copy");
    return;
  }
  // Numbered collision suffix, same policy as the Factory.
  std::string outPath = dir + "/" + base + "_edit.wav";
  for (int suffix = 2;; suffix++) {
    FILE* probe = fopen(outPath.c_str(), "rb");
    if (!probe) break;
    fclose(probe);
    if (suffix > 99) {
      ShowToast("Output names exhausted - clean up old _edit copies");
      return;
    }
    char sfx[24];
    snprintf(sfx, sizeof(sfx), "_edit_%d", suffix);
    outPath = dir + "/" + base + sfx + ".wav";
  }
  StartEditCopyExport(outPath);   // export_stream.cpp: OnTimer pump, tab on finish
}

// The pattern box opens REAPER's native input dialog - free-text editing in
// the accelerator-driven premium panel is not worth the cross-platform key
// handling risk (same call the marker edit dialog relies on).
void SneakPeak::EditOneShotPattern()
{
  if (!g_GetUserInputs) return;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", m_oneShotPanel.GetParams().pattern);
  // NOTE: captions_csv splits on commas - the caption must not contain any.
  if (!g_GetUserInputs("One-Shot naming", 1, "Pattern - tokens {name} {nn} {01}:",
                       buf, sizeof(buf)))
    return;
  m_oneShotPanel.SetPattern(buf);
  SaveOneShotParams();
  Invalidate();
}

// One-Shot session params (os_* keys, x1000-int encoding like lim_*).
void SneakPeak::SaveOneShotParams()
{
  if (!g_SetExtState) return;
  const OneShotParams& p = m_oneShotPanel.GetParams();
  char buf[16];
  auto wr = [&](const char* key, double v) {
    snprintf(buf, sizeof(buf), "%d", (int)std::lround(v * 1000.0));
    g_SetExtState("SneakPeak", key, buf, true);
  };
  wr("os_trim_thr", p.trimThreshDb);
  wr("os_pad", p.trimPadMs);
  wr("os_fade_in", p.fadeInMs);
  wr("os_fade_out", p.fadeOutMs);
  wr("os_target", p.normTarget);
  snprintf(buf, sizeof(buf), "%d", p.normMode);
  g_SetExtState("SneakPeak", "os_norm_mode", buf, true);
  g_SetExtState("SneakPeak", "os_trim", p.trimEnable ? "1" : "0", true);
  snprintf(buf, sizeof(buf), "%d", p.sliceMode);
  g_SetExtState("SneakPeak", "os_slice_mode", buf, true);
  g_SetExtState("SneakPeak", "os_pattern", p.pattern, true);
}

void SneakPeak::RestoreOneShotParams()
{
  if (!g_GetExtState) return;
  const char* ox = g_GetExtState("SneakPeak", "os_off_x");
  const char* oy = g_GetExtState("SneakPeak", "os_off_y");
  m_oneShotPanel.SetPanelOffset(ox && ox[0] ? atoi(ox) : 0,
                                oy && oy[0] ? atoi(oy) : 0);
  const char* probe = g_GetExtState("SneakPeak", "os_trim_thr");
  if (!probe || !probe[0]) return;   // first run: keep the plan defaults
  auto rd = [&](const char* key, double def) {
    const char* v = g_GetExtState("SneakPeak", key);
    return (v && v[0]) ? (double)atoi(v) / 1000.0 : def;
  };
  OneShotParams p;
  p.trimThreshDb = rd("os_trim_thr", -60.0);
  p.trimPadMs = rd("os_pad", 10.0);
  p.fadeInMs = rd("os_fade_in", 5.0);
  p.fadeOutMs = rd("os_fade_out", 20.0);
  p.normTarget = rd("os_target", -1.0);
  const char* nm = g_GetExtState("SneakPeak", "os_norm_mode");
  p.normMode = (nm && nm[0]) ? atoi(nm) : 3;
  const char* tr = g_GetExtState("SneakPeak", "os_trim");
  p.trimEnable = !(tr && tr[0] == '0');
  const char* sm = g_GetExtState("SneakPeak", "os_slice_mode");
  p.sliceMode = (sm && sm[0]) ? atoi(sm) : 0;
  const char* pt = g_GetExtState("SneakPeak", "os_pattern");
  if (pt && pt[0]) snprintf(p.pattern, sizeof(p.pattern), "%s", pt);
  m_oneShotPanel.SetParams(p);
}

void SneakPeak::SaveOneShotGeom()
{
  if (!g_SetExtState) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", m_oneShotPanel.GetPanelOffsetX());
  g_SetExtState("SneakPeak", "os_off_x", buf, true);
  snprintf(buf, sizeof(buf), "%d", m_oneShotPanel.GetPanelOffsetY());
  g_SetExtState("SneakPeak", "os_off_y", buf, true);
}

// Loop Lab session state (v2.4 INC-A5): weld crossfade ms + panel offsets.
void SneakPeak::SaveLoopLabParams()
{
  if (!g_SetExtState) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", m_loopLabPanel.GetWeldMs());
  g_SetExtState("SneakPeak", "loop_weld_ms", buf, true);
}

void SneakPeak::RestoreLoopLabParams()
{
  if (!g_GetExtState) return;
  const char* ox = g_GetExtState("SneakPeak", "loop_off_x");
  const char* oy = g_GetExtState("SneakPeak", "loop_off_y");
  m_loopLabPanel.SetPanelOffset(ox && ox[0] ? atoi(ox) : 0,
                                oy && oy[0] ? atoi(oy) : 0);
  const char* w = g_GetExtState("SneakPeak", "loop_weld_ms");
  if (w && w[0]) m_loopLabPanel.SetWeldMs(atoi(w));
}

void SneakPeak::SaveLoopLabGeom()
{
  if (!g_SetExtState) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", m_loopLabPanel.GetPanelOffsetX());
  g_SetExtState("SneakPeak", "loop_off_x", buf, true);
  snprintf(buf, sizeof(buf), "%d", m_loopLabPanel.GetPanelOffsetY());
  g_SetExtState("SneakPeak", "loop_off_y", buf, true);
}

// --- Limiter user presets (v2.4.0; same blob shape as the dynamics set) -----
// One ExtState blob "lim_user_presets": lines of "name\tparamsStr", params =
// LimiterParamsToString (locale-safe x1000 ints). Names sanitized of \t/\n.

std::vector<DynUserPreset> SneakPeak::LoadLimUserPresets()
{
  std::vector<DynUserPreset> out;
  if (!g_GetExtState) return out;
  const char* blob = g_GetExtState("SneakPeak", "lim_user_presets");
  if (!blob || !blob[0]) return out;
  std::string s(blob);
  size_t pos = 0;
  while (pos < s.size() && (int)out.size() < MAX_USER_PRESETS) {
    size_t nl = s.find('\n', pos);
    std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? s.size() : nl + 1;
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) continue;
    out.push_back({ line.substr(0, tab), line.substr(tab + 1) });
  }
  return out;
}

void SneakPeak::SaveLimUserPresets(const std::vector<DynUserPreset>& list)
{
  if (!g_SetExtState) return;
  std::string blob;
  for (const auto& p : list) {
    blob += p.name;  blob += '\t';  blob += p.params;  blob += '\n';
  }
  g_SetExtState("SneakPeak", "lim_user_presets", blob.c_str(), true);
}

void SneakPeak::AddLimUserPreset()
{
  if (!g_GetUserInputs) return;
  char name[128] = "My Preset";
  if (!g_GetUserInputs("Save Limiter Preset", 1, "Preset name:", name, sizeof(name)))
    return;  // cancelled
  std::string n(name);
  for (char& c : n) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  size_t a = n.find_first_not_of(' '), b = n.find_last_not_of(' ');
  if (a == std::string::npos) return;
  n = n.substr(a, b - a + 1);

  char params[128];
  LimiterParamsToString(m_limiterPanel.GetParams(), params, sizeof(params));

  auto list = LoadLimUserPresets();
  bool replaced = false;
  for (auto& p : list)
    if (p.name == n) { p.params = params; replaced = true; break; }  // overwrite by name
  if (!replaced) {
    if ((int)list.size() >= MAX_USER_PRESETS) {
      ShowToast("Preset list is full (32) - delete one first");
      return;
    }
    list.push_back({ n, params });
  }
  SaveLimUserPresets(list);
  m_limiterPanel.SetUserPresetName(n.c_str());   // the box shows what you saved
}

bool SneakPeak::ApplyLimUserPreset(int idx)
{
  auto list = LoadLimUserPresets();
  if (idx < 0 || idx >= (int)list.size()) return false;
  LimiterParams p;
  if (!LimiterParamsFromString(list[idx].params.c_str(), p)) return false;
  m_limiterPanel.SetParams(p);   // clamps to the knob ranges
  m_limiterPanel.SetUserPresetName(list[idx].name.c_str());
  return true;
}

void SneakPeak::DeleteLimUserPreset(int idx)
{
  auto list = LoadLimUserPresets();
  if (idx < 0 || idx >= (int)list.size()) return;
  list.erase(list.begin() + idx);
  SaveLimUserPresets(list);
}

// --- User dynamics presets (global, persisted in ExtState) ------------------
// Stored as one blob: each preset is "name\tparamsStr", presets separated by '\n'.
// Names are sanitized to contain neither '\t' nor '\n'; the params string is the
// DynamicsParamsToString() output (key=value pairs with spaces - no tab/newline).

std::vector<DynUserPreset> SneakPeak::LoadUserPresets()
{
  std::vector<DynUserPreset> out;
  if (!g_GetExtState) return out;
  const char* blob = g_GetExtState("SneakPeak", "dyn_user_presets");
  if (!blob || !blob[0]) return out;
  std::string s(blob);
  size_t pos = 0;
  while (pos < s.size() && (int)out.size() < MAX_USER_PRESETS) {
    size_t nl = s.find('\n', pos);
    std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? s.size() : nl + 1;
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) continue;
    out.push_back({ line.substr(0, tab), line.substr(tab + 1) });
  }
  return out;
}

void SneakPeak::SaveUserPresets(const std::vector<DynUserPreset>& list)
{
  if (!g_SetExtState) return;
  std::string blob;
  for (const auto& p : list) {
    blob += p.name;  blob += '\t';  blob += p.params;  blob += '\n';
  }
  g_SetExtState("SneakPeak", "dyn_user_presets", blob.c_str(), true);
}

void SneakPeak::AddUserPreset()
{
  if (!g_GetUserInputs) return;
  char name[128] = "My Preset";
  if (!g_GetUserInputs("Save Dynamics Preset", 1, "Preset name:", name, sizeof(name)))
    return;  // cancelled
  // Sanitize: drop the field/record delimiters and trim surrounding blanks.
  std::string n(name);
  for (char& c : n) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  size_t a = n.find_first_not_of(' '), b = n.find_last_not_of(' ');
  if (a == std::string::npos) return;            // empty / all blanks
  n = n.substr(a, b - a + 1);

  char params[256];
  DynamicsParamsToString(m_dynamicsPanel.GetParams(), params, sizeof(params));

  auto list = LoadUserPresets();
  bool replaced = false;
  for (auto& p : list)
    if (p.name == n) { p.params = params; replaced = true; break; }  // overwrite by name
  if (!replaced) {
    if ((int)list.size() >= MAX_USER_PRESETS) return;                 // cap reached
    list.push_back({ n, params });
  }
  SaveUserPresets(list);
}

bool SneakPeak::ApplyUserPreset(int idx)
{
  auto list = LoadUserPresets();
  if (idx < 0 || idx >= (int)list.size()) return false;
  DynamicsParams p;
  if (!DynamicsParamsFromString(list[idx].params.c_str(), p)) return false;
  m_dynamicsPanel.ApplyParams(p);
  return true;
}

void SneakPeak::DeleteUserPreset(int idx)
{
  auto list = LoadUserPresets();
  if (idx < 0 || idx >= (int)list.size()) return;
  list.erase(list.begin() + idx);
  SaveUserPresets(list);
}

// Run the canonical reanalysis ONCE right after the panel opens so the panel's
// avg GR (and therefore the auto-makeup curve baseline + GR meter) is correct from
// the first paint. Without this, Show() leaves m_avgGR at 0, the transfer curve is
// drawn with makeup=0, and the FIRST handle/knob drag triggers the reanalysis that
// makes the whole curve leap up by the (now non-zero) makeup - the on-grab jump the
// user reported. Mirrors the OnMouseMove path (input_handling.cpp ~1455-1467).
void SneakPeak::RefreshDynamicsAvgGr()
{
  m_dynamics.SetParams(m_dynamicsPanel.GetParams());
  RequestDynamicsAnalysis();   // Standalone: now; item views: with the result
}

