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
    // Reload item to reflect undo changes
    m_waveform.ClearItem();
    LoadSelectedItem();
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
    double s = m_waveform.RelTimeToAbsTime(std::min(sel.startTime, sel.endTime));
    double e = m_waveform.RelTimeToAbsTime(std::max(sel.startTime, sel.endTime));
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

  // Internal clipboard
  s_clipboard.numChannels = nch;
  s_clipboard.sampleRate = m_waveform.GetSampleRate();
  s_clipboard.numFrames = selFrames;
  s_clipboard.samples.resize((size_t)selFrames * nch);

  const auto& data = m_waveform.GetAudioData();
  size_t srcOffset = (size_t)startF * nch;
  size_t copyLen = (size_t)selFrames * nch;
  if (srcOffset + copyLen > data.size()) return;
  std::copy(data.begin() + (long)srcOffset,
            data.begin() + (long)(srcOffset + copyLen),
            s_clipboard.samples.begin());

  // Also trigger REAPER's native copy (40060 = Copy selected area of items)
  if (g_Main_OnCommand) g_Main_OnCommand(40060, 0);

  DBG("[SneakPeak] Copied %d frames to clipboard\n", selFrames);
}

void SneakPeak::DoCut()
{
  // Non-destructive: copy to clipboard, then delete via split+remove
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;

  DoCopy();
  DoDelete();
}

void SneakPeak::DoPaste()
{
  if (!m_waveform.HasItem() || s_clipboard.numFrames <= 0) return;
  if (s_clipboard.numChannels != m_waveform.GetNumChannels()) return;
  if (m_waveform.IsMultiItem()) {
    MessageBox(m_hwnd, "Paste is not supported in multi-item view.", "SneakPeak", MB_OK);
    return;
  }

  int ret = MessageBox(m_hwnd,
    "Paste modifies the audio file on disk. Continue?",
    "SneakPeak — Destructive Operation", MB_YESNO | MB_ICONWARNING);
  if (ret != IDYES) return;

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

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoDelete()
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;

  // Standalone: remove selected samples from audio data
  if (m_waveform.IsStandaloneMode()) {
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
    return;
  }

  // Non-destructive: split item at selection edges, delete middle piece
  if (!g_SplitMediaItem || !g_DeleteTrackMediaItem || !g_GetMediaItem_Track) return;

  MediaItem* item = m_waveform.GetItem();
  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  double splitStart = m_waveform.RelTimeToAbsTime(selStart);
  double splitEnd = m_waveform.RelTimeToAbsTime(selEnd);

  // Split at end first (so item pointer stays valid for the first part)
  MediaItem* rightPart = g_SplitMediaItem(item, splitEnd);
  // Split at start — item becomes left part, middlePart is the selection
  MediaItem* middlePart = g_SplitMediaItem(item, splitStart);

  // Delete the middle part
  if (middlePart) {
    MediaTrack* track = g_GetMediaItem_Track(middlePart);
    if (track) g_DeleteTrackMediaItem(track, middlePart);
  }

  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Delete (non-destructive)", -1);

  // Reload — item pointer may have changed, re-select
  if (rightPart) {
    // Focus on the right part (or left part if it exists)
    m_waveform.ClearItem();
    LoadSelectedItem();
  }

  m_waveform.ClearSelection();
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

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Normalize (non-destructive)", -1);

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
  if (!g_SetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double fadeLen;

  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    fadeLen = fabs(sel.endTime - sel.startTime);
  } else {
    fadeLen = m_waveform.GetItemDuration(); // fade entire item
  }

  if (fadeLen < 0.001) return;

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_FADEINLEN", fadeLen);
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Fade In", -1);

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
  if (!g_SetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double fadeLen;

  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    fadeLen = fabs(sel.endTime - sel.startTime);
  } else {
    fadeLen = m_waveform.GetItemDuration();
  }

  if (fadeLen < 0.001) return;

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_FADEOUTLEN", fadeLen);
  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Fade Out", -1);

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

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DoGain(double factor)
{
  if (!m_waveform.HasItem()) return;

  if (m_waveform.IsStandaloneMode()) {
    StandaloneUndoSave();
    auto& data = m_waveform.GetAudioData();
    int nch = m_waveform.GetNumChannels();
    int frames = m_waveform.GetAudioSampleCount();
    if (frames > 0 && nch > 0)
      AudioOps::Gain(data.data(), frames, nch, factor);
    m_dirty = true;
    UpdateTitle();
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Non-destructive: multiply current D_VOL by factor
  if (!g_SetMediaItemInfo_Value || !g_GetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double curVol = g_GetMediaItemInfo_Value(item, "D_VOL");
  double newVol = curVol * factor;

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
  if (g_UpdateArrange) g_UpdateArrange();

  char desc[64];
  snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB", 20.0 * log10(factor));
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);

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

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
  if (g_UpdateArrange) g_UpdateArrange();
  char desc[64];
  snprintf(desc, sizeof(desc), "SneakPeak: Normalize to %.0f LUFS", targetLufs);
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

