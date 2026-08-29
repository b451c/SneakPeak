// ============================================================================
// convert_job.cpp — Convert & go (v2.5, UX audit follow-up 2026-08-29)
//
// A destructive ITEM edit works in place on the samples of a WAV; a compressed
// source (MP3, FLAC, AAC, ...) has no samples to rewrite. Instead of refusing,
// the edit asks ONE question and then: decodes the whole source to a WAV next
// to it in OnTimer slices (~15 ms per tick, title progress, Esc cancels -
// the WAV never exists on a cancel), points every take that used the file at
// the WAV (ReplaceSourceInTimeline: one REAPER undo point, the compressed
// original untouched), reloads the view with the selection the edit was asked
// on, and runs the edit without a second prompt. From then on the item is a
// WAV item and every later edit is instant and in place.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define strcasecmp _stricmp   // as standalone_file.cpp: the path compare is case-insensitive here
#endif

static const int kConvertChunkFrames = 65536;

// "MP3" / "FLAC" / ... when the item's source is not a WAV (anything REAPER
// can play converts); "" for a WAV or no item.
std::string SneakPeak::DestructiveConvertExt() const
{
  MediaItem_Take* take = m_waveform.GetTake();
  if (!m_waveform.HasItem() || !take || m_waveform.IsStandaloneMode()) return std::string();
  const std::string path = AudioEngine::GetSourceFilePath(take);
  const size_t dot = path.find_last_of('.');
  if (path.empty() || dot == std::string::npos) return std::string();
  std::string ext = path.substr(dot + 1);
  for (auto& c : ext) c = (char)toupper((unsigned char)c);
  return (ext == "WAV" || ext == "WAVE") ? std::string() : ext;
}

// The one prompt, then the pump starts. `verb` completes "... and then <verb>"
// ("reverse it"); `then` is the edit, run after the swap. False = declined or
// the source/destination could not be opened (a toast says which).
bool SneakPeak::StartConvertToWav(const char* verb, std::function<void()> then)
{
  ConvertJob& J = m_convertJob;
  if (J.active) return false;
  MediaItem_Take* take = m_waveform.GetTake();
  const std::string srcPath = take ? AudioEngine::GetSourceFilePath(take) : std::string();
  const std::string ext = DestructiveConvertExt();
  if (srcPath.empty() || ext.empty()) return false;

  // <name>.wav next to the source; a numbered suffix past an existing file.
  const std::string stem = srcPath.substr(0, srcPath.find_last_of('.'));
  std::string outPath = stem + ".wav";
  for (int suffix = 2;; suffix++) {
    FILE* probe = fopen(outPath.c_str(), "rb");
    if (!probe) break;
    fclose(probe);
    if (suffix > 99) {
      ShowToast("Output names exhausted next to the source - clean up old .wav copies");
      return false;
    }
    outPath = stem + "_" + std::to_string(suffix) + ".wav";
  }
  const size_t slash = outPath.find_last_of("/\\");
  const std::string outName = slash == std::string::npos ? outPath : outPath.substr(slash + 1);

  char prompt[640];
  snprintf(prompt, sizeof(prompt),
           "This item's source is %s, which cannot be edited in place.\n\n"
           "SneakPeak will decode it to WAV (%s, next to the file), point every item that uses "
           "the %s at the WAV, and then %s. The %s itself stays untouched.\n\nContinue?",
           ext.c_str(), outName.c_str(), ext.c_str(), verb, ext.c_str());
  if (MessageBox(m_hwnd, prompt, "SneakPeak - Convert to WAV", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return false;

  if (!AudioEngine::OpenSourceReader(srcPath, J.reader)) {
    ShowToast("Could not open the source for decoding");
    return false;
  }
  // 32-bit float: lossless for what the decoder delivers, clip-safe for hot MP3s.
  if (!J.writer.Begin(outPath, J.reader.nch, J.reader.sr, 32, 3)) {
    AudioEngine::CloseSourceReader(J.reader);
    ShowToast("Write failed - check the source folder permissions");
    return false;
  }
  J.srcPath = srcPath;
  J.outPath = outPath;
  J.verb = verb;
  J.then = std::move(then);
  J.sel = m_waveform.GetSelection();
  J.viewStart = m_waveform.GetViewStart();
  J.viewDur = m_waveform.GetViewDuration();
  J.generation = m_waveform.GetLoadGeneration();
  J.lastPct = -1;
  J.active = true;
  return true;
}

// OnTimer: ~15 ms of decode + write per tick, progress in the title.
void SneakPeak::StepConvertJob()
{
  ConvertJob& J = m_convertJob;
  if (!J.active) return;
  if (J.pending) {
    RunConvertedEdit();
    return;
  }
  if (m_waveform.IsStandaloneMode() || J.generation != m_waveform.GetLoadGeneration() ||
      (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)m_waveform.GetTake(), "MediaItem_Take*"))) {
    AbortConvertJob("Conversion cancelled - the item changed");
    return;
  }
  const DWORD t0 = GetTickCount();
  J.chunk.resize((size_t)kConvertChunkFrames * (size_t)J.reader.nch);
  while (J.reader.pos < J.reader.frames && GetTickCount() - t0 < 15) {
    const int n = AudioEngine::ReadSourceChunk(J.reader, J.chunk.data(), kConvertChunkFrames);
    if (n <= 0) break;
    if (!J.writer.Write(J.chunk.data(), n)) {
      AbortConvertJob("Conversion failed - could not write the WAV");
      return;
    }
  }
  if (J.reader.pos < J.reader.frames) {
    const int pct = (int)(100.0 * (double)J.reader.pos / (double)J.reader.frames);
    if (pct != J.lastPct && m_hwnd) {   // title writes are not free - only on change
      J.lastPct = pct;
      char title[128];
      snprintf(title, sizeof(title), "SneakPeak: Converting to WAV... %d%% (Esc cancels)", pct);
      SetWindowText(m_hwnd, title);
    }
    return;
  }
  FinishConvertJob();
}

// Esc / window close / item change: the destination never existed.
void SneakPeak::AbortConvertJob(const char* toast)
{
  ConvertJob& J = m_convertJob;
  if (!J.active) return;
  const bool swapped = J.pending;   // the WAV is in place; only the edit is dropped
  J.writer.Abort();
  AudioEngine::CloseSourceReader(J.reader);
  J.active = false;
  J.pending = false;
  J.then = nullptr;
  if (m_hwnd) UpdateTitle();
  if (toast) ShowToast(swapped ? "Converted to WAV - the edit was not run" : toast);
}

// The WAV is complete: swap the source under every take that used the file
// and hand the reload + the edit to the pending phase (RunConvertedEdit).
void SneakPeak::FinishConvertJob()
{
  ConvertJob& J = m_convertJob;
  const bool ok = J.writer.End();
  AudioEngine::CloseSourceReader(J.reader);
  UpdateTitle();
  if (!ok) {
    J.active = false;
    J.then = nullptr;
    ShowToast("Write failed - check the source folder permissions");
    return;
  }
  // Our readers on the old source go first (the take keeps its pointer, only
  // its source changes - the retained buffer must not survive the swap).
  AbortItemAudioLoad();
  JoinDynamicsWorker(true);
  m_waveform.ReleaseTakeAccessors();
  const int count = ReplaceSourceInTimeline(J.srcPath, J.outPath);
  DBG("[SneakPeak] convert: %s -> %s, %d items swapped\n", J.srcPath.c_str(), J.outPath.c_str(), count);
  const size_t slash = J.outPath.find_last_of("/\\");
  char buf[sizeof(m_toastText)];
  snprintf(buf, sizeof(buf), "Converted to WAV: %s (%d item%s)",
           slash == std::string::npos ? J.outPath.c_str() : J.outPath.c_str() + slash + 1,
           count, count == 1 ? "" : "s");
  ShowToast(buf);
  if (count <= 0) {   // nothing referenced the file any more: no edit either
    J.active = false;
    J.then = nullptr;
    return;
  }
  m_waveform.ClearItem();
  J.pending = true;
  J.pendingUntil = GetTickCount() + 8000;
  RunConvertedEdit();
}

// The pending phase: reload the view onto the WAV (on Windows the swapped
// media comes back online a moment after the swap, so the first reload can
// find no item - try again per tick, for up to 8 s), restore the view range
// and the selection the edit was asked on, then run the edit.
void SneakPeak::RunConvertedEdit()
{
  ConvertJob& J = m_convertJob;
  if (!m_waveform.HasItem()) LoadSelectedItem();
  bool ready = false;
  if (m_waveform.HasItem() && m_waveform.GetTake()) {
    const std::string now = AudioEngine::GetSourceFilePath(m_waveform.GetTake());
#ifdef __linux__
    ready = now == J.outPath;
#else
    ready = strcasecmp(now.c_str(), J.outPath.c_str()) == 0;
#endif
  }
  if (!ready) {
    if (GetTickCount() < J.pendingUntil) return;   // next tick
    J.active = false;
    J.pending = false;
    J.then = nullptr;
    ShowToast("Converted to WAV - select the item again and repeat the edit");
    return;
  }
  std::function<void()> then = std::move(J.then);
  J.then = nullptr;
  J.active = false;
  J.pending = false;
  if (J.viewDur > 0.0 && J.viewDur <= m_waveform.GetItemDuration()) {
    m_waveform.SetViewStart(J.viewStart);
    m_waveform.SetViewDuration(J.viewDur);
  }
  if (J.sel.active) {
    m_waveform.SetSelection(J.sel);
    SyncSelectionToReaper();
  }
  if (then) then();
  Invalidate();
}
