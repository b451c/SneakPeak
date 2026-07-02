// ============================================================================
// standalone_file.cpp — Standalone file management for SneakPeak
//
// Tab lifecycle (add/close/switch), file save/load/save-as,
// standalone preview playback, and pending fade baking.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "wav_smpl.h"
#include "audio_ops.h"
#include "theme.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#ifndef _WIN32
#include <unistd.h>  // access()
#include <pthread.h>
#else
#include <io.h>      // _access()
#include <commdlg.h> // GetSaveFileName
#define access(p, m) _access(p, m)
#define strcasecmp _stricmp
#ifndef F_OK
#define F_OK 0
#endif
#endif

// --- Standalone file mode ---

// MOVES the active buffer + undo/redo stacks into the tab entry (STA-2: a tab
// switch used to deep-copy gigabytes both ways on long files). INVARIANT: the
// caller replaces the active view right after - RestoreStandaloneState, a new
// tab install, or leaving standalone (LoadSelectedItem) - every call site is
// audited for this. While a tab is ACTIVE its entry holds no audio/stacks; the
// authoritative state lives in m_waveform + m_standalone*Stacks.
void SneakPeak::SaveCurrentStandaloneState()
{
  if (m_activeFileIdx < 0 || m_activeFileIdx >= (int)m_standaloneFiles.size()) return;
  if (!m_waveform.IsStandaloneMode()) return;

  auto& fs = m_standaloneFiles[m_activeFileIdx];
  fs.audioData = std::move(m_waveform.GetAudioData());
  fs.undoStack = std::move(m_standaloneUndoStack);
  fs.redoStack = std::move(m_standaloneRedoStack);
  fs.numChannels = m_waveform.GetNumChannels();
  fs.sampleRate = m_waveform.GetSampleRate();
  fs.audioSampleCount = m_waveform.GetAudioSampleCount();
  fs.bitsPerSample = m_wavBitsPerSample;
  fs.audioFormat = m_wavAudioFormat;
  fs.itemDuration = m_waveform.GetItemDuration();
  fs.cursorTime = m_waveform.GetCursorTime();
  fs.viewStartTime = m_waveform.GetViewStart();
  fs.viewDuration = m_waveform.GetViewDuration();
  fs.selection = m_waveform.GetSelection();
  fs.dirty = m_dirty;
  fs.fade = m_waveform.GetStandaloneFade();
  fs.loopStartFrame = m_waveform.GetLoopStart();
  fs.loopEndFrame = m_waveform.GetLoopEnd();
  fs.savedPath = m_savedPath;
  fs.overwriteConfirmed = m_overwriteConfirmed;

  DBG("[SneakPeak] Saved state for tab %d: %s\n", m_activeFileIdx, fs.filePath.c_str());
}

void SneakPeak::RestoreStandaloneState(int idx)
{
  if (idx < 0 || idx >= (int)m_standaloneFiles.size()) return;

  StandaloneCleanupPreview();

  auto& fs = m_standaloneFiles[idx];
  // MOVE the tab's audio into the waveform (SaveCurrentStandaloneState moves
  // it back on the next switch - see the invariant there). No copies.
  m_waveform.RestoreFromMemory(fs.filePath, std::move(fs.audioData),
                                fs.numChannels, fs.sampleRate, fs.audioSampleCount,
                                fs.bitsPerSample, fs.audioFormat, fs.itemDuration);
  fs.audioData.clear();
  m_waveform.SetViewStart(fs.viewStartTime);
  m_waveform.SetViewDuration(fs.viewDuration);
  m_waveform.SetCursorTime(fs.cursorTime);
  m_waveform.SetSelection(fs.selection);
  m_waveform.SetStandaloneFade(fs.fade);
  m_waveform.SetLoop(fs.loopStartFrame, fs.loopEndFrame);
  m_waveform.Invalidate();

  m_standaloneUndoStack = std::move(fs.undoStack);
  m_standaloneRedoStack = std::move(fs.redoStack);
  fs.undoStack.clear();
  fs.redoStack.clear();
  m_dirty = fs.dirty;
  m_hasUndo = !m_standaloneUndoStack.empty();
  m_wavBitsPerSample = fs.bitsPerSample;
  m_wavAudioFormat = fs.audioFormat;
  m_savedPath = fs.savedPath;
  m_overwriteConfirmed = fs.overwriteConfirmed;
  m_activeFileIdx = idx;

  m_gainPanel.ShowStandalone();
  m_spectral.ClearSpectrum();
  m_spectral.Invalidate();
  m_minimap.Invalidate();
  InvalidateLimiterPreview();   // the GR preview belongs to the previous buffer
  m_loopCandidates.clear();     // finder pins are transient proposals
  m_standaloneBufferSerial++;   // a pending background apply must not swap in here

  if (m_hwnd) {
    const char* fname = FileNameFromPath(fs.filePath.c_str());
    char title[512];
    snprintf(title, sizeof(title), "SneakPeak: %s%s", fs.dirty ? "*" : "", fname);
    SetWindowText(m_hwnd, title);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  DBG("[SneakPeak] Restored state for tab %d: %s\n", idx, fs.filePath.c_str());
}

void SneakPeak::OnModeBarCloseTab(int idx)
{
  if (idx < 0 || idx >= (int)m_standaloneFiles.size()) return;

  // Sync current dirty state to array before checking
  bool isActiveTab = (m_waveform.IsStandaloneMode() && idx == m_activeFileIdx);
  if (isActiveTab)
    m_standaloneFiles[idx].dirty = m_dirty;

  // Dirty check: Yes=save+close, No=close without saving, Cancel=abort
  if (m_standaloneFiles[idx].dirty) {
    int result = MessageBox(m_hwnd, "Save changes before closing?",
                            "SneakPeak", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (result == IDCANCEL) return;
    if (result == IDYES) {
      // Saving works on the ACTIVE state - switch to the tab first when it is
      // a background one (the old code silently skipped saving those).
      if (!isActiveTab) {
        if (m_waveform.IsStandaloneMode() && m_activeFileIdx >= 0)
          SaveCurrentStandaloneState();
        RestoreStandaloneState(idx);
        isActiveTab = true;
      }
      SaveStandaloneFile();
    }
  }

  bool wasActive = (m_waveform.IsStandaloneMode() && idx == m_activeFileIdx);
  m_standaloneFiles.erase(m_standaloneFiles.begin() + idx);

  // Adjust active index
  if (wasActive) {
    if (m_standaloneFiles.empty()) {
      m_activeFileIdx = -1;
      // Switch to REAPER mode or empty
      if (g_CountSelectedMediaItems && g_CountSelectedMediaItems(nullptr) > 0) {
        LoadSelectedItem();
      } else {
        m_waveform.ClearItem();
        m_dirty = false;
        UpdateTitle();
        if (m_hwnd) {
          InvalidateRect(m_hwnd, nullptr, FALSE);
        }
      }
    } else {
      int newIdx = (idx < (int)m_standaloneFiles.size()) ? idx : (int)m_standaloneFiles.size() - 1;
      RestoreStandaloneState(newIdx);
    }
  } else {
    // Adjust index if the removed tab was before the active one
    if (idx < m_activeFileIdx) m_activeFileIdx--;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::LoadStandaloneFile(const char* path)
{
  if (!path || !path[0]) return;
  StandaloneCleanupPreview();
  if (!m_previewTempPath.empty()) { remove(m_previewTempPath.c_str()); m_previewTempPath.clear(); }
  m_standaloneUndoStack.clear();
  m_standaloneRedoStack.clear();
  m_waveform.ClearStandaloneFade();
  m_waveform.ClearStandaloneGain();
  m_waveform.ClearLoop();

  std::string spath(path);
  DBG("[SneakPeak] LoadStandaloneFile: %s\n", path);

  if (!m_waveform.LoadFromFile(spath)) {
    MessageBox(m_hwnd, "Failed to load audio file.", "SneakPeak", MB_OK | MB_ICONERROR);
    return;
  }

  m_wavBitsPerSample = m_waveform.GetStandaloneBitsPerSample();
  m_wavAudioFormat = m_waveform.GetStandaloneAudioFormat();
  m_hasUndo = false;
  m_dirty = false;
  m_savedPath.clear();
  m_overwriteConfirmed = false;
  m_previewCacheDirty = true;

  {   // INC-A4: adopt an existing smpl sustain loop from the file
    int ls = -1, le = -1;
    if (ParseWavSmplFile(spath.c_str(), &ls, &le)) {
      const int fr = m_waveform.GetAudioSampleCount();
      if (ls >= 0 && le > ls && ls < fr) {
        m_waveform.SetLoop(ls, std::min(le, fr));
        ShowToast("Loop points loaded from file");
      }
    }
  }

  // Show gain panel in standalone mode
  m_gainPanel.ShowStandalone();

  // Clear spectral/minimap
  m_spectral.ClearSpectrum();
  m_spectral.Invalidate();
  m_minimap.Invalidate();

  if (m_hwnd) {
    UpdateTitle();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  DBG("[SneakPeak] Loaded standalone file: %s\n", path);
}

void SneakPeak::AddStandaloneFile(const char* path)
{
  if (!path || !path[0]) return;

  std::string spath(path);

  // Check for duplicate — activate existing tab
  for (int i = 0; i < (int)m_standaloneFiles.size(); i++) {
    if (m_standaloneFiles[i].filePath == spath) {
      if (m_waveform.IsStandaloneMode() && m_activeFileIdx == i) return; // already active
      if (m_waveform.IsStandaloneMode()) SaveCurrentStandaloneState();
      RestoreStandaloneState(i);
      return;
    }
  }

  // STA-1: decode incrementally. Long files stream in OnTimer slices with a
  // progress title while the CURRENT view keeps working; the new tab installs
  // at completion (FinishStandaloneLoad owns all bookkeeping). Small files
  // finish synchronously - identical feel to the old path.
  if (m_stdLoading) {
    ShowToast("Still loading the previous file...");
    return;
  }
  if (AudioEngine::BeginStream(spath, m_stdLoad)) {
    const double secs =
      (double)m_stdLoad.totalFrames / (double)std::max(1, m_stdLoad.info.sampleRate);
    if (secs > 20.0) {
      m_stdLoading = true;
      char toast[300];
      snprintf(toast, sizeof(toast), "Loading %s...", FileNameFromPath(path));
      ShowToast(toast);
      return; // StepStandaloneLoad (OnTimer) drives the rest
    }
    while (AudioEngine::ReadStreamStep(m_stdLoad, 0.050)) {}
    FinishStandaloneLoad();
    return;
  }

  // PCM_Source unavailable/unreadable: the old synchronous path (including the
  // direct-WAV fallback and its error box), with the original tab bookkeeping.
  if (m_waveform.IsStandaloneMode() && m_activeFileIdx >= 0)
    SaveCurrentStandaloneState();
  EvictStandaloneTabIfFull();
  LoadStandaloneFile(path);
  if (!m_waveform.IsStandaloneMode()) return; // load failed
  InstallStandaloneTab(spath);
}

// Oldest non-dirty tab (or plain oldest) makes room at MAX_STANDALONE_FILES.
void SneakPeak::EvictStandaloneTabIfFull()
{
  if ((int)m_standaloneFiles.size() < MAX_STANDALONE_FILES) return;
  int evictIdx = 0;
  for (int i = 0; i < (int)m_standaloneFiles.size(); i++) {
    if (!m_standaloneFiles[i].dirty) { evictIdx = i; break; }
  }
  m_standaloneFiles.erase(m_standaloneFiles.begin() + evictIdx);
  if (m_activeFileIdx > evictIdx) m_activeFileIdx--;
  else if (m_activeFileIdx == evictIdx) m_activeFileIdx = -1;
}

// Create + activate the tab entry for the audio currently in the waveform.
// Per the SaveCurrentStandaloneState invariant the ACTIVE tab's entry holds no
// audio/stacks (they live in the members), so only metadata is recorded here -
// no buffer copy at install.
void SneakPeak::InstallStandaloneTab(const std::string& spath)
{
  StandaloneFileState fs;
  fs.filePath = spath;
  fs.numChannels = m_waveform.GetNumChannels();
  fs.sampleRate = m_waveform.GetSampleRate();
  fs.audioSampleCount = m_waveform.GetAudioSampleCount();
  fs.bitsPerSample = m_wavBitsPerSample;
  fs.audioFormat = m_wavAudioFormat;
  fs.itemDuration = m_waveform.GetItemDuration();
  fs.cursorTime = 0.0;
  fs.viewStartTime = m_waveform.GetViewStart();
  fs.viewDuration = m_waveform.GetViewDuration();
  fs.selection = m_waveform.GetSelection();
  fs.dirty = false;

  m_standaloneFiles.push_back(std::move(fs));
  m_activeFileIdx = (int)m_standaloneFiles.size() - 1;

  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
  DBG("[SneakPeak] Added standalone tab %d: %s\n", m_activeFileIdx, spath.c_str());
}

// OnTimer slice of an in-flight incremental load (~20 ms of decode per tick;
// the message loop breathes between ticks, so the UI stays live).
void SneakPeak::StepStandaloneLoad()
{
  if (!m_stdLoading) return;
  if (AudioEngine::ReadStreamStep(m_stdLoad, 0.020)) {
    if (m_hwnd && m_stdLoad.totalFrames > 0) {
      char title[512];
      snprintf(title, sizeof(title), "SneakPeak: Loading %s... %d%%",
               FileNameFromPath(m_stdLoad.path.c_str()),
               (int)(100.0 * (double)m_stdLoad.framesRead / (double)m_stdLoad.totalFrames));
      SetWindowText(m_hwnd, title);
    }
    return;
  }
  FinishStandaloneLoad();
}

// Install a finished incremental load: everything the old synchronous path did
// (preview/undo cleanup, waveform install, standalone state reset) PLUS the tab
// bookkeeping - all deferred to completion so a failed/canceled load leaves the
// previous state untouched.
void SneakPeak::FinishStandaloneLoad()
{
  m_stdLoading = false;
  if (m_stdLoad.framesRead <= 0 || m_stdLoad.info.numChannels <= 0) {
    AudioEngine::AbortStream(m_stdLoad);
    MessageBox(m_hwnd, "Failed to load audio file.", "SneakPeak", MB_OK | MB_ICONERROR);
    if (m_hwnd) UpdateTitle();
    return;
  }

  if (m_waveform.IsStandaloneMode() && m_activeFileIdx >= 0)
    SaveCurrentStandaloneState();
  EvictStandaloneTabIfFull();

  StandaloneCleanupPreview();
  if (!m_previewTempPath.empty()) { remove(m_previewTempPath.c_str()); m_previewTempPath.clear(); }
  m_standaloneUndoStack.clear();
  m_standaloneRedoStack.clear();
  m_waveform.ClearStandaloneFade();
  m_waveform.ClearStandaloneGain();
  m_waveform.ClearLoop();

  const WavInfo info = m_stdLoad.info;
  const std::string spath = m_stdLoad.path;
  const double dur = (double)info.numFrames / (double)std::max(1, info.sampleRate);
  m_waveform.RestoreFromMemory(spath, std::move(m_stdLoad.samples),
                               info.numChannels, info.sampleRate, info.numFrames,
                               info.bitsPerSample, info.audioFormat, dur);
  m_waveform.SetViewStart(0.0);
  m_waveform.SetViewDuration(dur);
  m_waveform.SetCursorTime(0.0);
  m_waveform.ClearSelection();
  m_waveform.ClearLoop();
  AudioEngine::AbortStream(m_stdLoad); // samples were moved out; closes the rest

  m_wavBitsPerSample = info.bitsPerSample;
  m_wavAudioFormat = info.audioFormat;
  m_hasUndo = false;
  m_dirty = false;
  m_savedPath.clear();
  m_overwriteConfirmed = false;
  m_previewCacheDirty = true;

  m_gainPanel.ShowStandalone();
  m_spectral.ClearSpectrum();
  m_spectral.Invalidate();
  m_minimap.Invalidate();
  InvalidateLimiterPreview();   // the GR preview belongs to the previous buffer
  m_loopCandidates.clear();     // finder pins are transient proposals
  m_standaloneBufferSerial++;   // a pending background apply must not swap in here

  {   // INC-A4: adopt an existing smpl sustain loop from the file
    int ls = -1, le = -1;
    if (ParseWavSmplFile(spath.c_str(), &ls, &le)) {
      const int fr = m_waveform.GetAudioSampleCount();
      if (ls >= 0 && le > ls && ls < fr) {
        m_waveform.SetLoop(ls, std::min(le, fr));
        ShowToast("Loop points loaded from file");
      }
    }
  }

  InstallStandaloneTab(spath);
  if (m_hwnd) {
    UpdateTitle();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
  DBG("[SneakPeak] Incremental load installed: %s (%d frames)\n",
      spath.c_str(), info.numFrames);
}

// Generate a unique "_edit.wav" path from an original file path.
static std::string GenerateEditPath(const std::string& originalPath)
{
  auto dotPos = originalPath.find_last_of('.');
  std::string base = (dotPos != std::string::npos) ? originalPath.substr(0, dotPos) : originalPath;

  std::string candidate = base + "_edit.wav";
  if (access(candidate.c_str(), F_OK) != 0) return candidate;

  for (int i = 2; i < 100; i++) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "_edit_%d.wav", i);
    candidate = base + suffix;
    if (access(candidate.c_str(), F_OK) != 0) return candidate;
  }
  return base + "_edit.wav"; // fallback
}

static bool IsWavExtension(const std::string& path)
{
  auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos) return false;
  std::string ext = path.substr(dotPos + 1);
  for (auto& c : ext) c = (char)tolower((unsigned char)c);
  return (ext == "wav" || ext == "wave");
}

void SneakPeak::BakePendingFades()
{
  auto sf = m_waveform.GetStandaloneFade();
  if (sf.fadeInLen < 0.001 && sf.fadeOutLen < 0.001) return;

  StandaloneUndoSave();
  auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int totalFrames = m_waveform.GetAudioSampleCount();
  if (sf.fadeInLen >= 0.001) {
    int fadeFrames = std::min((int)(sf.fadeInLen * sr), totalFrames);
    if (fadeFrames > 0)
      AudioOps::FadeInShaped(data.data(), fadeFrames, nch, sf.fadeInShape);
  }
  if (sf.fadeOutLen >= 0.001) {
    int fadeFrames = std::min((int)(sf.fadeOutLen * sr), totalFrames);
    if (fadeFrames > 0) {
      int startFrame = totalFrames - fadeFrames;
      AudioOps::FadeOutShaped(data.data() + (size_t)startFrame * nch, fadeFrames, nch, sf.fadeOutShape);
    }
  }
  m_waveform.ClearStandaloneFade();
  m_waveform.Invalidate();
}

void SneakPeak::SaveStandaloneFile()
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;

  std::string origPath = m_waveform.GetStandaloneFilePath();
  if (origPath.empty()) return;

  std::string savePath;

  if (!m_savedPath.empty()) {
    // Already saved once — overwrite silently
    savePath = m_savedPath;
  } else if (IsWavExtension(origPath)) {
    // Original is WAV — confirm overwrite first time
    if (!m_overwriteConfirmed) {
      char msg[512];
      snprintf(msg, sizeof(msg), "Overwrite original file?\n%s",
               FileNameFromPath(origPath.c_str()));
      int result = MessageBox(m_hwnd, msg, "SneakPeak", MB_YESNO | MB_ICONQUESTION);
      if (result == IDYES) {
        m_overwriteConfirmed = true;
        savePath = origPath;
      } else {
        SaveStandaloneFileAs();
        return;
      }
    } else {
      savePath = origPath;
    }
  } else {
    // Non-WAV (MP3, FLAC, etc.) — auto-create _edit.wav
    savePath = GenerateEditPath(origPath);
    m_wavBitsPerSample = 24;
    m_wavAudioFormat = 1;
  }

  if (savePath.empty()) return;

  BakePendingFades();

  const auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();

  const bool wantLoop = m_writeLoopOnSave && m_waveform.HasLoop();
  if (AudioEngine::WriteWavFile(savePath, data.data(), frames, nch, sr,
                                m_wavBitsPerSample, m_wavAudioFormat,
                                wantLoop ? m_waveform.GetLoopStart() : -1,
                                wantLoop ? std::min(frames, m_waveform.GetLoopEnd()) : -1)) {
    DBG("[SneakPeak] Saved: %s\n", savePath.c_str());
    m_savedPath = savePath;
    m_dirty = false;
    UpdateTitle();
    if (m_activeFileIdx >= 0 && m_activeFileIdx < (int)m_standaloneFiles.size()) {
      m_standaloneFiles[m_activeFileIdx].dirty = false;
      m_standaloneFiles[m_activeFileIdx].savedPath = m_savedPath;
      m_standaloneFiles[m_activeFileIdx].overwriteConfirmed = m_overwriteConfirmed;
    }
    ShowToast("Saved!");
  } else {
    MessageBox(m_hwnd, "Failed to save file.", "SneakPeak", MB_OK | MB_ICONERROR);
  }
}

void SneakPeak::SaveStandaloneFileAs()
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;

  std::string origPath = m_waveform.GetStandaloneFilePath();

  // Determine initial directory and suggested filename
  std::string initialDir, initialFile;
  if (!m_savedPath.empty()) {
    auto lastSlash = m_savedPath.rfind('/');
    if (lastSlash != std::string::npos) {
      initialDir = m_savedPath.substr(0, lastSlash);
      initialFile = m_savedPath.substr(lastSlash + 1);
    }
  } else if (!origPath.empty()) {
    auto lastSlash = origPath.rfind('/');
    if (lastSlash != std::string::npos) {
      initialDir = origPath.substr(0, lastSlash);
      std::string baseName = origPath.substr(lastSlash + 1);
      auto dotPos = baseName.find_last_of('.');
      if (dotPos != std::string::npos) baseName.resize(dotPos);
      initialFile = baseName + "_edit.wav";
    }
  }

  char fn[1024] = {};
  if (!initialFile.empty())
    snprintf(fn, sizeof(fn), "%s", initialFile.c_str());

#ifdef _WIN32
  {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = "WAV files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = fn;
    ofn.nMaxFile = sizeof(fn);
    ofn.lpstrTitle = "Save WAV file";
    if (!initialDir.empty())
      ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "wav";
    if (!GetSaveFileNameA(&ofn))
      return; // user cancelled
  }
#else
  if (!BrowseForSaveFile("Save WAV file",
                          initialDir.empty() ? nullptr : initialDir.c_str(),
                          fn[0] ? fn : nullptr,
                          "WAV files\0*.wav\0All files\0*.*\0",
                          fn, sizeof(fn))) {
    return; // user cancelled
  }
#endif

  std::string savePath(fn);
  // Ensure .wav extension
  if (savePath.size() < 4 ||
      strcasecmp(savePath.c_str() + savePath.size() - 4, ".wav") != 0) {
    savePath += ".wav";
  }

  BakePendingFades();

  const auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();

  if (m_wavBitsPerSample < 24) {
    m_wavBitsPerSample = 24;
    m_wavAudioFormat = 1;
  }

  const bool wantLoop = m_writeLoopOnSave && m_waveform.HasLoop();
  if (AudioEngine::WriteWavFile(savePath, data.data(), frames, nch, sr,
                                m_wavBitsPerSample, m_wavAudioFormat,
                                wantLoop ? m_waveform.GetLoopStart() : -1,
                                wantLoop ? std::min(frames, m_waveform.GetLoopEnd()) : -1)) {
    DBG("[SneakPeak] Saved As: %s\n", savePath.c_str());
    m_savedPath = savePath;
    m_overwriteConfirmed = true; // user explicitly chose this path
    m_dirty = false;
    UpdateTitle();
    if (m_activeFileIdx >= 0 && m_activeFileIdx < (int)m_standaloneFiles.size()) {
      m_standaloneFiles[m_activeFileIdx].dirty = false;
      m_standaloneFiles[m_activeFileIdx].savedPath = m_savedPath;
      m_standaloneFiles[m_activeFileIdx].overwriteConfirmed = m_overwriteConfirmed;
    }
    ShowToast("Saved!");
  } else {
    MessageBox(m_hwnd, "Failed to save file.", "SneakPeak", MB_OK | MB_ICONERROR);
  }
}

void SneakPeak::StandaloneCleanupPreview()
{
  if (m_previewReg) {
    auto* reg = (preview_register_t*)m_previewReg;
    if (g_StartPreviewFade)
      g_StartPreviewFade(nullptr, reg, 0.050, 2); // 50ms fade-out
    if (g_StopPreview) g_StopPreview(reg);
#ifdef _WIN32
    DeleteCriticalSection(&reg->cs);
#else
    pthread_mutex_destroy(&reg->mutex);
#endif
    delete reg;
    m_previewReg = nullptr;
  }
  if (m_previewSrc) {
    delete m_previewSrc;
    m_previewSrc = nullptr;
  }
  // Keep temp file for cache (reused on next play if audio unchanged)
  m_previewActive = false;
  m_previewLoop = false;
  m_previewLoopOffset = 0.0;
}

// Write [startFrame, endFrame) of the current buffer to the preview temp WAV
// (non-destructive fades applied at their whole-file positions first). The
// rewrite is skipped when audio, fades AND the requested range are unchanged;
// a range change alone invalidates the cache (Loop Lab auditions a region).
bool SneakPeak::StandaloneWritePreviewFile(int startFrame, int endFrame)
{
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0 || nch <= 0 || sr <= 0) return false;
  startFrame = std::max(0, std::min(frames, startFrame));
  endFrame = std::max(startFrame, std::min(frames, endFrame));
  if (endFrame - startFrame <= 0) return false;

  if (!m_previewCacheDirty && !m_previewTempPath.empty() &&
      m_previewCacheStart == startFrame && m_previewCacheEnd == endFrame)
    return true;

  std::vector<double> previewData = m_waveform.GetAudioData(); // copy
  if (previewData.empty()) return false;

  // Apply pending non-destructive fade to preview copy
  auto sf = m_waveform.GetStandaloneFade();
  if (sf.fadeInLen >= 0.001) {
    int fadeFrames = std::min((int)(sf.fadeInLen * sr), frames);
    if (fadeFrames > 0)
      AudioOps::FadeInShaped(previewData.data(), fadeFrames, nch, sf.fadeInShape);
  }
  if (sf.fadeOutLen >= 0.001) {
    int fadeFrames = std::min((int)(sf.fadeOutLen * sr), frames);
    if (fadeFrames > 0) {
      int fs0 = frames - fadeFrames;
      AudioOps::FadeOutShaped(previewData.data() + (size_t)fs0 * nch, fadeFrames, nch, sf.fadeOutShape);
    }
  }

  // Slice the requested range (whole file = no-op)
  if (startFrame > 0 || endFrame < frames) {
    previewData.erase(previewData.begin() + (size_t)endFrame * nch, previewData.end());
    previewData.erase(previewData.begin(), previewData.begin() + (size_t)startFrame * nch);
  }

  // Clean up old temp file
  if (!m_previewTempPath.empty()) remove(m_previewTempPath.c_str());

  // Preview is temporary — always use temp dir (file deleted after playback)
  {
    const char* tmpDir = getenv("TMPDIR");
    if (!tmpDir) tmpDir = "/tmp";
    char tmpPath[512];
    snprintf(tmpPath, sizeof(tmpPath), "%s/sneakpeak_preview_%d.wav", tmpDir, (int)getpid());
    if (AudioEngine::WriteWavFile(tmpPath, previewData.data(), endFrame - startFrame,
                                  nch, sr, m_wavBitsPerSample, m_wavAudioFormat))
      m_previewTempPath = tmpPath;
    else
      m_previewTempPath.clear();
  }
  if (m_previewTempPath.empty()) return false;
  m_previewCacheDirty = false;
  m_previewCacheStart = startFrame;
  m_previewCacheEnd = endFrame;
  return true;
}

// Create the PCM source + preview register for the current temp WAV and start
// playback. loopFlag = REAPER's native gapless wrap over the whole source
// (Loop Lab writes just the region, so the wrap IS the loop). displayOffset
// maps the register's 0-based curpos back to absolute waveform time.
bool SneakPeak::StandaloneStartPreviewPlayback(double curpos, bool loopFlag,
                                               double displayOffset)
{
  PCM_source* src = g_PCM_Source_CreateFromFile(m_previewTempPath.c_str());
  if (!src) return false;

  auto* reg = new preview_register_t();
  memset(reg, 0, sizeof(*reg));
#ifdef _WIN32
  InitializeCriticalSection(&reg->cs);
#else
  pthread_mutex_init(&reg->mutex, nullptr);
#endif
  reg->src = src;
  reg->m_out_chan = 0;
  reg->loop = loopFlag;
  reg->volume = 1.0;
  reg->curpos = curpos;

  if (g_PlayPreview(reg)) {
    m_previewReg = reg;
    m_previewSrc = src;
    m_previewActive = true;
    m_previewLoop = loopFlag;
    m_previewLoopOffset = displayOffset;
    DBG("[SneakPeak] Standalone preview started at %.3f (loop=%d, src=%p, len=%.3f)\n",
        curpos, loopFlag ? 1 : 0, (void*)src, src->GetLength());
    return true;
  }
#ifdef _WIN32
  DeleteCriticalSection(&reg->cs);
#else
  pthread_mutex_destroy(&reg->mutex);
#endif
  delete reg;
  delete src;
  DBG("[SneakPeak] Standalone preview FAILED to start\n");
  return false;
}

void SneakPeak::StandalonePlayStop()
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;

  // If already playing, stop
  if (m_previewActive) {
    StandaloneCleanupPreview();
    return;
  }

  if (!g_PCM_Source_CreateFromFile || !g_PlayPreview) return;
  if (!StandaloneWritePreviewFile(0, m_waveform.GetAudioSampleCount())) return;

  // Start from selection start (if any), otherwise cursor position
  double startTime = 0.0;
  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    startTime = std::min(sel.startTime, sel.endTime);
  } else {
    startTime = m_waveform.GetCursorTime();
  }
  if (startTime < 0.0) startTime = 0.0;
  if (startTime >= m_waveform.GetItemDuration()) startTime = 0.0;

  StandaloneStartPreviewPlayback(startTime, false, 0.0);
}

// Loop Weld (v2.4 INC-A3): bake an equal-power crossfade over the seam - the
// last L frames of the loop blend into the material that PRECEDES the start,
// so the wrap becomes continuous by construction. Length-preserving, bounded
// range undo; the DSP lives in loop_finder.cpp (WeldLoopSeam, offline-tested).
void SneakPeak::DoWeldLoop(double crossfadeMs)
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;
  if (!m_waveform.HasLoop()) return;
  auto& data = m_waveform.GetAudioData();
  const int nch = m_waveform.GetNumChannels();
  const int sr = m_waveform.GetSampleRate();
  const int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0 || nch <= 0 || sr <= 0) return;

  const int s = std::max(0, std::min(frames, m_waveform.GetLoopStart()));
  const int e = std::max(s, std::min(frames, m_waveform.GetLoopEnd()));
  int L = (int)(crossfadeMs * 0.001 * sr + 0.5);
  L = std::max(8, std::min(L, e - s));   // weld cannot exceed the loop itself
  if (s < L) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "Weld needs %.0f ms of audio before the loop start", crossfadeMs);
    ShowToast(buf);
    return;
  }

  StandaloneUndoSaveRange(e - L, L);
  if (!WeldLoopSeam(data.data(), frames, nch, s, e, L)) {
    if (!m_standaloneUndoStack.empty()) m_standaloneUndoStack.pop_back();
    m_hasUndo = !m_standaloneUndoStack.empty();
    ShowToast("Weld failed: invalid loop region");
    return;
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "Loop welded (%.0f ms crossfade)",
           (double)L * 1000.0 / sr);
  ShowToast(buf);

  m_dirty = true;
  m_previewCacheDirty = true;
  UpdateTitle();
  m_waveform.Invalidate();
  m_minimap.Invalidate();
  m_spectral.ClearSpectrum();
  InvalidateLimiterPreview();
  // A running audition replays the welded seam right away.
  if (m_previewActive && m_previewLoop) {
    StandaloneCleanupPreview();
    StandaloneAuditionLoop();
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Loop Lab finder (v2.4 INC-A2): score loop-point candidates on a worker
// (NCC of the pre-end vs pre-start windows + spectral tie-break; the module
// doc in loop_finder.h has the math). Results land as numbered pins.
void SneakPeak::StartLoopFind()
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;
  if (m_loopFindBusy.load()) {
    ShowToast("Loop finder is already running...");
    return;
  }
  const int frames = m_waveform.GetAudioSampleCount();
  const int nch = m_waveform.GetNumChannels();
  const int sr = m_waveform.GetSampleRate();
  if (frames <= 0 || nch <= 0 || sr <= 0) return;
  if (frames < sr + sr / 5) {   // finder needs at least ~1.2 s of material
    ShowToast("File too short for loop finding");
    return;
  }
  if (m_loopFindThread.joinable()) m_loopFindThread.join();
  m_loopFindDone.store(false);
  m_loopFindBusy.store(true);
  ShowToast("Finding loop points...");
  std::vector<double> copy = m_waveform.GetAudioData();
  m_loopFindThread = std::thread(&SneakPeak::LoopFindThread, this,
                                 std::move(copy), frames, nch, sr,
                                 m_standaloneBufferSerial.load());
}

void SneakPeak::LoopFindThread(std::vector<double> audio, int frames, int nch,
                               int sr, uint64_t serial)
{
  std::vector<LoopCandidate> found =
      FindLoopCandidates(audio.data(), frames, nch, sr, 5);
  m_loopFindResult = std::move(found);   // read on the main thread after Done
  m_loopFindSerial = serial;
  m_loopFindDone.store(true);            // busy clears in the tick
}

// OnTimer: publish finished finder results (worker cannot touch the UI).
void SneakPeak::LoopFindTick()
{
  if (!m_loopFindBusy.load() || !m_loopFindDone.load()) return;
  m_loopFindDone.store(false);
  if (m_loopFindThread.joinable()) m_loopFindThread.join();
  m_loopFindBusy.store(false);
  if (m_loopFindSerial != m_standaloneBufferSerial.load() ||
      !m_waveform.IsStandaloneMode()) {
    ShowToast("Loop finder result discarded - audio changed");
    return;
  }
  m_loopCandidates = std::move(m_loopFindResult);
  m_loopFindResult.clear();
  if (m_loopCandidates.empty()) {
    ShowToast("No clean loop points found (NCC floor 0.5)");
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d loop candidate%s - click a pin",
             (int)m_loopCandidates.size(),
             m_loopCandidates.size() == 1 ? "" : "s");
    ShowToast(buf);
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Loop Lab (v2.4 INC-A1): gapless audition of the loop region. The temp WAV
// holds JUST the region and the preview register loops it natively - the
// wrap is seamless by construction. Toggle semantics like StandalonePlayStop.
void SneakPeak::StandaloneAuditionLoop()
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;

  if (m_previewActive && m_previewLoop) {   // already auditioning: stop
    StandaloneCleanupPreview();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (!m_waveform.HasLoop()) return;
  if (!g_PCM_Source_CreateFromFile || !g_PlayPreview) return;
  if (m_previewActive) StandaloneCleanupPreview();   // switch normal -> loop

  const int frames = m_waveform.GetAudioSampleCount();
  const int sr = m_waveform.GetSampleRate();
  if (frames <= 0 || sr <= 0) return;
  const int s = std::max(0, std::min(frames, m_waveform.GetLoopStart()));
  const int e = std::max(s, std::min(frames, m_waveform.GetLoopEnd()));
  if (e - s < 64) {
    ShowToast("Loop region too short to audition");
    return;
  }

  if (!StandaloneWritePreviewFile(s, e)) return;
  StandaloneStartPreviewPlayback(0.0, true, (double)s / (double)sr);
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Replace Source in REAPER Timeline ---

int SneakPeak::ReplaceSourceInTimeline(const std::string& oldPath, const std::string& newPath)
{
  if (oldPath.empty() || newPath.empty()) return 0;
  if (!g_CountTracks || !g_GetTrack || !g_GetTrackNumMediaItems || !g_GetTrackMediaItem ||
      !g_GetMediaItemNumTakes || !g_GetMediaItemTake || !g_GetMediaItemTake_Source ||
      !g_GetMediaSourceFileName || !g_PCM_Source_CreateFromFile || !g_GetSetMediaItemTakeInfo)
    return 0;

  std::vector<MediaItem*> touchedItems;
  if (g_PreventUIRefresh) g_PreventUIRefresh(1);
  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  int trackCount = g_CountTracks(nullptr);
  char pathBuf[4096];
  for (int t = 0; t < trackCount; t++) {
    MediaTrack* track = g_GetTrack(nullptr, t);
    if (!track) continue;
    int itemCount = g_GetTrackNumMediaItems(track);
    for (int i = 0; i < itemCount; i++) {
      MediaItem* item = g_GetTrackMediaItem(track, i);
      if (!item) continue;
      int takeCount = g_GetMediaItemNumTakes(item);
      bool itemTouched = false;
      for (int k = 0; k < takeCount; k++) {
        MediaItem_Take* take = g_GetMediaItemTake(item, k);
        if (!take) continue;
        PCM_source* src = g_GetMediaItemTake_Source(take);
        if (!src) continue;
        pathBuf[0] = 0;
        g_GetMediaSourceFileName(src, pathBuf, sizeof(pathBuf));
        if (strcasecmp(pathBuf, oldPath.c_str()) != 0) continue;
        PCM_source* newSrc = g_PCM_Source_CreateFromFile(newPath.c_str());
        if (!newSrc) continue;
        // P_SOURCE with set transfers ownership; REAPER destroys the old source
        g_GetSetMediaItemTakeInfo(take, "P_SOURCE", newSrc);
        itemTouched = true;
      }
      if (itemTouched) touchedItems.push_back(item);
    }
  }

  // Notify REAPER per item so peak cache invalidates and arrange redraws immediately
  // (without this, waveforms stay stale until REAPER regains window focus)
  if (g_UpdateItemInProject)
    for (MediaItem* it : touchedItems) g_UpdateItemInProject(it);

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Replace source in timeline", -1);
  if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

  if (!touchedItems.empty() && g_CountSelectedMediaItems && g_GetSelectedMediaItem &&
      g_SetMediaItemSelected && g_Main_OnCommand) {
    // Force peak rebuild on touched items: swap selection to them, trigger action, restore
    std::vector<MediaItem*> savedSel;
    int prevCount = g_CountSelectedMediaItems(nullptr);
    savedSel.reserve((size_t)prevCount);
    for (int i = 0; i < prevCount; i++)
      if (MediaItem* s = g_GetSelectedMediaItem(nullptr, i)) savedSel.push_back(s);
    for (MediaItem* s : savedSel) g_SetMediaItemSelected(s, false);
    for (MediaItem* it : touchedItems) g_SetMediaItemSelected(it, true);
    g_Main_OnCommand(40047, 0);  // Item: Build any missing peaks for selected items
    for (MediaItem* it : touchedItems) g_SetMediaItemSelected(it, false);
    for (MediaItem* s : savedSel) g_SetMediaItemSelected(s, true);
  }

  if (g_UpdateArrange) g_UpdateArrange();
  if (g_UpdateTimeline) g_UpdateTimeline();
  return (int)touchedItems.size();
}

void SneakPeak::DoReplaceSourceInTimeline()
{
  if (!m_waveform.IsStandaloneMode() || !m_waveform.HasItem()) return;

  std::string origPath = m_waveform.GetStandaloneFilePath();
  if (origPath.empty()) {
    ShowToast("No source path to replace");
    return;
  }

  // Save any pending edits first - SaveStandaloneFile handles overwrite prompt / Save As for non-WAV.
  // If user cancels the save dialog, m_savedPath stays empty and we abort the swap.
  if (m_dirty || m_savedPath.empty()) {
    SaveStandaloneFile();
    if (m_savedPath.empty()) return;  // user cancelled
  }

  int count = ReplaceSourceInTimeline(origPath, m_savedPath);
  char buf[128];
  if (count > 0) snprintf(buf, sizeof(buf), "Replaced %d item%s in timeline", count, count == 1 ? "" : "s");
  else snprintf(buf, sizeof(buf), "No items in timeline reference this file");
  ShowToast(buf);
}
