// edit_view.cpp — Main SneakPeak window, double-buffered GDI rendering
// Includes: markers, clipboard ops, destructive editing, context menu
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
#include <sys/stat.h>
#include <pthread.h>

AudioClipboard SneakPeak::s_clipboard;

SneakPeak::SneakPeak() {}
SneakPeak::~SneakPeak() { Destroy(); }

void SneakPeak::Create()
{
  if (m_hwnd) return;

  m_hwnd = CreateSneakPeakDialog(g_reaperMainHwnd, DlgProc, (LPARAM)this);
  if (!m_hwnd) {
    DBG("[SneakPeak] Failed to create dialog\n");
    return;
  }

  if (g_DockWindowAddEx) {
    g_DockWindowAddEx(m_hwnd, "SneakPeak", "SneakPeak_main", true);
  }

  ShowWindow(m_hwnd, SW_SHOW);
  SetTimer(m_hwnd, TIMER_REFRESH, TIMER_INTERVAL_MS, nullptr);

  // Restore persisted settings
  if (g_GetExtState) {
    const char* snap = g_GetExtState("SneakPeak", "snap_zero");
    if (snap && snap[0] == '1') m_waveform.SetSnapToZero(true);
    const char* mm = g_GetExtState("SneakPeak", "minimap");
    if (mm && mm[0] == '1') m_minimapVisible = true;
    const char* mmh = g_GetExtState("SneakPeak", "minimap_h");
    if (mmh && mmh[0]) {
      int h = atoi(mmh);
      if (h >= MINIMAP_HEIGHT && h <= 120) m_minimapHeight = h;
    }
    const char* multiMode = g_GetExtState("SneakPeak", "multi_mode");
    if (multiMode && strcmp(multiMode, "layered") == 0)
      m_waveform.SetMultiItemMode(MultiItemMode::LAYERED);
    else if (multiMode && strcmp(multiMode, "layered_tracks") == 0)
      m_waveform.SetMultiItemMode(MultiItemMode::LAYERED_TRACKS);
    const char* joinLines = g_GetExtState("SneakPeak", "show_join_lines");
    if (joinLines && joinLines[0] == '0') m_waveform.SetShowJoinLines(false);
    const char* meterMode = g_GetExtState("SneakPeak", "meter_mode");
    if (meterMode && strcmp(meterMode, "rms") == 0) m_levels.SetMode(MeterMode::RMS);
    else if (meterMode && strcmp(meterMode, "vu") == 0) m_levels.SetMode(MeterMode::VU);
    const char* meterSrc = g_GetExtState("SneakPeak", "meter_source");
    if (meterSrc && strcmp(meterSrc, "master") == 0) m_meterFromMaster = true;
  }

  // Recalc layout after restoring settings (minimap visibility etc.)
  {
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    RecalcLayout(cr.right, cr.bottom);
  }

  DBG("[SneakPeak] Window created: hwnd=%p\n", (void*)m_hwnd);
}

void SneakPeak::Destroy()
{
  if (!m_hwnd) return;
  StandaloneCleanupPreview();
  if (!m_previewTempPath.empty()) { remove(m_previewTempPath.c_str()); m_previewTempPath.clear(); }
  CleanupDragTemp();
  KillTimer(m_hwnd, TIMER_REFRESH);
  if (g_DockWindowRemove) g_DockWindowRemove(m_hwnd);
  DestroyWindow(m_hwnd);
  m_hwnd = nullptr;
}

void SneakPeak::Toggle()
{
  if (!m_hwnd) return;
  m_pendingClose = true;
}

bool SneakPeak::IsVisible() const
{
  return m_hwnd && IsWindowVisible(m_hwnd);
}

void SneakPeak::LoadSelectedItem()
{
  if (!g_CountSelectedMediaItems || !g_GetSelectedMediaItem) return;

  if (m_previewActive) StandaloneCleanupPreview();

  int count = g_CountSelectedMediaItems(nullptr);
  if (count <= 0) {
    m_waveform.ClearItem();
    m_hasUndo = false;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  MediaItem* item = g_GetSelectedMediaItem(nullptr, 0);
  if (!item) return;

  // Multi-item: show all selected items as one continuous waveform (cross-track)
  if (count > 1 && g_GetMediaItemInfo_Value) {
    std::vector<MediaItem*> items;
    for (int i = 0; i < count; i++) {
      MediaItem* mi = g_GetSelectedMediaItem(nullptr, i);
      if (mi) items.push_back(mi);
    }

    // Sort by timeline position
    std::sort(items.begin(), items.end(), [](MediaItem* a, MediaItem* b) {
      return g_GetMediaItemInfo_Value(a, "D_POSITION") < g_GetMediaItemInfo_Value(b, "D_POSITION");
    });

    DBG("[SneakPeak] Multi-item: %d items selected\n", (int)items.size());
#ifdef SNEAKPEAK_DEBUG
    for (size_t i = 0; i < items.size() && i < 8; i++) {
      double p = g_GetMediaItemInfo_Value(items[i], "D_POSITION");
      double l = g_GetMediaItemInfo_Value(items[i], "D_LENGTH");
      DBG("[SneakPeak]   item[%d]: pos=%.3f len=%.3f\n", (int)i, p, l);
    }
#endif

    if (items.size() > 1) {
      m_waveform.SetItems(items);
      m_waveform.UpdateFadeCache(); // clear stale single-item fades immediately
      m_spectralVisible = false;  // spectral is per-item, reset on switch
      m_spectral.ClearSpectrum();
      m_spectral.Invalidate();
      m_minimap.Invalidate();

      // Hide gain panel if items span multiple tracks (changing D_VOL across tracks is ambiguous)
      // Same track: batch mode (knob applies relative offset to all items)
      bool multiTrack = false;
      if (g_GetMediaItem_Track) {
        MediaTrack* firstTrack = g_GetMediaItem_Track(items[0]);
        for (size_t i = 1; i < items.size(); i++) {
          if (g_GetMediaItem_Track(items[i]) != firstTrack) { multiTrack = true; break; }
        }
      }
      if (multiTrack) {
        m_gainPanel.Hide();
      } else {
        m_gainPanel.ShowBatch(items);
      }
      m_hasUndo = false;
      m_dirty = false;
      if (m_hwnd) {
        RECT cr;
        GetClientRect(m_hwnd, &cr);
        RecalcLayout(cr.right, cr.bottom);
        m_waveform.Invalidate();
        char title[512];
        snprintf(title, sizeof(title), "SneakPeak [%d items]", (int)items.size());
        SetWindowText(m_hwnd, title);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      DBG("[SneakPeak] Multi-item loaded: segments=%d audioFrames=%d dur=%.3f\n",
          (int)m_waveform.GetSegments().size(), m_waveform.GetAudioSampleCount(),
          m_waveform.GetItemDuration());
      return;
    }
  }

  // Clear first to exit multi-item mode if active
  if (m_waveform.IsMultiItem()) m_waveform.ClearItem();
  m_waveform.SetItem(item);
  m_waveform.UpdateFadeCache(); // read D_VOL/fades immediately so first paint is correct
  m_spectralVisible = false;  // spectral is per-item, reset on switch
  m_spectral.ClearSpectrum();
  m_spectral.Invalidate();
  m_minimap.Invalidate();
  if (m_hwnd) {
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    RecalcLayout(cr.right, cr.bottom);
    m_waveform.Invalidate();
  }

  // Gain panel — always visible, follows current item
  m_gainPanel.Show(item);

  // Read WAV format info for write-back + cache file size
  m_cachedFileSizeMB = 0.0;
  MediaItem_Take* take = m_waveform.GetTake();
  if (take) {
    std::string path = AudioEngine::GetSourceFilePath(take);
    if (!path.empty()) {
      WavInfo info;
      if (AudioEngine::ReadWavHeader(path, info)) {
        m_wavBitsPerSample = info.bitsPerSample;
        m_wavAudioFormat = info.audioFormat;
      }
      struct stat st;
      if (stat(path.c_str(), &st) == 0)
        m_cachedFileSizeMB = static_cast<double>(st.st_size) / (1024.0 * 1024.0);
    }
  }

  m_hasUndo = false;
  m_dirty = false;
  m_lastChanMode = 0;
  if (g_GetSetMediaItemTakeInfo && m_waveform.GetTake()) {
    int* pCM = (int*)g_GetSetMediaItemTakeInfo(m_waveform.GetTake(), "I_CHANMODE", nullptr);
    if (pCM) m_lastChanMode = *pCM;
  }

  if (m_hwnd) {
    UpdateTitle();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::OnTimer()
{
  if (m_pendingClose) {
    m_pendingClose = false;
    if (g_SetExtState) g_SetExtState("SneakPeak", "was_visible", "0", true);
    Destroy();
    return;
  }
  if (!m_hwnd || !IsVisible()) return;

  // Validate cached item pointer — may become dangling after split/snap/delete in arrange
  if (m_waveform.HasItem() && !m_waveform.IsStandaloneMode() && g_ValidatePtr2 &&
      !g_ValidatePtr2(nullptr, (void*)m_waveform.GetItem(), "MediaItem*")) {
    m_waveform.ClearItem();
    m_hasUndo = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Auto-scroll when dragging selection near edges
  if (m_dragging && m_waveform.HasItem()) {
    int edgeZone = EDGE_ZONE;
    int mx = m_lastMouseX;
    double scrollSpeed = m_waveform.GetViewDuration() * 0.08; // 8% of view per tick

    if (mx < m_waveformRect.left + edgeZone) {
      // Scroll left — faster the closer to edge
      double factor = 1.0 - (double)(mx - m_waveformRect.left) / (double)edgeZone;
      if (factor < 0.0) factor = 1.0; // past edge = max speed
      m_waveform.ScrollH(-scrollSpeed * factor);
      m_waveform.UpdateSelection(m_waveform.XToTime(mx));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    else if (mx > m_waveformRect.right - DB_SCALE_WIDTH - edgeZone) {
      // Scroll right
      int rightEdge = m_waveformRect.right - DB_SCALE_WIDTH;
      double factor = 1.0 - (double)(rightEdge - mx) / (double)edgeZone;
      if (factor < 0.0) factor = 1.0;
      m_waveform.ScrollH(scrollSpeed * factor);
      m_waveform.UpdateSelection(m_waveform.XToTime(mx));
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
  }

  if (g_GetPlayState) {
    int state = g_GetPlayState();
    bool playing = (state & 1) != 0;

    // Track follow: when external playback, follow items on selected track
    // Skip when multi-item view is active — don't overwrite concatenated view
    if (playing && !m_startedPlayback && !m_waveform.IsMultiItem() &&
        g_GetPlayPosition2 &&
        g_GetSelectedTrack && g_GetTrackNumMediaItems && g_GetTrackMediaItem &&
        g_GetMediaItemInfo_Value) {
      MediaTrack* selTrack = g_GetSelectedTrack(nullptr, 0);
      if (selTrack) {
        double playPos = g_GetPlayPosition2();
        int numItems = g_GetTrackNumMediaItems(selTrack);
        for (int i = 0; i < numItems; i++) {
          MediaItem* trackItem = g_GetTrackMediaItem(selTrack, i);
          if (!trackItem) continue;
          double iPos = g_GetMediaItemInfo_Value(trackItem, "D_POSITION");
          double iLen = g_GetMediaItemInfo_Value(trackItem, "D_LENGTH");
          if (playPos >= iPos && playPos < iPos + iLen) {
            if (trackItem != m_waveform.GetItem()) {
              m_waveform.SetItem(trackItem);
              InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            break;
          }
        }
      }
    }

    // Grace period countdown after play start
    if (m_playGraceTicks > 0) m_playGraceTicks--;

    // No auto-stop — user controls playback via spacebar

    if (!playing && m_wasPlaying) {
      m_startedPlayback = false;
      m_autoStopped = false;  // reset for next play
    }
    m_wasPlaying = playing;

    if (playing) {
      // Scroll view to follow playhead when it exits visible area
      if (g_GetPlayPosition2 && m_waveform.HasItem() && !m_waveform.IsStandaloneMode()) {
        double relPos = m_waveform.AbsTimeToRelTime(g_GetPlayPosition2());
        double viewStart = m_waveform.GetViewStart();
        double viewEnd = m_waveform.GetViewEnd();
        if (relPos >= 0.0 && relPos <= m_waveform.GetItemDuration() &&
            (relPos < viewStart || relPos > viewEnd)) {
          double newStart = relPos - m_waveform.GetViewDuration() * 0.1;
          if (newStart < 0.0) newStart = 0.0;
          m_waveform.SetViewStart(newStart);
          m_waveform.Invalidate();
        }
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
  }

  // Standalone preview: follow playhead (independent of REAPER transport)
  if (m_previewActive && m_previewReg && m_waveform.IsStandaloneMode()) {
    auto* reg = (preview_register_t*)m_previewReg;
    double pos = reg->curpos;
    double viewStart = m_waveform.GetViewStart();
    double viewEnd = m_waveform.GetViewEnd();
    if (pos >= 0.0 && pos <= m_waveform.GetItemDuration() &&
        (pos < viewStart || pos > viewEnd)) {
      double newStart = pos - m_waveform.GetViewDuration() * 0.1;
      if (newStart < 0.0) newStart = 0.0;
      m_waveform.SetViewStart(newStart);
      m_waveform.Invalidate();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  // Keep repainting while RMS meters are decaying after stop
  if (m_levels.IsDecaying()) {
    InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
  }

  // Keep repainting while spectral is computing (progress bar update)
  if (m_spectralVisible && (m_spectral.IsLoading() || (m_spectral.IsReady() && !m_spectralPainted))) {
    InvalidateRect(m_hwnd, &m_spectralRect, FALSE);
    if (m_spectral.IsReady()) m_spectralPainted = true;
  }

  // Update fade/volume cache for paint (not in standalone mode)
  if (m_waveform.HasItem() && !m_waveform.IsStandaloneMode()) {
    if (m_waveform.UpdateFadeCache()) {
      // Volume changed in REAPER — repaint waveform + meters
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    // Batch gain: sync knob offset to waveform for visual feedback
    if (m_gainPanel.IsBatch()) {
      double offsetLin = pow(10.0, m_gainPanel.GetDb() / 20.0);
      m_waveform.SetBatchGainOffset(offsetLin);
    } else if (m_waveform.IsMultiItemActive()) {
      m_waveform.SetBatchGainOffset(1.0);
    }
  }
  // In standalone mode, reflect gain panel dB in waveform display
  if (m_waveform.IsStandaloneMode() && m_gainPanel.IsVisible()) {
    double gainLin = pow(10.0, m_gainPanel.GetDb() / 20.0);
    if (m_waveform.HasSelection()) {
      WaveformSelection sel = m_waveform.GetSelection();
      double s = std::min(sel.startTime, sel.endTime);
      double e = std::max(sel.startTime, sel.endTime);
      m_waveform.SetStandaloneGain(gainLin, s, e);
    } else {
      m_waveform.SetStandaloneGain(gainLin, -1.0, -1.0); // whole file
    }
  } else if (m_waveform.IsStandaloneMode()) {
    m_waveform.ClearStandaloneGain();
  }

  // Multi-item: detect volume changes and reload (every ~1s to avoid heavy polling)
  if (m_waveform.IsMultiItemActive() && !m_waveform.IsStandaloneMode()) {
    if (m_audioChangeCheckCounter % 30 == 0) {
      if (m_waveform.GetMultiItemView().CheckVolumeChanged()) {
        LoadSelectedItem();  // reloads with new volumes
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    }
  }

  // Update solo state from REAPER (polled, not in paint, not in standalone)
  if (!m_waveform.IsStandaloneMode()) UpdateSoloState();

  // Single-item only: refresh position/duration, detect channel mode changes
  if (m_waveform.HasItem() && !m_waveform.IsStandaloneMode() && !m_waveform.IsMultiItem() && g_GetMediaItemInfo_Value) {
    double pos = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_POSITION");
    double len = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_LENGTH");
    if (pos != m_waveform.GetItemPosition() || len != m_waveform.GetItemDuration()) {
      m_waveform.SetItemPosition(pos);
      m_waveform.SetItemDuration(len);
    }

    bool bothActive = m_waveform.IsChannelActive(0) && m_waveform.IsChannelActive(1);
    if (g_GetSetMediaItemTakeInfo && m_waveform.GetTake() && bothActive) {
      int* pChanMode = (int*)g_GetSetMediaItemTakeInfo(m_waveform.GetTake(), "I_CHANMODE", nullptr);
      int chanMode = pChanMode ? *pChanMode : 0;
      if (chanMode != m_lastChanMode) {
        m_lastChanMode = chanMode;
        MediaItem* item = m_waveform.GetItem();
        m_waveform.ClearItem();
        m_waveform.SetItem(item);
        if (m_gainPanel.IsVisible()) m_gainPanel.Show(item);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    }
  }

  // External audio change detection (every 30 ticks ≈ 1 second)
  if (!m_waveform.IsStandaloneMode() && m_waveform.HasItem()) {
    if (++m_audioChangeCheckCounter >= 30) {
      m_audioChangeCheckCounter = 0;
      if (m_waveform.CheckAudioChanged()) {
        DBG("[SneakPeak] External audio change detected, reloading\n");
        m_waveform.ReloadAfterExternalChange();
        m_spectral.ClearSpectrum();
        m_spectral.Invalidate();
        m_minimap.Invalidate();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    }
  }

  // Cursor + RMS levels — works for both single and multi-item
  if (m_waveform.HasItem()) {
    if (m_waveform.IsStandaloneMode() && m_previewActive && m_previewReg) {
      // Track standalone preview cursor
      auto* reg = (preview_register_t*)m_previewReg;
      pthread_mutex_lock(&reg->mutex);
      double pos = reg->curpos;
      pthread_mutex_unlock(&reg->mutex);
      double dur = m_waveform.GetItemDuration();
      if (pos >= dur) {
        // Preview finished
        DBG("[SneakPeak] Preview finished: pos=%.3f dur=%.3f\n", pos, dur);
        StandaloneCleanupPreview();
      } else {
        m_waveform.SetCursorTime(pos);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    } else if (g_GetCursorPosition && !m_waveform.IsStandaloneMode()) {
      double curPos = g_GetCursorPosition();
      double relPos = m_waveform.AbsTimeToRelTime(curPos);
      // Clamp cursor to item bounds
      double dur = m_waveform.GetItemDuration();
      if (relPos < 0.0) relPos = 0.0;
      if (relPos > dur) relPos = dur;
      m_waveform.SetCursorTime(relPos);
    }

    // Meter source: master track or item audio
    if (m_meterFromMaster && g_GetMasterTrack && g_Track_GetPeakInfo && g_GetPlayState) {
      MediaTrack* master = g_GetMasterTrack(nullptr);
      bool playing = (g_GetPlayState() & 1) != 0;
      if (master) {
        double pkL = g_Track_GetPeakInfo(master, 0);
        double pkR = g_Track_GetPeakInfo(master, 1);
        m_levels.UpdateFromTrackPeak(pkL, pkR, playing, 2);
      }
    } else {
      int sr = m_waveform.GetSampleRate();
      int nch = m_waveform.GetNumChannels();
      if (sr > 0 && nch > 0) {
        bool playing = g_GetPlayState && (g_GetPlayState() & 1);
        int startFrame, endFrame;
        if (playing) {
          double absPos = g_GetPlayPosition ? g_GetPlayPosition()
                        : (g_GetPlayPosition2 ? g_GetPlayPosition2() : 0.0);
          double playPos = m_waveform.AbsTimeToRelTime(absPos);
          if (playPos < 0.0) playPos = 0.0;
          int center = static_cast<int>(playPos * sr);
          int halfWin = m_levels.GetIntegrationHalfWindow(sr);
          startFrame = center - halfWin;
          endFrame = center + halfWin;
        } else {
          startFrame = static_cast<int>(m_waveform.GetViewStart() * sr);
          endFrame = static_cast<int>(m_waveform.GetViewEnd() * sr);
        }
        double itemVol = 1.0;
        if (!m_waveform.IsMultiItem() && g_GetMediaItemInfo_Value) {
          itemVol = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_VOL");
          if (g_GetSetMediaItemTakeInfo && m_waveform.GetTake()) {
            double* pTakeVol = (double*)g_GetSetMediaItemTakeInfo(m_waveform.GetTake(), "D_VOL", nullptr);
            if (pTakeVol) itemVol *= *pTakeVol;
          }
        }
        const bool chActive[2] = { m_waveform.IsChannelActive(0), m_waveform.IsChannelActive(1) };

        if (m_waveform.IsMultiItemActive()) {
          std::vector<double> mixBuf;
          m_waveform.GetMultiItemView().GetMixedAudio(startFrame, endFrame, nch, mixBuf);
          m_levels.Update(mixBuf, 0, (int)mixBuf.size() / std::max(1, nch), sr, nch, itemVol, playing, chActive);
        } else {
          m_levels.Update(m_waveform.GetAudioData(), startFrame, endFrame, sr, nch, itemVol, playing, chActive);
        }
      }
    }
    // (master mode is toggled manually via MASTER tab, not auto-disabled here)
  }

  // Master meter mode (manually activated via MASTER tab)
  if (m_masterMode) {
    if (g_GetMasterTrack && g_Track_GetPeakInfo && g_GetPlayState) {
      MediaTrack* master = g_GetMasterTrack(nullptr);
      bool playing = (g_GetPlayState() & 1) != 0;
      if (master) {
        double pkL = g_Track_GetPeakInfo(master, 0);
        double pkR = g_Track_GetPeakInfo(master, 1);
        m_levels.UpdateFromTrackPeak(pkL, pkR, playing, 2);

        // Push into rolling buffer
        if (playing) {
          m_masterPeakBufL[m_masterPeakHead] = (float)pkL;
          m_masterPeakBufR[m_masterPeakHead] = (float)pkR;
          m_masterPeakHead = (m_masterPeakHead + 1) % MASTER_ROLLING_SIZE;
          if (m_masterPeakCount < MASTER_ROLLING_SIZE) m_masterPeakCount++;
        }
        InvalidateRect(m_hwnd, &m_waveformRect, FALSE);
        InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
      }
    }
  }
}



void SneakPeak::GetItemTitle(char* buf, int bufSize)
{
  const char* prefix = m_dirty ? "* " : "";
  if (!m_waveform.HasItem()) {
    snprintf(buf, bufSize, "%sSneakPeak", prefix);
    return;
  }
  MediaItem_Take* take = g_GetActiveTake ? g_GetActiveTake(m_waveform.GetItem()) : nullptr;
  if (take && g_GetSetMediaItemTakeInfo_String) {
    char nameBuf[256] = {};
    if (g_GetSetMediaItemTakeInfo_String(take, "P_NAME", nameBuf, false)) {
      snprintf(buf, bufSize, "%sSneakPeak: %s", prefix, nameBuf);
      return;
    }
  }
  snprintf(buf, bufSize, "%sSneakPeak", prefix);
}

void SneakPeak::UpdateTitle()
{
  if (!m_hwnd) return;
  char title[512];
  if (m_waveform.IsStandaloneMode()) {
    // Show saved filename if available, otherwise original
    const std::string& displayPath = m_savedPath.empty()
        ? m_waveform.GetStandaloneFilePath() : m_savedPath;
    const char* name = FileNameFromPath(displayPath.c_str());
    snprintf(title, sizeof(title), "%sSneakPeak: %s",
             m_dirty ? "* " : "", name);
  } else {
    GetItemTitle(title, sizeof(title));
  }
  SetWindowText(m_hwnd, title);
}

// --- Dialog Procedure ---

INT_PTR CALLBACK SneakPeak::DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  SneakPeak* self = nullptr;

  if (msg == WM_CREATE || msg == WM_INITDIALOG) {
    self = reinterpret_cast<SneakPeak*>(lParam);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    if (self) self->m_hwnd = hwnd;
    return 0;
  }

  self = reinterpret_cast<SneakPeak*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (!self) return DefWindowProc(hwnd, msg, wParam, lParam);

  INT_PTR result = self->HandleMessage(msg, wParam, lParam);
  if (result == -1) return DefWindowProc(hwnd, msg, wParam, lParam);
  return result;
}

INT_PTR SneakPeak::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;

    case WM_SIZE: {
      RECT rc;
      GetClientRect(m_hwnd, &rc);
      int w = rc.right - rc.left;
      int h = rc.bottom - rc.top;
      if (w > 0 && h > 0) OnSize(w, h);
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(m_hwnd, &ps);
      if (hdc) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

#ifdef _WIN32
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
        OnPaint(memDC);
        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
#else
        HDC memDC = SWELL_CreateMemContext(hdc, w, h);
        if (memDC) {
          OnPaint(memDC);
          BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
          SWELL_DeleteGfxContext(memDC);
        } else {
          OnPaint(hdc);
        }
#endif
      }
      EndPaint(m_hwnd, &ps);
      return 0;
    }

    case WM_LBUTTONDBLCLK: {
      int x = (short)LOWORD(lParam);
      int y = (short)HIWORD(lParam);
      OnDoubleClick(x, y);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      int x = (short)LOWORD(lParam);
      int y = (short)HIWORD(lParam);
      OnMouseDown(x, y, wParam);
      return 0;
    }

    case WM_LBUTTONUP: {
      int x = (short)LOWORD(lParam);
      int y = (short)HIWORD(lParam);
      OnMouseUp(x, y);
      return 0;
    }

    case WM_MOUSEMOVE: {
      int x = (short)LOWORD(lParam);
      int y = (short)HIWORD(lParam);
      OnMouseMove(x, y, wParam);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      int delta = (short)HIWORD(wParam);
      POINT pt;
      pt.x = (short)LOWORD(lParam);
      pt.y = (short)HIWORD(lParam);
      ScreenToClient(m_hwnd, &pt);
      OnMouseWheel(pt.x, pt.y, delta, wParam);
      return 0;
    }

    case WM_KEYDOWN:
      OnKeyDown(wParam);
      return 0;

    case WM_RBUTTONUP: {
      int x = (short)LOWORD(lParam);
      int y = (short)HIWORD(lParam);
      OnRightClick(x, y);
      return 0;
    }

    case WM_COMMAND: {
      int id = LOWORD(wParam);
      if (id == IDCANCEL) {
        // Docker [x] button sends IDCANCEL — defer destruction to OnTimer
        // to avoid re-entrancy crash (destroying window inside its own DlgProc)
        m_pendingClose = true;
        return 0;
      }
      if (id >= CM_UNDO && id < CM_LAST) {
        OnContextMenuCommand(id);
        return 0;
      }
      break;
    }

    case WM_TIMER:
      if (wParam == TIMER_REFRESH) OnTimer();
      return 0;

    case WM_SETCURSOR:
      // Let OnMouseMove handle cursor — prevent system from resetting it
      return TRUE;

    case WM_DROPFILES: {
      DBG("[SneakPeak] WM_DROPFILES received\n");
      HDROP hDrop = (HDROP)wParam;
      int n = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0);
      for (int i = 0; i < n && i < MAX_STANDALONE_FILES; i++) {
        char path[2048] = {};
        DragQueryFile(hDrop, i, path, sizeof(path));
        DBG("[SneakPeak] Drop file[%d]: %s\n", i, path);
        if (path[0]) AddStandaloneFile(path);
      }
      DragFinish(hDrop);
      return 0;
    }

    case WM_CLOSE:
      m_pendingClose = true;
      return 0;

    case WM_DESTROY:
      KillTimer(m_hwnd, TIMER_REFRESH);
      return 0;
  }

  return -1;
}

// --- Layout ---

void SneakPeak::RecalcLayout(int w, int h)
{
  m_toolbarRect      = { 0, 0, w, TOOLBAR_HEIGHT };
  m_modeBarRect      = { 0, TOOLBAR_HEIGHT, w, TOOLBAR_HEIGHT + MODE_BAR_HEIGHT };
  m_rulerRect        = { 0, TOOLBAR_HEIGHT + MODE_BAR_HEIGHT, w, TOOLBAR_HEIGHT + MODE_BAR_HEIGHT + RULER_HEIGHT };
  m_bottomPanelRect  = { 0, h - BOTTOM_PANEL_HEIGHT, w, h };
  int minimapH = m_minimapVisible ? m_minimapHeight : 0;
  m_scrollbarRect    = { 0, h - BOTTOM_PANEL_HEIGHT - SCROLLBAR_HEIGHT, w, h - BOTTOM_PANEL_HEIGHT };
  m_minimapRect      = { 0, m_scrollbarRect.top - minimapH, w, m_scrollbarRect.top };

  int contentTop = TOOLBAR_HEIGHT + MODE_BAR_HEIGHT + RULER_HEIGHT;
  int contentBot = m_minimapRect.top;
  int contentH = contentBot - contentTop;

  if (m_spectralVisible && contentH > MIN_WAVEFORM_HEIGHT + MIN_SPECTRAL_HEIGHT + SPLITTER_HEIGHT) {
    int waveH = (int)((float)contentH * m_splitterRatio) - SPLITTER_HEIGHT / 2;
    waveH = std::max(MIN_WAVEFORM_HEIGHT, std::min(contentH - MIN_SPECTRAL_HEIGHT - SPLITTER_HEIGHT, waveH));
    int splitterTop = contentTop + waveH;
    int spectralTop = splitterTop + SPLITTER_HEIGHT;

    m_waveformRect = { 0, contentTop, w, splitterTop };
    m_splitterRect = { 0, splitterTop, w, spectralTop };
    m_spectralRect = { 0, spectralTop, w, contentBot };
  } else {
    m_waveformRect = { 0, contentTop, w, contentBot };
    m_splitterRect = {};
    m_spectralRect = {};
  }

  m_toolbar.SetRect(0, 0, w, TOOLBAR_HEIGHT);
  m_waveform.SetRect(m_waveformRect.left, m_waveformRect.top,
                     m_waveformRect.right - m_waveformRect.left,
                     m_waveformRect.bottom - m_waveformRect.top);
  if (m_spectralVisible) {
    m_spectral.SetRect(m_spectralRect.left, m_spectralRect.top,
                       m_spectralRect.right - m_spectralRect.left,
                       m_spectralRect.bottom - m_spectralRect.top);
  }
  if (m_minimapVisible) {
    m_minimap.SetRect(m_minimapRect.left, m_minimapRect.top,
                      m_minimapRect.right - m_minimapRect.left,
                      m_minimapRect.bottom - m_minimapRect.top);
  }
}

void SneakPeak::OnSize(int w, int h)
{
  RecalcLayout(w, h);
  m_waveform.Invalidate();
  m_spectral.Invalidate();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Painting ---

// --- Mouse ---

void SneakPeak::OnDoubleClick(int x, int y)
{
  // Double-click on gain panel = reset to 0 dB
  if (m_gainPanel.OnDoubleClick(x, y, m_waveformRect)) {
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Double-click on waveform area: marker edit or select all
  if (y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    // Check if clicking on a marker first
    int markerIdx = m_markers.HitTestMarker(x, m_waveform);
    if (markerIdx >= 0) {
      m_markers.EditMarkerDialog(markerIdx);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
    // Otherwise select all
    if (m_waveform.HasItem()) {
      m_dragExportPending = false; // cancel any pending drag
      m_dragging = false;          // prevent mouseup from clearing selection
      ReleaseCapture();
      m_waveform.StartSelection(0.0);
      m_waveform.UpdateSelection(m_waveform.GetItemDuration());
      m_waveform.EndSelection();
      SyncSelectionToReaper();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  // Double-click on marker in ruler = edit marker
  if (y >= m_rulerRect.top && y < m_rulerRect.bottom) {
    int markerIdx = m_markers.HitTestMarker(x, m_waveform);
    if (markerIdx >= 0) {
      m_markers.EditMarkerDialog(markerIdx);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }
}

void SneakPeak::OnMouseDown(int x, int y, WPARAM wParam)
{
  m_lastMouseX = x;
  m_lastMouseY = y;

  // Stop standalone preview on click (allows repositioning cursor)
  if (m_previewActive) StandaloneCleanupPreview();

  if (y >= m_toolbarRect.top && y < m_toolbarRect.bottom) {
    int btn = m_toolbar.HitTest(x, y);
    if (btn >= 0) OnToolbarClick(btn);
    return;
  }

  // Mode bar click
  if (y >= m_modeBarRect.top && y < m_modeBarRect.bottom) {
    for (const auto& tab : m_modeBarTabs) {
      if (x >= tab.rect.left && x < tab.rect.right &&
          y >= tab.rect.top && y < tab.rect.bottom) {
        // Check close button first
        if (tab.closeRect.right > tab.closeRect.left &&
            x >= tab.closeRect.left && x < tab.closeRect.right &&
            y >= tab.closeRect.top && y < tab.closeRect.bottom) {
          OnModeBarCloseTab(tab.fileIdx);
          return;
        }
        if (tab.fileIdx == -2) {
          // Toggle MASTER mode
          m_masterMode = !m_masterMode;
          if (m_masterMode) {
            // Clear rolling buffer on activation
            m_masterPeakHead = 0;
            m_masterPeakCount = 0;
          }
          InvalidateRect(m_hwnd, nullptr, FALSE);
        } else if (tab.isReaper) {
          // Switch to REAPER mode
          m_masterMode = false;
          if (m_waveform.IsStandaloneMode()) SaveCurrentStandaloneState();
          LoadSelectedItem();
        } else {
          // Switch to standalone tab
          m_masterMode = false;
          if (m_waveform.IsStandaloneMode() && tab.fileIdx == m_activeFileIdx) return;
          if (m_waveform.IsStandaloneMode()) SaveCurrentStandaloneState();
          RestoreStandaloneState(tab.fileIdx);
        }
        return;
      }
    }
    return;
  }

  // Splitter drag
  if (m_spectralVisible && y >= m_splitterRect.top && y < m_splitterRect.bottom) {
    m_splitterDragging = true;
    SetCapture(m_hwnd);
    return;
  }

  if (y >= m_rulerRect.top && y < m_rulerRect.bottom) {
    if (m_waveform.HasItem()) {
      // Check if clicking on a marker — start drag
      int markerIdx = m_markers.HitTestMarker(x, m_waveform);
      if (markerIdx >= 0) {
        m_markers.StartDrag(markerIdx);
        SetCapture(m_hwnd);
        return;
      }

      double time = m_waveform.XToTime(x);
      m_waveform.SetCursorTime(time);
      if (g_SetEditCurPos)
        g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), false, false);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  // Minimap resize — drag top edge
  if (m_minimapVisible && y >= m_minimapRect.top - 3 && y < m_minimapRect.top + 3) {
    m_minimapDragging = true;
    SetCapture(m_hwnd);
    return;
  }

  // Minimap click — start scroll drag
  if (m_minimapVisible && y >= m_minimapRect.top && y < m_minimapRect.bottom && m_waveform.HasItem()) {
    m_minimapScrollDragging = true;
    SetCapture(m_hwnd);
    double clickTime = m_minimap.XToTime(x, m_waveform.GetItemDuration());
    double halfView = m_waveform.GetViewDuration() / 2.0;
    double newStart = clickTime - halfView;
    newStart = std::max(0.0, std::min(m_waveform.GetItemDuration() - m_waveform.GetViewDuration(), newStart));
    m_waveform.ScrollH(newStart - m_waveform.GetViewStart());
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (y >= m_scrollbarRect.top && y < m_scrollbarRect.bottom) {
    m_scrollbarDragging = true;
    SetCapture(m_hwnd);
    return;
  }

  // Gain panel interaction
  if (m_gainPanel.IsVisible() && m_gainPanel.HitTest(x, y, m_waveformRect)) {
    if (m_gainPanel.OnMouseDown(x, y, m_waveformRect)) {
      if (m_gainPanel.IsDragging()) {
        SetCapture(m_hwnd);
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return; // don't pass click through
  }

  // Spectral area — time selection + frequency band selection
  if (m_spectralVisible && y >= m_spectralRect.top && y < m_spectralRect.bottom) {
    if (m_waveform.HasItem()) {
      int specH = m_spectralRect.bottom - m_spectralRect.top;
      int nch = m_waveform.GetNumChannels();
      int chSep = (nch > 1) ? CHANNEL_SEPARATOR_HEIGHT : 0;
      int chH = (nch > 1) ? (specH - chSep) / 2 : specH;
      // Determine which channel was clicked
      int chTop = m_spectralRect.top;
      if (nch > 1 && y >= m_spectralRect.top + chH + chSep)
        chTop = m_spectralRect.top + chH + chSep;

      // Alt+click = frequency band selection
      bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      if (altDown) {
        double freq = m_spectral.YToFreq(y, chTop, chH);
        m_spectral.StartFreqSelection(freq);
        m_spectralFreqDragging = true;
        m_spectralFreqDragChTop = chTop;
        m_spectralFreqDragChH = chH;
        SetCapture(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
      }

      // Normal click = time selection (same as waveform)
      m_spectral.ClearFreqSelection();
      double time = m_waveform.XToTime(x);
      if (wParam & MK_SHIFT) {
        m_waveform.UpdateSelection(time);
      } else {
        m_waveform.StartSelection(time);
        m_waveform.SetCursorTime(time);
        if (g_SetEditCurPos)
          g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), false, false);
      }
      m_dragging = true;
      SetCapture(m_hwnd);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  if (y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    if (m_waveform.HasItem()) {
      // Solo button
      if (ClickSoloButton(x, y)) {
        ToggleTrackSolo();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
      }

      // Channel mute button — visual dimming + audio via I_CHANMODE
      if (m_waveform.ClickChannelButton(x, y)) {
        int chanMode = m_waveform.GetChanMode();
        if (m_waveform.GetTake() && g_GetSetMediaItemTakeInfo) {
          g_GetSetMediaItemTakeInfo(m_waveform.GetTake(), "I_CHANMODE", &chanMode);
          m_lastChanMode = chanMode;
          if (g_UpdateArrange) g_UpdateArrange();
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
      }

      // Standalone fade handles — always visible at top corners (16px hit zone)
      if (m_waveform.IsStandaloneMode()) {
        int waveL = m_waveformRect.left;
        int waveR = m_waveformRect.right - DB_SCALE_WIDTH;
        auto sf = m_waveform.GetStandaloneFade();
        int fiX = (sf.fadeInLen >= 0.001) ? m_waveform.TimeToX(sf.fadeInLen) : waveL;
        int foX = (sf.fadeOutLen >= 0.001) ? m_waveform.TimeToX(m_waveform.GetItemDuration() - sf.fadeOutLen) : waveR;
        if (abs(x - fiX) <= 16 && y < m_waveformRect.top + 30) {
          m_fadeDragging = FADE_IN;
          m_standaloneFadeDrag = true;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = sf.fadeInDir;
          SetCapture(m_hwnd);
          return;
        }
        if (abs(x - foX) <= 16 && y < m_waveformRect.top + 30) {
          m_fadeDragging = FADE_OUT;
          m_standaloneFadeDrag = true;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = sf.fadeOutDir;
          SetCapture(m_hwnd);
          return;
        }
      }

      // Check REAPER fade handles (16px hit zone around handle)
      if (!m_waveform.IsStandaloneMode() && g_GetMediaItemInfo_Value) {
        double fadeInLen = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_FADEINLEN");
        double fadeOutLen = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_FADEOUTLEN");
        int fiX = m_waveform.TimeToX(fadeInLen);
        int foX = m_waveform.TimeToX(m_waveform.GetItemDuration() - fadeOutLen);
        if (fadeInLen >= 0.001 && abs(x - fiX) <= 16 && y < m_waveformRect.top + 30) {
          m_fadeDragging = FADE_IN;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_FADEINDIR");
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          return;
        }
        if (fadeOutLen >= 0.001 && abs(x - foX) <= 16 && y < m_waveformRect.top + 30) {
          m_fadeDragging = FADE_OUT;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_FADEOUTDIR");
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          return;
        }
      }

      double time = m_waveform.XToTime(x);

      // Check if clicking inside existing selection — potential drag export
      if (m_waveform.HasSelection() && !(wParam & MK_SHIFT)) {
        WaveformSelection sel = m_waveform.GetSelection();
        double selS = std::min(sel.startTime, sel.endTime);
        double selE = std::max(sel.startTime, sel.endTime);
        if (time >= selS && time <= selE) {
          DBG("[SneakPeak] Drag export pending: click at t=%.3f inside sel [%.3f..%.3f]\n",
              time, selS, selE);
          m_dragExportPending = true;
          m_dragStartX = x;
          m_dragStartY = y;
          SetCapture(m_hwnd);
          return;
        }
      }

      if (wParam & MK_SHIFT) {
        m_waveform.UpdateSelection(time);
      } else {
        m_waveform.StartSelection(time);
        m_waveform.SetCursorTime(time);
        if (g_SetEditCurPos)
          g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), false, false);
      }
      m_dragging = true;
      SetCapture(m_hwnd);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
  }
}

void SneakPeak::OnMouseUp(int x, int y)
{
  if (m_dragExportPending) {
    // Didn't meet drag threshold — treat as click inside selection (place cursor)
    m_dragExportPending = false;
    ReleaseCapture();
    double time = m_waveform.XToTime(x);
    m_waveform.SetCursorTime(time);
    if (g_SetEditCurPos)
      g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), false, false);
    m_waveform.ClearSelection();
    SyncSelectionToReaper();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_spectralFreqDragging) {
    m_spectralFreqDragging = false;
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_minimapDragging) {
    m_minimapDragging = false;
    ReleaseCapture();
    if (g_SetExtState) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", m_minimapHeight);
      g_SetExtState("SneakPeak", "minimap_h", buf, true);
    }
    return;
  }
  if (m_minimapScrollDragging) {
    m_minimapScrollDragging = false;
    ReleaseCapture();
    return;
  }
  if (m_splitterDragging) {
    m_splitterDragging = false;
    ReleaseCapture();
    return;
  }
  if (m_fadeDragging != FADE_NONE) {
    FadeDragType wasType = m_fadeDragging;
    bool wasStandalone = m_standaloneFadeDrag;
    m_fadeDragging = FADE_NONE;
    m_standaloneFadeDrag = false;
    m_waveform.SetFadeDragInfo(0, 0);
    ReleaseCapture();

    if (wasStandalone) {
      // Keep fade as non-destructive preview — baked only on save
      auto sf = m_waveform.GetStandaloneFade();
      if ((wasType == FADE_IN && sf.fadeInLen >= 0.001) ||
          (wasType == FADE_OUT && sf.fadeOutLen >= 0.001)) {
        m_dirty = true;
        UpdateTitle();
      }
      m_waveform.Invalidate();
    } else {
      if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Adjust fade", -1);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_gainPanel.IsDragging()) {
    bool wasKnobDrag = m_gainPanel.IsDragging() && !m_gainPanel.IsPanelDragging();
    m_gainPanel.OnMouseUp();
    ReleaseCapture();

    // In standalone mode, bake gain into audio data immediately on knob release
    if (wasKnobDrag && m_waveform.IsStandaloneMode() && m_gainPanel.IsStandalone()) {
      double db = m_gainPanel.GetDb();
      if (std::abs(db) > 0.01) {
        StandaloneUndoSave(); // save state before destructive edit
        double factor = pow(10.0, db / 20.0);
        auto& data = m_waveform.GetAudioData();
        int nch = m_waveform.GetNumChannels();
        int sr = m_waveform.GetSampleRate();
        int totalFrames = m_waveform.GetAudioSampleCount();

        if (m_waveform.HasSelection()) {
          WaveformSelection sel = m_waveform.GetSelection();
          int startFrame = (int)(std::min(sel.startTime, sel.endTime) * sr);
          int endFrame = (int)(std::max(sel.startTime, sel.endTime) * sr);
          startFrame = std::max(0, std::min(totalFrames, startFrame));
          endFrame = std::max(0, std::min(totalFrames, endFrame));
          int selFrames = endFrame - startFrame;
          if (selFrames > 0) {
            int fadeFrames = std::min(sr / 100, selFrames / 2); // ~10ms crossfade
            AudioOps::GainWithCrossfade(data.data() + (size_t)startFrame * nch, selFrames, nch, factor, fadeFrames);
          }
        } else {
          AudioOps::Gain(data.data(), totalFrames, nch, factor);
        }

        m_gainPanel.ShowStandalone(); // reset knob to 0dB
        m_waveform.ClearStandaloneGain();
        m_waveform.Invalidate(); // recalc peaks
        m_dirty = true;
        UpdateTitle();
        DBG("[SneakPeak] Standalone gain baked: %.1f dB\n", db);
      }
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_dragging) {
    m_waveform.EndSelection();
    m_dragging = false;
    ReleaseCapture();
    SyncSelectionToReaper();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
  if (m_scrollbarDragging) {
    m_scrollbarDragging = false;
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
  if (m_markers.IsDragging()) {
    m_markers.EndDrag();
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::OnMouseMove(int x, int y, WPARAM wParam)
{
  // Drag export: check threshold
  if (m_dragExportPending) {
    int dx = x - m_dragStartX;
    int dy = y - m_dragStartY;
    if (dx * dx + dy * dy > 25) {  // 5px threshold
      DBG("[SneakPeak] Drag threshold met: dx=%d dy=%d, initiating export\n", dx, dy);
      m_dragExportPending = false;
      InitiateDragExport();
      ReleaseCapture();
      return;
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    return;
  }

  // Minimap scroll dragging — continuously scroll waveform
  if (m_minimapScrollDragging && m_waveform.HasItem()) {
    double clickTime = m_minimap.XToTime(x, m_waveform.GetItemDuration());
    double halfView = m_waveform.GetViewDuration() / 2.0;
    double newStart = clickTime - halfView;
    newStart = std::max(0.0, std::min(m_waveform.GetItemDuration() - m_waveform.GetViewDuration(), newStart));
    m_waveform.ScrollH(newStart - m_waveform.GetViewStart());
    m_waveform.Invalidate();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Minimap resize dragging
  if (m_minimapDragging) {
    int scrollTop = m_scrollbarRect.top;
    int newH = scrollTop - y;
    newH = std::max(MINIMAP_HEIGHT, std::min(120, newH));
    if (newH != m_minimapHeight) {
      m_minimapHeight = newH;
      RECT cr;
      GetClientRect(m_hwnd, &cr);
      RecalcLayout(cr.right, cr.bottom);
      m_waveform.Invalidate();
      m_minimap.Invalidate();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  // Splitter dragging
  if (m_splitterDragging) {
    RECT clientRect;
    GetClientRect(m_hwnd, &clientRect);
    int contentTop = TOOLBAR_HEIGHT + MODE_BAR_HEIGHT + RULER_HEIGHT;
    int contentBot = clientRect.bottom - BOTTOM_PANEL_HEIGHT - SCROLLBAR_HEIGHT;
    int contentH = contentBot - contentTop;
    if (contentH > 0) {
      m_splitterRatio = (float)(y - contentTop) / (float)contentH;
      m_splitterRatio = std::max(0.15f, std::min(0.85f, m_splitterRatio));
      RecalcLayout(clientRect.right, clientRect.bottom);
      m_waveform.Invalidate();
      m_spectral.Invalidate();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  if (y >= m_toolbarRect.top && y < m_toolbarRect.bottom) {
    m_toolbar.SetHover(m_toolbar.HitTest(x, y));
    InvalidateRect(m_hwnd, &m_toolbarRect, FALSE);
  } else {
    m_toolbar.SetHover(-1);
  }

  if (m_fadeDragging != FADE_NONE && m_waveform.HasItem()) {
    double time = m_waveform.XToTime(x);
    double dur = m_waveform.GetItemDuration();

    // Vertical = curvature: drag changes curve shape
    // Fade-in: REAPER dir is inverted vs visual, so flip drag direction
    int dy = y - m_fadeDragStartY;
    double sign = (m_fadeDragging == FADE_IN) ? 1.0 : -1.0;
    double newDir = m_fadeDragStartDir + sign * (double)dy / 100.0;
    newDir = std::max(-1.0, std::min(1.0, newDir));

    if (m_standaloneFadeDrag) {
      auto sf = m_waveform.GetStandaloneFade();
      if (m_fadeDragging == FADE_IN) {
        sf.fadeInLen = std::max(0.0, std::min(time, dur));
        sf.fadeInDir = newDir;
      } else {
        sf.fadeOutLen = std::max(0.0, dur - time);
        sf.fadeOutLen = std::min(sf.fadeOutLen, dur);
        sf.fadeOutDir = newDir;
      }
      m_waveform.SetStandaloneFade(sf);
      m_waveform.SetFadeDragInfo((m_fadeDragging == FADE_IN) ? 1 : 2,
        (m_fadeDragging == FADE_IN) ? sf.fadeInShape : sf.fadeOutShape);
    } else if (g_SetMediaItemInfo_Value) {
      MediaItem* item = m_waveform.GetItem();
      if (m_fadeDragging == FADE_IN) {
        double fadeLen = std::max(0.0, std::min(time, dur));
        g_SetMediaItemInfo_Value(item, "D_FADEINLEN", fadeLen);
        g_SetMediaItemInfo_Value(item, "D_FADEINDIR", newDir);
      } else {
        double fadeLen = std::max(0.0, dur - time);
        fadeLen = std::min(fadeLen, dur);
        g_SetMediaItemInfo_Value(item, "D_FADEOUTLEN", fadeLen);
        g_SetMediaItemInfo_Value(item, "D_FADEOUTDIR", newDir);
      }
      m_waveform.SetFadeDragInfo((m_fadeDragging == FADE_IN) ? 1 : 2,
        (int)g_GetMediaItemInfo_Value(item, (m_fadeDragging == FADE_IN) ? "C_FADEINSHAPE" : "C_FADEOUTSHAPE"));
      if (g_UpdateArrange) g_UpdateArrange();
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (m_spectralFreqDragging) {
    double freq = m_spectral.YToFreq(y, m_spectralFreqDragChTop, m_spectralFreqDragChH);
    m_spectral.UpdateFreqSelection(freq);
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (m_gainPanel.IsDragging()) {
    m_gainPanel.OnMouseMove(x, y, m_waveformRect);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  if (m_dragging && m_waveform.HasItem()) {
    m_waveform.UpdateSelection(m_waveform.XToTime(x));
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  if (m_markers.IsDragging()) {
    m_markers.UpdateDrag(x, m_waveform);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  if (m_scrollbarDragging && m_waveform.HasItem()) {
    int sw = m_scrollbarRect.right - m_scrollbarRect.left;
    if (sw > 0) {
      double deltaTime = ((double)(x - m_lastMouseX) / (double)sw) * m_waveform.GetItemDuration();
      m_waveform.ScrollH(deltaTime);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
  }

  // Update mouse cursor based on what's under pointer
  {
    HCURSOR cur = LoadCursor(nullptr, IDC_ARROW);

    // Minimap resize edge
    if (m_minimapVisible && y >= m_minimapRect.top - 3 && y < m_minimapRect.top + 3) {
      cur = LoadCursor(nullptr, IDC_SIZENS);
    }
    // Splitter
    else if (m_spectralVisible && y >= m_splitterRect.top && y < m_splitterRect.bottom) {
      cur = LoadCursor(nullptr, IDC_SIZENS);
    }
    // Fade handles
    else if (m_waveform.HasItem() && y >= m_waveformRect.top && y < m_waveformRect.bottom) {
      // Solo button
      if (x >= m_soloBtnRect.left && x < m_soloBtnRect.right &&
          y >= m_soloBtnRect.top && y < m_soloBtnRect.bottom) {
        cur = LoadCursor(nullptr, IDC_HAND);
      }
      // Channel buttons (dB scale area, stereo only)
      else if (m_waveform.GetNumChannels() > 1 &&
               x >= m_waveformRect.right - DB_SCALE_WIDTH) {
        cur = LoadCursor(nullptr, IDC_HAND);
      }
      // Gain panel
      else if (m_gainPanel.IsVisible() && m_gainPanel.HitTest(x, y, m_waveformRect)) {
        cur = LoadCursor(nullptr, IDC_HAND);
      }
      // Fade handles (near item edges)
      else if (m_fadeDragging != FADE_NONE) {
        cur = LoadCursor(nullptr, IDC_SIZEWE);
      }
    }
    // Markers in ruler
    else if (y >= m_rulerRect.top && y < m_rulerRect.bottom && m_waveform.HasItem()) {
      if (m_markers.HitTestMarker(x, m_waveform) >= 0) {
        cur = LoadCursor(nullptr, IDC_SIZEWE);
      }
    }
    // Minimap click area
    else if (m_minimapVisible && y >= m_minimapRect.top && y < m_minimapRect.bottom) {
      cur = LoadCursor(nullptr, IDC_HAND);
    }
    // Scrollbar
    else if (y >= m_scrollbarRect.top && y < m_scrollbarRect.bottom) {
      cur = LoadCursor(nullptr, IDC_HAND);
    }

    SetCursor(cur);
  }

  m_lastMouseX = x;
  m_lastMouseY = y;
}

void SneakPeak::OnMouseWheel(int x, int y, int delta, WPARAM wParam)
{
  if (!m_waveform.HasItem()) return;

  bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0;
  bool shift = (LOWORD(wParam) & MK_SHIFT) != 0;
  bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

  double steps = (double)delta / 120.0;

  // Scroll on dB scale column = vertical zoom
  int dbScaleLeft = m_waveformRect.right - DB_SCALE_WIDTH;
  if (x >= dbScaleLeft && x <= m_waveformRect.right &&
      y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    m_waveform.ZoomVertical((float)pow(1.15, steps));
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (alt || shift) {
    m_waveform.ZoomVertical((float)pow(1.15, steps));
  } else if (ctrl) {
    m_waveform.ScrollH(-steps * m_waveform.GetViewDuration() * 0.1);
  } else {
    double centerTime = m_waveform.XToTime(x);
    m_waveform.ZoomHorizontal(pow(ZOOM_FACTOR, steps), centerTime);
  }

  m_spectral.Invalidate();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::OnKeyDown(WPARAM key)
{
  bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

  switch (key) {
    case VK_HOME:
      if (m_waveform.HasItem()) {
        m_waveform.SetCursorTime(0.0);
        if (g_SetEditCurPos) g_SetEditCurPos(m_waveform.RelTimeToAbsTime(0.0), false, false);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    case VK_END:
      if (m_waveform.HasItem()) {
        m_waveform.SetCursorTime(m_waveform.GetItemDuration());
        if (g_SetEditCurPos) g_SetEditCurPos(m_waveform.RelTimeToAbsTime(m_waveform.GetItemDuration()), false, false);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    case VK_SPACE: {
      if (m_waveform.IsStandaloneMode()) { StandalonePlayStop(); break; }
      if (g_GetPlayState && g_OnPlayButton && g_OnStopButton) {
        if (g_GetPlayState() & 1) {
          g_OnStopButton();
        } else if (m_waveform.HasItem() && m_waveform.HasSelection()) {
          // Play selection (sets loop range + plays from start)
          DoLoopSelection();
        } else {
          // No selection: play from cursor
          m_startedPlayback = true;
          m_autoStopped = false;
          m_playGraceTicks = PLAY_GRACE_TICKS;
          g_OnPlayButton();
        }
      }
      break;
    }
    case VK_TAB: {
      bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      NavigateToMarker(!shift);
      break;
    }
    case VK_ESCAPE:
      if (m_waveform.HasSelection()) {
        m_waveform.ClearSelection();
        SyncSelectionToReaper();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (g_OnStopButton) {
        g_OnStopButton();
      }
      break;
    case VK_DELETE:
    case VK_BACK:
      if (ctrl) {
        DoSilence();
      } else {
        DoDelete();
      }
      break;
    case 'A':
      if (ctrl && m_waveform.HasItem()) {
        m_waveform.StartSelection(0.0);
        m_waveform.UpdateSelection(m_waveform.GetItemDuration());
        m_waveform.EndSelection();
        SyncSelectionToReaper();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    case 'C':
      if (ctrl) DoCopy();
      break;
    case 'X':
      if (ctrl) DoCut();
      break;
    case 'V':
      if (ctrl) DoPaste();
      break;
    case 'Z':
      if (ctrl) UndoRestore();
      break;
    case 'S':
    case 's':
      if (ctrl) {
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift && m_waveform.IsStandaloneMode())
          SaveStandaloneFileAs();
        else
          SaveStandaloneFile();
      } else if (!m_waveform.IsStandaloneMode() && m_waveform.HasItem()) {
        SyncSelectionToReaper();
        if (m_waveform.HasSelection() && g_Main_OnCommand) {
          g_Main_OnCommand(40061, 0); // Split at time selection (both edges)
        } else if (g_Main_OnCommand) {
          g_Main_OnCommand(40012, 0); // Split at edit cursor
        }
      }
      break;
    case 'N':
      if (ctrl) DoNormalize();
      break;
    case 'E':
    case 'e':
      if (!ctrl) DoDelete();
      break;
    case 'M':
    case 'm': {
      bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      if (shift && m_waveform.HasSelection()) {
        m_markers.AddRegionFromSelection(m_waveform);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (!ctrl && !shift) {
        m_markers.AddMarkerAtCursor(m_waveform);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    }
    case 'G':
      if (!ctrl) {
        m_gainPanel.Toggle(m_waveform.GetItem());
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
  }
}

// --- Right-click context menu ---

// --- Toolbar actions ---

void SneakPeak::OnToolbarClick(int button)
{
  switch (button) {
    case TB_ZOOM_IN: {
      double center = m_waveform.GetViewStart() + m_waveform.GetViewDuration() / 2.0;
      m_waveform.ZoomHorizontal(ZOOM_FACTOR * 2.0, center);
      break;
    }
    case TB_ZOOM_OUT: {
      double center = m_waveform.GetViewStart() + m_waveform.GetViewDuration() / 2.0;
      m_waveform.ZoomHorizontal(1.0 / (ZOOM_FACTOR * 2.0), center);
      break;
    }
    case TB_ZOOM_FIT:  m_waveform.ZoomToFit(); break;
    case TB_ZOOM_SEL:  m_waveform.ZoomToSelection(); break;
    case TB_PLAY:      if (g_OnPlayButton) g_OnPlayButton(); break;
    case TB_STOP:      if (g_OnStopButton) g_OnStopButton(); break;
    case TB_NORMALIZE: DoNormalize(); break;
    case TB_FADE_IN:   DoFadeIn(); break;
    case TB_FADE_OUT:  DoFadeOut(); break;
    case TB_REVERSE:   DoReverse(); break;
    case TB_VZOOM_IN:  m_waveform.ZoomVertical(1.5f); break;
    case TB_VZOOM_OUT: m_waveform.ZoomVertical(1.0f / 1.5f); break;
    case TB_VZOOM_RESET: m_waveform.ZoomVertical(1.0f / m_waveform.GetVerticalZoom()); break;
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}
