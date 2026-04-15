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
#define access(p, m) _access(p, m)
#ifndef F_OK
#define F_OK 0
#endif
#endif

// --- Standalone file mode ---

void SneakPeak::SaveCurrentStandaloneState()
{
  if (m_activeFileIdx < 0 || m_activeFileIdx >= (int)m_standaloneFiles.size()) return;
  if (!m_waveform.IsStandaloneMode()) return;

  auto& fs = m_standaloneFiles[m_activeFileIdx];
  fs.audioData = m_waveform.GetAudioData(); // copy
  fs.undoStack = m_standaloneUndoStack;
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
  fs.savedPath = m_savedPath;
  fs.overwriteConfirmed = m_overwriteConfirmed;

  DBG("[SneakPeak] Saved state for tab %d: %s\n", m_activeFileIdx, fs.filePath.c_str());
}

void SneakPeak::RestoreStandaloneState(int idx)
{
  if (idx < 0 || idx >= (int)m_standaloneFiles.size()) return;

  StandaloneCleanupPreview();

  auto& fs = m_standaloneFiles[idx];
  // Move audio data into waveform (we'll copy it back on save)
  std::vector<double> audioCopy = fs.audioData;
  m_waveform.RestoreFromMemory(fs.filePath, std::move(audioCopy),
                                fs.numChannels, fs.sampleRate, fs.audioSampleCount,
                                fs.bitsPerSample, fs.audioFormat, fs.itemDuration);
  m_waveform.SetViewStart(fs.viewStartTime);
  m_waveform.SetViewDuration(fs.viewDuration);
  m_waveform.SetCursorTime(fs.cursorTime);
  m_waveform.SetSelection(fs.selection);
  m_waveform.SetStandaloneFade(fs.fade);
  m_waveform.Invalidate();

  m_standaloneUndoStack = fs.undoStack;
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
      // Save if this is the active tab
      if (isActiveTab) SaveStandaloneFile();
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
  m_waveform.ClearStandaloneFade();
  m_waveform.ClearStandaloneGain();

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

  // Save current standalone state before switching
  if (m_waveform.IsStandaloneMode() && m_activeFileIdx >= 0) {
    SaveCurrentStandaloneState();
  }

  // Evict if at max
  if ((int)m_standaloneFiles.size() >= MAX_STANDALONE_FILES) {
    // Find oldest non-dirty, or oldest if all dirty
    int evictIdx = 0;
    for (int i = 0; i < (int)m_standaloneFiles.size(); i++) {
      if (!m_standaloneFiles[i].dirty) { evictIdx = i; break; }
    }
    m_standaloneFiles.erase(m_standaloneFiles.begin() + evictIdx);
    if (m_activeFileIdx > evictIdx) m_activeFileIdx--;
    else if (m_activeFileIdx == evictIdx) m_activeFileIdx = -1;
  }

  // Load the file via existing logic
  LoadStandaloneFile(path);
  if (!m_waveform.IsStandaloneMode()) return; // load failed

  // Create state entry
  StandaloneFileState fs;
  fs.filePath = spath;
  fs.audioData = m_waveform.GetAudioData();
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
  DBG("[SneakPeak] Added standalone tab %d: %s\n", m_activeFileIdx, path);
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

  if (AudioEngine::WriteWavFile(savePath, data.data(), frames, nch, sr,
                                m_wavBitsPerSample, m_wavAudioFormat)) {
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

  if (!BrowseForSaveFile("Save WAV file",
                          initialDir.empty() ? nullptr : initialDir.c_str(),
                          fn[0] ? fn : nullptr,
                          "WAV files\0*.wav\0All files\0*.*\0",
                          fn, sizeof(fn))) {
    return; // user cancelled
  }

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

  if (AudioEngine::WriteWavFile(savePath, data.data(), frames, nch, sr,
                                m_wavBitsPerSample, m_wavAudioFormat)) {
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

  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();
  if (frames <= 0) return;

  // Only rewrite temp WAV if audio/fade changed since last write
  if (m_previewCacheDirty || m_previewTempPath.empty()) {
    std::vector<double> previewData = m_waveform.GetAudioData(); // copy
    if (previewData.empty()) return;

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
        int startFrame = frames - fadeFrames;
        AudioOps::FadeOutShaped(previewData.data() + (size_t)startFrame * nch, fadeFrames, nch, sf.fadeOutShape);
      }
    }

    // Clean up old temp file
    if (!m_previewTempPath.empty()) remove(m_previewTempPath.c_str());

    // Preview is temporary — always use temp dir (file deleted after playback)
    {
      const char* tmpDir = getenv("TMPDIR");
      if (!tmpDir) tmpDir = "/tmp";
      char tmpPath[512];
      snprintf(tmpPath, sizeof(tmpPath), "%s/sneakpeak_preview_%d.wav", tmpDir, (int)getpid());
      if (AudioEngine::WriteWavFile(tmpPath, previewData.data(), frames, nch, sr,
                                     m_wavBitsPerSample, m_wavAudioFormat))
        m_previewTempPath = tmpPath;
      else
        m_previewTempPath.clear();
    }
    if (m_previewTempPath.empty()) return;
    m_previewCacheDirty = false;
  }

  PCM_source* src = g_PCM_Source_CreateFromFile(m_previewTempPath.c_str());
  if (!src) return;

  auto* reg = new preview_register_t();
  memset(reg, 0, sizeof(*reg));
#ifdef _WIN32
  InitializeCriticalSection(&reg->cs);
#else
  pthread_mutex_init(&reg->mutex, nullptr);
#endif
  reg->src = src;
  reg->m_out_chan = 0;
  reg->loop = false;
  reg->volume = 1.0;

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
  reg->curpos = startTime;

  if (g_PlayPreview(reg)) {
    m_previewReg = reg;
    m_previewSrc = src;
    m_previewActive = true;
    DBG("[SneakPeak] Standalone preview started at %.3f (src=%p, nch=%d, sr=%.0f, len=%.3f)\n",
        startTime, (void*)src, src->GetNumChannels(), src->GetSampleRate(), src->GetLength());
  } else {
#ifdef _WIN32
    DeleteCriticalSection(&reg->cs);
#else
    pthread_mutex_destroy(&reg->mutex);
#endif
    delete reg;
    delete src;
    DBG("[SneakPeak] Standalone preview FAILED to start\n");
  }
}
