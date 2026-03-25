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

// Helper: portable AppendMenu (SWELL doesn't have it directly)
static void MenuAppend(HMENU menu, unsigned int flags, UINT_PTR id, const char* str)
{
#ifdef _WIN32
  AppendMenuA(menu, flags, id, str);
#else
  // SWELL_Menu_AddMenuItem treats any non-zero flags as MFS_GRAYED,
  // so use InsertMenuItem directly for proper MF_CHECKED support
  MENUITEMINFO mi = { sizeof(mi) };
  mi.fMask = MIIM_ID | MIIM_STATE | MIIM_TYPE;
  mi.fType = MFT_STRING;
  mi.fState = (flags & MF_CHECKED) ? MFS_CHECKED : 0;
  if (flags & MF_GRAYED) mi.fState |= MFS_GRAYED;
  mi.wID = (unsigned int)id;
  mi.dwTypeData = (char*)str;
  InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &mi);
#endif
}

static void MenuAppendSeparator(HMENU menu)
{
#ifdef _WIN32
  AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
#else
  SWELL_Menu_AddMenuItem(menu, "", 0, MF_SEPARATOR);
#endif
}

static void MenuAppendSubmenu(HMENU menu, HMENU submenu, const char* str)
{
#ifdef _WIN32
  AppendMenuA(menu, MF_POPUP, (UINT_PTR)submenu, str);
#else
  InsertMenu(menu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)submenu, str);
#endif
}

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

static const char* FileNameFromPath(const char* path);

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

void SneakPeak::OnPaint(HDC hdc)
{
  if (!hdc) return;

  DrawModeBar(hdc);
  DrawRuler(hdc);
  if (m_masterMode) {
    DrawMasterWaveform(hdc);
  } else {
    m_waveform.Paint(hdc);
  }
  if (m_markers.GetShowMarkers()) m_markers.DrawMarkers(hdc, m_waveformRect, m_rulerRect, m_waveform);
  if (m_waveform.HasItem()) m_gainPanel.Draw(hdc, m_waveformRect);
  if (m_waveform.HasItem()) DrawSoloButton(hdc);
  if (m_spectralVisible) {
    DrawSplitter(hdc);
    m_spectral.Paint(hdc, m_waveform);
  }
  if (m_minimapVisible) m_minimap.Paint(hdc, m_waveform);
  DrawScrollbar(hdc);
  DrawBottomPanel(hdc);
  DrawToast(hdc);
}

void SneakPeak::DrawSplitter(HDC hdc)
{
  if (m_splitterRect.bottom <= m_splitterRect.top) return;

  HBRUSH bg = CreateSolidBrush(RGB(45, 45, 45));
  FillRect(hdc, &m_splitterRect, bg);
  DeleteObject(bg);

  // Grip dots in center
  int cx = (m_splitterRect.left + m_splitterRect.right) / 2;
  int cy = (m_splitterRect.top + m_splitterRect.bottom) / 2;
  HBRUSH dot = CreateSolidBrush(RGB(120, 120, 120));
  for (int dx = -12; dx <= 12; dx += 6) {
    RECT d = { cx + dx - 1, cy - 1, cx + dx + 2, cy + 2 };
    FillRect(hdc, &d, dot);
  }
  DeleteObject(dot);
}

// Helper: extract filename from path
static const char* FileNameFromPath(const char* path)
{
  const char* slash = strrchr(path, '/');
  if (!slash) slash = strrchr(path, '\\');
  return slash ? slash + 1 : path;
}

void SneakPeak::DrawModeBar(HDC hdc)
{
  int w = m_modeBarRect.right - m_modeBarRect.left;
  int h = m_modeBarRect.bottom - m_modeBarRect.top;
  if (w <= 0 || h <= 0) return;

  // Background
  HBRUSH bgBrush = CreateSolidBrush(g_theme.modeBarBg);
  FillRect(hdc, &m_modeBarRect, bgBrush);
  DeleteObject(bgBrush);

  // Bottom border
  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_modeBarRect.left, m_modeBarRect.bottom - 1, nullptr);
  LineTo(hdc, m_modeBarRect.right, m_modeBarRect.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(hdc, TRANSPARENT);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);

  m_modeBarTabs.clear();
  int xPos = 6;
  int yMid = m_modeBarRect.top + h / 2;

  bool isStandalone = m_waveform.IsStandaloneMode() || !m_standaloneFiles.empty();
  bool isReaper = m_waveform.HasItem() && !m_waveform.IsStandaloneMode();
  bool isEmpty = !m_waveform.HasItem() && m_standaloneFiles.empty();

  // MASTER tab — always available (on the right side, drawn later)
  // We'll draw it after the main content

  if (isEmpty && !m_masterMode) {
    // Empty state
    SetTextColor(hdc, g_theme.emptyText);
    RECT textR = { xPos, m_modeBarRect.top, m_modeBarRect.right - 80, m_modeBarRect.bottom };
    DrawText(hdc, "SneakPeak - Drop audio file or select item", -1, &textR,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else if (isEmpty && m_masterMode) {
    // Master mode active — show indicator
    COLORREF accent = RGB(200, 80, 80);
    HBRUSH accentBrush = CreateSolidBrush(accent);
    RECT dot = { xPos, yMid - 3, xPos + 7, yMid + 4 };
    FillRect(hdc, &dot, accentBrush);
    DeleteObject(accentBrush);
    xPos += 12;
    SetTextColor(hdc, accent);
    RECT labelR = { xPos, m_modeBarRect.top, xPos + 80, m_modeBarRect.bottom };
    DrawText(hdc, "MASTER", -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else {
    // Mode indicator
    COLORREF accent = isStandalone && !isReaper ? g_theme.modeBarStandaloneAccent : g_theme.modeBarReaperAccent;
    const char* modeLabel = isStandalone && !isReaper ? "STANDALONE" : "REAPER";

    // Draw indicator dot/diamond
    HBRUSH accentBrush = CreateSolidBrush(accent);
    if (isStandalone && !isReaper) {
      // Orange filled circle (small rounded rect)
      RECT dot = { xPos, yMid - 3, xPos + 7, yMid + 4 };
      FillRect(hdc, &dot, accentBrush);
    } else {
      // Blue diamond — draw as small rotated square using lines
      int cx = xPos + 3, cy = yMid;
      POINT diamond[4] = { {cx, cy-4}, {cx+4, cy}, {cx, cy+4}, {cx-4, cy} };
      HPEN acPen = CreatePen(PS_SOLID, 1, accent);
      HPEN prevPen = (HPEN)SelectObject(hdc, acPen);
      HBRUSH prevBr = (HBRUSH)SelectObject(hdc, accentBrush);
      Polygon(hdc, diamond, 4);
      SelectObject(hdc, prevPen);
      SelectObject(hdc, prevBr);
      DeleteObject(acPen);
    }
    DeleteObject(accentBrush);
    xPos += 12;

    // Mode label
    SetTextColor(hdc, accent);
    RECT labelR = { xPos, m_modeBarRect.top, xPos + 80, m_modeBarRect.bottom };
    DrawText(hdc, modeLabel, -1, &labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT labelMeasure = { 0, 0, 200, 20 };
    DrawText(hdc, modeLabel, -1, &labelMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    xPos += (labelMeasure.right - labelMeasure.left) + 8;

    // Separator
    HPEN sepPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HPEN sPrev = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, xPos, m_modeBarRect.top + 3, nullptr);
    LineTo(hdc, xPos, m_modeBarRect.bottom - 3);
    SelectObject(hdc, sPrev);
    DeleteObject(sepPen);
    xPos += 8;

    // Switch to normal weight for tabs
    oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

    int tabAreaRight = m_modeBarRect.right - 8;

    // REAPER pseudo-tab (if we have standalone files and we're in REAPER mode, or for switching back)
    if (isReaper && !m_standaloneFiles.empty()) {
      // Active REAPER tab
      char reaperLabel[256] = "REAPER item";
      if (m_waveform.HasItem()) {
        const char* fp = m_waveform.GetStandaloneFilePath().c_str();
        if (fp[0]) {
          snprintf(reaperLabel, sizeof(reaperLabel), "%s", FileNameFromPath(fp));
        } else {
          // Get item name from waveform
          char buf[256];
          GetItemTitle(buf, sizeof(buf));
          if (buf[0]) snprintf(reaperLabel, sizeof(reaperLabel), "%s", buf);
        }
      }
      RECT tsR = { 0, 0, 300, 20 };
      DrawText(hdc, reaperLabel, -1, &tsR, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      int tw = std::min((int)(tsR.right - tsR.left) + 12, MODE_TAB_MAX_W);

      RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
      HBRUSH tabBg = CreateSolidBrush(g_theme.modeBarActiveTab);
      FillRect(hdc, &tabR, tabBg);
      DeleteObject(tabBg);

      // Blue accent underline
      HPEN ulPen = CreatePen(PS_SOLID, 2, g_theme.modeBarReaperAccent);
      HPEN ulPrev = (HPEN)SelectObject(hdc, ulPen);
      MoveToEx(hdc, tabR.left, tabR.bottom - 1, nullptr);
      LineTo(hdc, tabR.right, tabR.bottom - 1);
      SelectObject(hdc, ulPrev);
      DeleteObject(ulPen);

      SetTextColor(hdc, RGB(220, 220, 220));
      RECT textR = { tabR.left + 6, tabR.top, tabR.right - 6, tabR.bottom };
      DrawText(hdc, reaperLabel, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

      ModeBarTab mbt;
      mbt.rect = tabR;
      mbt.closeRect = {};
      mbt.fileIdx = -1;
      mbt.isReaper = true;
      m_modeBarTabs.push_back(mbt);
      xPos = tabR.right + 2;
    }

    // Standalone file tabs
    for (int i = 0; i < (int)m_standaloneFiles.size() && xPos < tabAreaRight; i++) {
      const auto& fs = m_standaloneFiles[i];
      const char* fname = FileNameFromPath(fs.filePath.c_str());
      bool isDirty = (m_waveform.IsStandaloneMode() && i == m_activeFileIdx) ? m_dirty : fs.dirty;
      char label[160];
      if (isDirty)
        snprintf(label, sizeof(label), "*%s", fname);
      else
        snprintf(label, sizeof(label), "%s", fname);

      RECT tsR2 = { 0, 0, 300, 20 };
      DrawText(hdc, label, -1, &tsR2, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      bool isActive = (m_waveform.IsStandaloneMode() && i == m_activeFileIdx);
      int closeW = isActive ? MODE_TAB_CLOSE_SIZE : 0;
      int tw = std::min((int)(tsR2.right - tsR2.left) + 12 + closeW, MODE_TAB_MAX_W);
      if (xPos + tw > tabAreaRight) tw = tabAreaRight - xPos;
      if (tw < 20) break;

      RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
      HBRUSH tabBg = CreateSolidBrush(isActive ? g_theme.modeBarActiveTab : g_theme.modeBarInactiveTab);
      FillRect(hdc, &tabR, tabBg);
      DeleteObject(tabBg);

      if (isActive) {
        // Orange accent underline for active standalone tab
        HPEN ulPen = CreatePen(PS_SOLID, 2, g_theme.modeBarStandaloneAccent);
        HPEN ulPrev = (HPEN)SelectObject(hdc, ulPen);
        MoveToEx(hdc, tabR.left, tabR.bottom - 1, nullptr);
        LineTo(hdc, tabR.right, tabR.bottom - 1);
        SelectObject(hdc, ulPrev);
        DeleteObject(ulPen);
      }

      SetTextColor(hdc, isActive ? RGB(220, 220, 220) : g_theme.modeBarText);
      int textRight = tabR.right - 4 - closeW;
      RECT textR = { tabR.left + 6, tabR.top, textRight, tabR.bottom };
      DrawText(hdc, label, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

      // Close button on active tab
      RECT closeR = {};
      if (isActive) {
        int csz = MODE_TAB_CLOSE_SIZE - 4;
        int cx = tabR.right - MODE_TAB_CLOSE_SIZE + 1;
        int cy = yMid - csz / 2;
        closeR = { cx, cy, cx + csz, cy + csz };
        SetTextColor(hdc, g_theme.modeBarText);
        DrawText(hdc, "x", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }

      ModeBarTab mbt;
      mbt.rect = tabR;
      mbt.closeRect = closeR;
      mbt.fileIdx = i;
      mbt.isReaper = false;
      m_modeBarTabs.push_back(mbt);
      xPos = tabR.right + 2;
    }

    // If in standalone mode with no REAPER item showing, but we had a REAPER item before,
    // show a "REAPER" pseudo-tab for switching back
    if (!isReaper && !m_standaloneFiles.empty()) {
      // Check if there's a REAPER item available
      if (g_CountSelectedMediaItems && g_CountSelectedMediaItems(nullptr) > 0) {
        const char* rl = "REAPER";
        RECT tsR3 = { 0, 0, 300, 20 };
        DrawText(hdc, rl, -1, &tsR3, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int tw = std::min((int)(tsR3.right - tsR3.left) + 12, MODE_TAB_MAX_W);
        if (xPos + tw <= tabAreaRight) {
          RECT tabR = { xPos, m_modeBarRect.top + 1, xPos + tw, m_modeBarRect.bottom - 1 };
          HBRUSH tabBg = CreateSolidBrush(g_theme.modeBarInactiveTab);
          FillRect(hdc, &tabR, tabBg);
          DeleteObject(tabBg);

          SetTextColor(hdc, g_theme.modeBarReaperAccent);
          RECT textR = { tabR.left + 6, tabR.top, tabR.right - 4, tabR.bottom };
          DrawText(hdc, rl, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

          ModeBarTab mbt;
          mbt.rect = tabR;
          mbt.closeRect = {};
          mbt.fileIdx = -1;
          mbt.isReaper = true;
          m_modeBarTabs.push_back(mbt);
        }
      }
    }
  }

  // MASTER tab — right-aligned, always visible
  {
    SelectObject(hdc, g_fonts.normal11);
    const char* ml = "MASTER";
    RECT tsM = { 0, 0, 200, 20 };
    DrawText(hdc, ml, -1, &tsM, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int tw = (int)(tsM.right - tsM.left) + 14;
    int tabRight = m_modeBarRect.right - 6;
    int tabLeft = tabRight - tw;

    RECT tabR = { tabLeft, m_modeBarRect.top + 1, tabRight, m_modeBarRect.bottom - 1 };
    HBRUSH tabBg = CreateSolidBrush(m_masterMode ? g_theme.modeBarActiveTab : g_theme.modeBarInactiveTab);
    FillRect(hdc, &tabR, tabBg);
    DeleteObject(tabBg);

    if (m_masterMode) {
      HPEN ulPen = CreatePen(PS_SOLID, 2, RGB(200, 80, 80));
      HPEN ulPrev = (HPEN)SelectObject(hdc, ulPen);
      MoveToEx(hdc, tabR.left, tabR.bottom - 1, nullptr);
      LineTo(hdc, tabR.right, tabR.bottom - 1);
      SelectObject(hdc, ulPrev);
      DeleteObject(ulPen);
    }

    SetTextColor(hdc, m_masterMode ? RGB(220, 220, 220) : RGB(200, 80, 80));
    RECT textR = { tabR.left + 7, tabR.top, tabR.right - 7, tabR.bottom };
    DrawText(hdc, ml, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ModeBarTab mbt;
    mbt.rect = tabR;
    mbt.closeRect = {};
    mbt.fileIdx = -2; // special: MASTER tab
    mbt.isReaper = false;
    m_modeBarTabs.push_back(mbt);
  }

  SelectObject(hdc, oldFont);
}

void SneakPeak::DrawRuler(HDC hdc)
{
  int w = m_rulerRect.right - m_rulerRect.left;
  int h = m_rulerRect.bottom - m_rulerRect.top;

  HBRUSH bg = CreateSolidBrush(g_theme.rulerBg);
  FillRect(hdc, &m_rulerRect, bg);
  DeleteObject(bg);

  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_rulerRect.left, m_rulerRect.bottom - 1, nullptr);
  LineTo(hdc, m_rulerRect.right, m_rulerRect.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  if (!m_waveform.HasItem()) return;

  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();
  if (viewDur <= 0) return;

  double pixelsPerSec = (double)w / viewDur;

  static const double intervals[] = {
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
    1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0
  };
  double tickInterval = 300.0;
  for (double iv : intervals) {
    if (iv * pixelsPerSec >= 80.0) { tickInterval = iv; break; }
  }

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, g_theme.rulerText);

  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

  HPEN tickPen = CreatePen(PS_SOLID, 1, g_theme.rulerTick);
  HPEN minorPen = CreatePen(PS_SOLID, 1, g_theme.rulerTickMinor);

  double firstTick = floor(viewStart / tickInterval) * tickInterval;
  int y = m_rulerRect.top;

  for (double t = firstTick; t < viewStart + viewDur; t += tickInterval) {
    if (t < 0) continue;
    int tx = m_waveform.TimeToX(t);
    if (tx < m_rulerRect.left || tx >= m_rulerRect.right) continue;

    HPEN op = (HPEN)SelectObject(hdc, tickPen);
    MoveToEx(hdc, tx, y + h - 8, nullptr);
    LineTo(hdc, tx, y + h - 1);
    SelectObject(hdc, op);

    // Format as HH:MM:SS;ms
    char label[32];
    {
      int totalSec = (int)t;
      int hours = totalSec / 3600;
      int mins = (totalSec % 3600) / 60;
      int secs = totalSec % 60;
      int ms = (int)((t - totalSec) * 1000.0 + 0.5);
      if (ms >= 1000) { ms -= 1000; secs++; }

      if (tickInterval >= 60.0) {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;00", hours, mins, secs);
      } else if (tickInterval >= 1.0) {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;00", hours, mins, secs);
      } else if (tickInterval >= 0.01) {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;%02d", hours, mins, secs, ms / 10);
      } else {
        snprintf(label, sizeof(label), "%02d;%02d;%02d;%03d", hours, mins, secs, ms);
      }
    }
    RECT textRect = { tx + 3, y + 2, tx + 80, y + h - 8 };
    DrawText(hdc, label, -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    double minorIv = tickInterval / 5.0;
    for (int mi = 1; mi < 5; mi++) {
      int mx = m_waveform.TimeToX(t + mi * minorIv);
      if (mx >= m_rulerRect.left && mx < m_rulerRect.right) {
        HPEN op2 = (HPEN)SelectObject(hdc, minorPen);
        MoveToEx(hdc, mx, y + h - 4, nullptr);
        LineTo(hdc, mx, y + h - 1);
        SelectObject(hdc, op2);
      }
    }
  }

  DeleteObject(tickPen);
  DeleteObject(minorPen);
  SelectObject(hdc, oldFont);
}

void SneakPeak::DrawScrollbar(HDC hdc)
{
  HBRUSH bg = CreateSolidBrush(g_theme.scrollbarBg);
  FillRect(hdc, &m_scrollbarRect, bg);
  DeleteObject(bg);

  if (!m_waveform.HasItem() || m_waveform.GetItemDuration() <= 0) return;

  int sw = m_scrollbarRect.right - m_scrollbarRect.left;
  double totalDur = m_waveform.GetItemDuration();
  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();

  double startRatio = std::max(0.0, viewStart / totalDur);
  double endRatio = std::min(1.0, (viewStart + viewDur) / totalDur);

  int thumbX = m_scrollbarRect.left + (int)(startRatio * (double)sw);
  int thumbW = std::max(20, (int)((endRatio - startRatio) * (double)sw));

  COLORREF thumbColor = m_scrollbarDragging ? g_theme.scrollbarHover : g_theme.scrollbarThumb;
  RECT thumbRect = { thumbX, m_scrollbarRect.top + 2, thumbX + thumbW, m_scrollbarRect.bottom - 2 };
  HBRUSH thumbBrush = CreateSolidBrush(thumbColor);
  FillRect(hdc, &thumbRect, thumbBrush);
  DeleteObject(thumbBrush);
}

static void FormatTimeHMS(double sec, char* buf, int sz)
{
  if (sec < 0) sec = 0;
  int totalMs = static_cast<int>(sec * 1000.0 + 0.5);
  int ms = totalMs % 1000;
  int totalSec = totalMs / 1000;
  int s = totalSec % 60;
  int m = (totalSec / 60) % 60;
  int h = totalSec / 3600;
  snprintf(buf, sz, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

// --- Solo button ---

void SneakPeak::DrawSoloButton(HDC hdc)
{
  if (!m_waveform.HasItem()) return;

  // Position: top-right of waveform area, well left of dB scale and fade handles
  int btnW = 22, btnH = 16;
  int btnX = m_waveformRect.right - DB_SCALE_WIDTH - btnW - 30;
  int btnY = m_waveformRect.top + 10;
  m_soloBtnRect = { btnX, btnY, btnX + btnW, btnY + btnH };

  // Background
  COLORREF bgCol = m_trackSoloed ? RGB(200, 180, 0) : RGB(50, 50, 50);
  HBRUSH bg = CreateSolidBrush(bgCol);
  FillRect(hdc, &m_soloBtnRect, bg);
  DeleteObject(bg);

  // Border
  COLORREF borderCol = m_trackSoloed ? RGB(240, 220, 0) : RGB(80, 80, 80);
  HPEN pen = CreatePen(PS_SOLID, 1, borderCol);
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, m_soloBtnRect.left, m_soloBtnRect.top, nullptr);
  LineTo(hdc, m_soloBtnRect.right - 1, m_soloBtnRect.top);
  LineTo(hdc, m_soloBtnRect.right - 1, m_soloBtnRect.bottom - 1);
  LineTo(hdc, m_soloBtnRect.left, m_soloBtnRect.bottom - 1);
  LineTo(hdc, m_soloBtnRect.left, m_soloBtnRect.top);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Label "S"
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.bold12);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, m_trackSoloed ? RGB(0, 0, 0) : RGB(140, 140, 140));
  DrawText(hdc, "S", 1, &m_soloBtnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(hdc, oldFont);
}

bool SneakPeak::ClickSoloButton(int x, int y)
{
  if (!m_waveform.HasItem()) return false;
  return x >= m_soloBtnRect.left && x < m_soloBtnRect.right &&
         y >= m_soloBtnRect.top && y < m_soloBtnRect.bottom;
}

void SneakPeak::ToggleTrackSolo()
{
  if (!g_GetMediaItem_Track || !g_GetSetMediaTrackInfo) return;

  // Collect unique tracks from all segments (multi-item cross-track support)
  std::vector<MediaTrack*> tracks;
  const auto& segs = m_waveform.GetSegments();
  if (segs.size() > 1) {
    for (const auto& seg : segs) {
      if (!seg.item) continue;
      MediaTrack* tr = g_GetMediaItem_Track(seg.item);
      if (!tr) continue;
      bool found = false;
      for (auto* t : tracks) { if (t == tr) { found = true; break; } }
      if (!found) tracks.push_back(tr);
    }
  } else {
    MediaItem* item = m_waveform.GetItem();
    if (!item) return;
    MediaTrack* tr = g_GetMediaItem_Track(item);
    if (tr) tracks.push_back(tr);
  }
  if (tracks.empty()) return;

  // Check if any track is currently soloed — toggle all together
  bool anySoloed = false;
  for (auto* tr : tracks) {
    int* pSolo = (int*)g_GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
    if (pSolo && *pSolo != 0) { anySoloed = true; break; }
  }

  int newSolo = anySoloed ? 0 : 2;  // 2 = solo-in-place (SIP)
  for (auto* tr : tracks)
    g_GetSetMediaTrackInfo(tr, "I_SOLO", &newSolo);

  m_trackSoloed = (newSolo != 0);
  if (g_UpdateArrange) g_UpdateArrange();
}

void SneakPeak::UpdateSoloState()
{
  if (!g_GetMediaItem_Track || !g_GetSetMediaTrackInfo || !m_waveform.HasItem()) {
    m_trackSoloed = false;
    return;
  }

  // Collect unique tracks from all segments
  std::vector<MediaTrack*> tracks;
  const auto& segs = m_waveform.GetSegments();
  if (segs.size() > 1) {
    for (const auto& seg : segs) {
      if (!seg.item) continue;
      if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)seg.item, "MediaItem*")) continue;
      MediaTrack* tr = g_GetMediaItem_Track(seg.item);
      if (!tr) continue;
      bool found = false;
      for (auto* t : tracks) { if (t == tr) { found = true; break; } }
      if (!found) tracks.push_back(tr);
    }
  } else {
    MediaItem* item = m_waveform.GetItem();
    if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)item, "MediaItem*")) {
      m_trackSoloed = false;
      return;
    }
    MediaTrack* tr = g_GetMediaItem_Track(item);
    if (tr) tracks.push_back(tr);
  }

  // Soloed if any of our tracks is soloed
  m_trackSoloed = false;
  for (auto* tr : tracks) {
    int* pSolo = (int*)g_GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
    if (pSolo && *pSolo != 0) { m_trackSoloed = true; break; }
  }
}

void SneakPeak::DrawMasterWaveform(HDC hdc)
{
  RECT r = m_waveformRect;
  int scaleLeft = r.right - DB_SCALE_WIDTH;
  int waveRight = scaleLeft; // waveform ends before dB scale
  int w = waveRight - r.left;
  int h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return;

  // Dark background (full rect including scale area)
  HBRUSH bgBrush = CreateSolidBrush(g_theme.waveformBg);
  FillRect(hdc, &r, bgBrush);
  DeleteObject(bgBrush);

  int centerY = r.top + h / 2;
  float halfH = (float)(h / 2) * 0.9f; // 90% of half-height

  // Center line
  HPEN centerPen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
  HPEN oldPen = (HPEN)SelectObject(hdc, centerPen);
  MoveToEx(hdc, r.left, centerY, nullptr);
  LineTo(hdc, waveRight, centerY);
  SelectObject(hdc, oldPen);
  DeleteObject(centerPen);

  if (m_masterPeakCount == 0) {
    // No data yet — show label
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_theme.emptyText);
    RECT textRect = r;
    DrawText(hdc, "Master Output — play to see waveform", -1, &textRect,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    return;
  }

  // Draw rolling waveform — most recent samples on the right
  int count = m_masterPeakCount;
  int columnsToShow = std::min(w, count);

  // L channel (top half) — green, red above 0dB
  HPEN greenPenL = CreatePen(PS_SOLID, 1, RGB(40, 160, 60));
  HPEN redPen = CreatePen(PS_SOLID, 1, RGB(220, 50, 50));
  int clipY0dBTop = centerY - (int)(1.0f * halfH);
  int clipY0dBBot = centerY + (int)(1.0f * halfH);
  HPEN prevPen;

  for (int col = 0; col < columnsToShow; col++) {
    int bufIdx = (m_masterPeakHead - columnsToShow + col + MASTER_ROLLING_SIZE) % MASTER_ROLLING_SIZE;
    float pkL = m_masterPeakBufL[bufIdx];

    int x = waveRight - columnsToShow + col;
    int yL = centerY - (int)(pkL * halfH);
    if (yL < r.top) yL = r.top;

    if (pkL > 1.0f) {
      // Green part up to 0dB, red above
      prevPen = (HPEN)SelectObject(hdc, greenPenL);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, clipY0dBTop);
      SelectObject(hdc, redPen);
      MoveToEx(hdc, x, clipY0dBTop, nullptr);
      LineTo(hdc, x, yL);
      SelectObject(hdc, prevPen);
    } else {
      prevPen = (HPEN)SelectObject(hdc, greenPenL);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, yL);
      SelectObject(hdc, prevPen);
    }
  }

  // R channel (bottom half)
  HPEN greenPenR = CreatePen(PS_SOLID, 1, RGB(30, 140, 50));

  for (int col = 0; col < columnsToShow; col++) {
    int bufIdx = (m_masterPeakHead - columnsToShow + col + MASTER_ROLLING_SIZE) % MASTER_ROLLING_SIZE;
    float pkR = m_masterPeakBufR[bufIdx];

    int x = waveRight - columnsToShow + col;
    int yR = centerY + (int)(pkR * halfH);
    if (yR > r.bottom) yR = r.bottom;

    if (pkR > 1.0f) {
      prevPen = (HPEN)SelectObject(hdc, greenPenR);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, clipY0dBBot);
      SelectObject(hdc, redPen);
      MoveToEx(hdc, x, clipY0dBBot, nullptr);
      LineTo(hdc, x, yR);
      SelectObject(hdc, prevPen);
    } else {
      prevPen = (HPEN)SelectObject(hdc, greenPenR);
      MoveToEx(hdc, x, centerY, nullptr);
      LineTo(hdc, x, yR);
      SelectObject(hdc, prevPen);
    }
  }

  DeleteObject(greenPenL);
  DeleteObject(greenPenR);
  DeleteObject(redPen);

  // Clip line at 0dB (1.0 linear)
  int clipYTop = centerY - (int)(1.0f * halfH);
  int clipYBot = centerY + (int)(1.0f * halfH);
  HPEN clipPen = CreatePen(PS_SOLID, 1, RGB(100, 40, 40));
  prevPen = (HPEN)SelectObject(hdc, clipPen);
  MoveToEx(hdc, r.left, clipYTop, nullptr);
  LineTo(hdc, waveRight, clipYTop);
  MoveToEx(hdc, r.left, clipYBot, nullptr);
  LineTo(hdc, waveRight, clipYBot);
  SelectObject(hdc, prevPen);
  DeleteObject(clipPen);

  // "MASTER" label
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(100, 100, 100));
  RECT lblRect = { r.left + 6, r.top + 4, r.left + 120, r.top + 20 };
  DrawText(hdc, "MASTER", -1, &lblRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

  // dB scale (right column)
  RECT colRect = { scaleLeft, r.top, r.right, r.bottom };
  HBRUSH colBrush = CreateSolidBrush(RGB(25, 25, 25));
  FillRect(hdc, &colRect, colBrush);
  DeleteObject(colBrush);

  HPEN borderPen2 = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
  HPEN oldPen2 = (HPEN)SelectObject(hdc, borderPen2);
  MoveToEx(hdc, scaleLeft, r.top, nullptr);
  LineTo(hdc, scaleLeft, r.bottom);
  SelectObject(hdc, oldPen2);
  DeleteObject(borderPen2);

  SetTextColor(hdc, g_theme.dbScaleText);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);

  RECT hdrRect = { scaleLeft + 2, r.top + 1, r.right - 2, r.top + 13 };
  DrawText(hdc, "dB", -1, &hdrRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // -∞ at center
  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
  HPEN tpOld = (HPEN)SelectObject(hdc, tickPen);
  MoveToEx(hdc, scaleLeft + 1, centerY, nullptr);
  LineTo(hdc, scaleLeft + 5, centerY);
  SelectObject(hdc, tpOld);
  RECT infR = { scaleLeft + 5, centerY - 6, r.right - 2, centerY + 6 };
  DrawText(hdc, "-\xE2\x88\x9E", -1, &infR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  static const double dbVals[] = { -48, -36, -24, -18, -12, -6, -3, 0 };
  int lastYTop = centerY, lastYBot = centerY;
  for (double db : dbVals) {
    double lin = pow(10.0, db / 20.0);
    int yOff = (int)(lin * (double)halfH);
    if (yOff < 1) continue;

    // Top half
    int yt = centerY - yOff;
    if (yt > r.top + 2 && lastYTop - yt >= 13) {
      tpOld = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, yt, nullptr);
      LineTo(hdc, scaleLeft + 5, yt);
      SelectObject(hdc, tpOld);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", (int)db);
      RECT tr = { scaleLeft + 5, yt - 6, r.right - 2, yt + 6 };
      DrawText(hdc, lbl, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      lastYTop = yt;
    }
    // Bottom half
    int yb = centerY + yOff;
    if (yb < r.bottom - 2 && yb - lastYBot >= 13) {
      tpOld = (HPEN)SelectObject(hdc, tickPen);
      MoveToEx(hdc, scaleLeft + 1, yb, nullptr);
      LineTo(hdc, scaleLeft + 5, yb);
      SelectObject(hdc, tpOld);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", (int)db);
      RECT tr = { scaleLeft + 5, yb - 6, r.right - 2, yb + 6 };
      DrawText(hdc, lbl, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      lastYBot = yb;
    }
  }
  DeleteObject(tickPen);
  SelectObject(hdc, oldFont);
}

void SneakPeak::DrawBottomPanel(HDC hdc)
{
  // Dark background for entire bottom panel
  HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24));
  FillRect(hdc, &m_bottomPanelRect, bg);
  DeleteObject(bg);

  // Top border
  HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
  HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
  MoveToEx(hdc, m_bottomPanelRect.left, m_bottomPanelRect.top, nullptr);
  LineTo(hdc, m_bottomPanelRect.right, m_bottomPanelRect.top);
  SelectObject(hdc, oldPen);
  DeleteObject(borderPen);

  int panelW = m_bottomPanelRect.right - m_bottomPanelRect.left;
  int infoW = panelW * 35 / 100; // 35% for info, 65% for RMS
  if (infoW > 320) infoW = 320;
  int dividerX = m_bottomPanelRect.right - infoW;

  // Meters on the left (fills most space)
  RECT metersRect = { m_bottomPanelRect.left, m_bottomPanelRect.top + 1,
                      dividerX - 1, m_bottomPanelRect.bottom };
  m_metersRect = metersRect;
  int meterCh = (m_masterMode || m_meterFromMaster) ? 2 : m_waveform.GetNumChannels();
  m_levels.Draw(hdc, metersRect, meterCh);

  if (!m_waveform.HasItem() && !m_masterMode) return;

  // Divider line
  HPEN divPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
  HPEN prevDivPen = (HPEN)SelectObject(hdc, divPen);
  MoveToEx(hdc, dividerX, m_bottomPanelRect.top + 1, nullptr);
  LineTo(hdc, dividerX, m_bottomPanelRect.bottom);
  SelectObject(hdc, prevDivPen);
  DeleteObject(divPen);

  // Info text on the right side
  SetBkMode(hdc, TRANSPARENT);
  int infoLeft = dividerX + 6;
  int infoRight = m_bottomPanelRect.right - 4;
  int panelTop = m_bottomPanelRect.top + 2;
  int panelBot = m_bottomPanelRect.bottom - 1;
  int rowH = (panelBot - panelTop) / 3;

  // Row 1: Selection / Cursor
  {
    RECT r = { infoLeft, panelTop, infoRight, panelTop + rowH };
    char line[256];
    if (m_waveform.HasSelection()) {
      WaveformSelection sel = m_waveform.GetSelection();
      char sStart[16], sEnd[16], sDur[16];
      FormatTimeHMS(sel.startTime, sStart, sizeof(sStart));
      FormatTimeHMS(sel.endTime, sEnd, sizeof(sEnd));
      FormatTimeHMS(sel.endTime - sel.startTime, sDur, sizeof(sDur));
      snprintf(line, sizeof(line), "Sel: %s - %s  Dur: %s", sStart, sEnd, sDur);
      SetTextColor(hdc, RGB(210, 210, 210));
    } else {
      char sCur[16];
      FormatTimeHMS(m_waveform.GetCursorTime(), sCur, sizeof(sCur));
      snprintf(line, sizeof(line), "Cursor: %s", sCur);
      SetTextColor(hdc, RGB(170, 170, 170));
    }
    DrawText(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Row 2: View range
  {
    RECT r = { infoLeft, panelTop + rowH, infoRight, panelTop + rowH * 2 };
    char vStart[16], vEnd[16], vDur[16];
    FormatTimeHMS(m_waveform.GetViewStart(), vStart, sizeof(vStart));
    FormatTimeHMS(m_waveform.GetViewEnd(), vEnd, sizeof(vEnd));
    FormatTimeHMS(m_waveform.GetViewDuration(), vDur, sizeof(vDur));
    char line[256];
    snprintf(line, sizeof(line), "View: %s - %s  Dur: %s", vStart, vEnd, vDur);
    SetTextColor(hdc, RGB(140, 140, 140));
    DrawText(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // Row 3: Format info
  {
    RECT r = { infoLeft, panelTop + rowH * 2, infoRight, panelBot };

    double fileSizeMB = m_cachedFileSizeMB;

    char tTotal[16];
    FormatTimeHMS(m_waveform.GetItemDuration(), tTotal, sizeof(tTotal));

    const char* fmtName = (m_wavAudioFormat == 3) ? "Float" : "PCM";
    char line[256];
    snprintf(line, sizeof(line), "%dHz %s%d %dCh  %.1fMB  %s",
             m_waveform.GetSampleRate(), fmtName, m_wavBitsPerSample,
             m_waveform.GetNumChannels(), fileSizeMB, tTotal);
    SetTextColor(hdc, RGB(110, 110, 110));
    DrawText(hdc, line, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

}

void SneakPeak::ShowToast(const char* text)
{
  snprintf(m_toastText, sizeof(m_toastText), "%s", text);
  m_toastStartTick = GetTickCount();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::DrawToast(HDC hdc)
{
  if (!m_toastStartTick) return;
  DWORD elapsed = GetTickCount() - m_toastStartTick;
  if (elapsed > 2000) { m_toastStartTick = 0; return; }

  // Fade out in last 500ms
  int alpha = 255;
  if (elapsed > 1500) alpha = 255 - (int)(255.0 * (elapsed - 1500) / 500.0);

  // Draw centered pill on waveform area
  int textLen = (int)strlen(m_toastText);
  if (textLen == 0) return;

  int pillW = textLen * 9 + 24;
  int pillH = 26;
  int cx = (m_waveformRect.left + m_waveformRect.right) / 2;
  int cy = m_waveformRect.top + 30;
  RECT pill = { cx - pillW/2, cy - pillH/2, cx + pillW/2, cy + pillH/2 };

  // Background
  int bg = (alpha * 40) / 255;
  HBRUSH bgBrush = CreateSolidBrush(RGB(bg, bg + bg/4, bg));
  FillRect(hdc, &pill, bgBrush);
  DeleteObject(bgBrush);

  // Border
  HPEN pen = CreatePen(PS_SOLID, 1, RGB((alpha*80)/255, (alpha*80)/255, (alpha*80)/255));
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, pill.left, pill.top, nullptr);
  LineTo(hdc, pill.right, pill.top);
  LineTo(hdc, pill.right, pill.bottom);
  LineTo(hdc, pill.left, pill.bottom);
  LineTo(hdc, pill.left, pill.top);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Text
  SetBkMode(hdc, TRANSPARENT);
  int g = (alpha * 230) / 255;
  SetTextColor(hdc, RGB(g, g, g));
  DrawText(hdc, m_toastText, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

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

void SneakPeak::OnRightClick(int x, int y)
{
  // Right-click on bottom panel (meters) → meter mode menu
  POINT ptTest = { x, y };
  if (PtInRect(&m_bottomPanelRect, ptTest)) {
    HMENU meterMenu = CreatePopupMenu();
    if (!meterMenu) return;
    MeterMode cur = m_levels.GetMode();
    MenuAppend(meterMenu, MF_STRING | (cur == MeterMode::PEAK ? MF_CHECKED : 0), CM_METER_PEAK, "Peak (PPM)");
    MenuAppend(meterMenu, MF_STRING | (cur == MeterMode::RMS  ? MF_CHECKED : 0), CM_METER_RMS,  "RMS (AES/EBU)");
    MenuAppend(meterMenu, MF_STRING | (cur == MeterMode::VU   ? MF_CHECKED : 0), CM_METER_VU,   "VU");
    MenuAppendSeparator(meterMenu);
    MenuAppend(meterMenu, MF_STRING | (m_meterFromMaster ? MF_CHECKED : 0), CM_METER_SOURCE_MASTER, "Master Output");
    POINT pt = { x, y };
    ClientToScreen(m_hwnd, &pt);
    TrackPopupMenu(meterMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(meterMenu);
    return;
  }

  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  bool hasItem = m_waveform.HasItem();
  bool hasReaperItem = hasItem && !m_waveform.IsStandaloneMode();
  bool hasSel = m_waveform.HasSelection();
  bool hasClip = s_clipboard.numFrames > 0;

  // If right-clicking near a marker, show marker actions at top level for quick access
  m_markers.SetRightClickMarkerIdx(m_markers.HitTestMarker(x, m_waveform, 8));
  if (m_markers.GetRightClickMarkerIdx() >= 0) {
    MenuAppend(menu, MF_STRING, CM_EDIT_MARKER, "Edit Marker...");
    MenuAppend(menu, MF_STRING, CM_DELETE_MARKER, "Delete Marker");
    MenuAppendSeparator(menu);
  }

  // Edit submenu
  HMENU editMenu = CreatePopupMenu();
  // In REAPER mode, always enable undo (REAPER manages its own undo stack)
  // In standalone mode, use our own stack state
  bool canUndo = m_waveform.IsStandaloneMode() ? m_hasUndo : hasReaperItem;
  MenuAppend(editMenu, canUndo ? MF_STRING : MF_GRAYED, CM_UNDO, "Undo\tCtrl+Z");
  MenuAppendSeparator(editMenu);
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_CUT, "Cut\tCtrl+X");
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_COPY, "Copy\tCtrl+C");
  MenuAppend(editMenu, (hasItem && hasClip) ? MF_STRING : MF_GRAYED, CM_PASTE, "Paste (destructive)\tCtrl+V");
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_DELETE, "Delete\tDel");
  bool canSilence = hasItem && (hasSel || m_waveform.IsStandaloneMode());
  MenuAppend(editMenu, canSilence ? MF_STRING : MF_GRAYED, CM_SILENCE,
             (m_waveform.IsStandaloneMode() && !hasSel) ? "Insert Silence...\tCtrl+Del" : "Silence\tCtrl+Del");
  MenuAppendSeparator(editMenu);
  MenuAppend(editMenu, hasItem ? MF_STRING : MF_GRAYED, CM_SELECT_ALL, "Select All\tCtrl+A");

  // Process submenu — organized by category
  HMENU procMenu = CreatePopupMenu();

  // Normalization
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_NORMALIZE, "Normalize (Peak)");
  MenuAppend(procMenu, hasReaperItem ? MF_STRING : MF_GRAYED, CM_NORMALIZE_LUFS, "Normalize to -14 LUFS");
  MenuAppend(procMenu, hasReaperItem ? MF_STRING : MF_GRAYED, CM_NORMALIZE_LUFS_16, "Normalize to -16 LUFS");
  MenuAppendSeparator(procMenu);

  // Gain
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_GAIN_UP, "Gain +3 dB");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_GAIN_DOWN, "Gain -3 dB");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_GAIN_PANEL, "Gain Control...\tG");
  MenuAppendSeparator(procMenu);

  // Fades
  MenuAppend(procMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_FADE_IN, "Fade In");
  MenuAppend(procMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_FADE_OUT, "Fade Out");
  MenuAppendSeparator(procMenu);

  // Destructive
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_REVERSE, "Reverse");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_DC_REMOVE, "DC Offset Remove");
  MenuAppendSeparator(procMenu);

  // Channel
  {
    // Mono downmix toggle
    bool isMono = false;
    if (hasItem && g_GetSetMediaItemTakeInfo && m_waveform.GetTake()) {
      int* pCM = (int*)g_GetSetMediaItemTakeInfo(m_waveform.GetTake(), "I_CHANMODE", nullptr);
      int chanMode = pCM ? *pCM : 0;
      isMono = (chanMode == 2 || chanMode == 3);
    }
    UINT flags = hasItem ? MF_STRING : MF_GRAYED;
    if (isMono) flags |= MF_CHECKED;
    MenuAppend(procMenu, flags, CM_MONO_DOWNMIX, "Downmix to Mono");
  }

  // Markers submenu
  HMENU markerMenu = CreatePopupMenu();
  MenuAppend(markerMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ADD_MARKER, "Add Marker at Cursor\tM");
  MenuAppend(markerMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_ADD_REGION, "Add Region from Selection");
  m_markers.SetRightClickMarkerIdx(m_markers.HitTestMarker(x, m_waveform));
  MenuAppend(markerMenu, (m_markers.GetRightClickMarkerIdx() >= 0) ? MF_STRING : MF_GRAYED, CM_EDIT_MARKER, "Edit Marker...");
  MenuAppend(markerMenu, (m_markers.GetRightClickMarkerIdx() >= 0) ? MF_STRING : MF_GRAYED, CM_DELETE_MARKER, "Delete Marker");
  MenuAppendSeparator(markerMenu);
  MenuAppend(markerMenu, m_markers.GetShowMarkers() ? (MF_STRING | MF_CHECKED) : MF_STRING, CM_SHOW_MARKERS, "Show Markers");

  // View submenu
  HMENU viewMenu = CreatePopupMenu();
  MenuAppend(viewMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ZOOM_IN, "Zoom In");
  MenuAppend(viewMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ZOOM_OUT, "Zoom Out");
  MenuAppend(viewMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ZOOM_FIT, "Zoom to Fit");
  MenuAppend(viewMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_ZOOM_SEL, "Zoom to Selection");
  MenuAppendSeparator(viewMenu);
  bool canSpectral = hasItem && !m_waveform.IsMultiItemActive();
  MenuAppend(viewMenu, canSpectral ? MF_STRING : MF_GRAYED, CM_TOGGLE_SPECTRAL,
             m_spectralVisible ? "Spectral View  \xE2\x9C\x93" : "Spectral View");
  MenuAppend(viewMenu, MF_STRING, CM_SNAP_ZERO,
             m_waveform.GetSnapToZero() ? "Snap to Zero-Crossing  \xE2\x9C\x93" : "Snap to Zero-Crossing");
  MenuAppend(viewMenu, MF_STRING, CM_MINIMAP,
             m_minimapVisible ? "Minimap  \xE2\x9C\x93" : "Minimap");

  // Multi-item view mode submenu (only when multi-item active)
  HMENU multiMenu = nullptr;
  if (m_waveform.IsMultiItemActive()) {
    multiMenu = CreatePopupMenu();
    MultiItemMode curMode = m_waveform.GetMultiItemMode();
    MenuAppend(multiMenu, (curMode == MultiItemMode::MIX) ? (MF_STRING | MF_CHECKED) : MF_STRING,
               CM_MULTI_MODE_MIX, "Mix (Sum)");
    MenuAppend(multiMenu, (curMode == MultiItemMode::LAYERED) ? (MF_STRING | MF_CHECKED) : MF_STRING,
               CM_MULTI_MODE_LAYERED, "Layered (per Item)");
    MenuAppend(multiMenu, (curMode == MultiItemMode::LAYERED_TRACKS) ? (MF_STRING | MF_CHECKED) : MF_STRING,
               CM_MULTI_MODE_LAYERED_TRACKS, "Layered (per Track)");
    MenuAppendSeparator(multiMenu);
    MenuAppend(multiMenu, MF_STRING, CM_SHOW_JOIN_LINES,
               m_waveform.GetShowJoinLines() ? "Show Join Lines  \xE2\x9C\x93" : "Show Join Lines");
    MenuAppendSubmenu(viewMenu, multiMenu, "Multi-Item View");
  }

  HMENU supportMenu = CreatePopupMenu();
  MenuAppend(supportMenu, MF_STRING, CM_SUPPORT_KOFI, "Ko-fi");
  MenuAppend(supportMenu, MF_STRING, CM_SUPPORT_BMAC, "Buy Me a Coffee");
  MenuAppend(supportMenu, MF_STRING, CM_SUPPORT_PAYPAL, "PayPal");

  MenuAppendSubmenu(menu, editMenu, "Edit");
  MenuAppendSubmenu(menu, procMenu, "Process");
  MenuAppendSubmenu(menu, markerMenu, "Markers");
  MenuAppendSubmenu(menu, viewMenu, "View");
  MenuAppendSeparator(menu);
  MenuAppendSubmenu(menu, supportMenu, "Support");

  POINT pt = { x, y };
  ClientToScreen(m_hwnd, &pt);
  TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);

#ifndef _WIN32
  DestroyMenu(editMenu);
  DestroyMenu(procMenu);
  DestroyMenu(markerMenu);
  if (multiMenu) DestroyMenu(multiMenu);
  DestroyMenu(viewMenu);
  DestroyMenu(supportMenu);
#endif
  DestroyMenu(menu);
}

void SneakPeak::OnContextMenuCommand(int id)
{
  switch (id) {
    case CM_UNDO:      UndoRestore(); break;
    case CM_CUT:       DoCut(); break;
    case CM_COPY:      DoCopy(); break;
    case CM_PASTE:     DoPaste(); break;
    case CM_DELETE:    DoDelete(); break;
    case CM_SILENCE:   DoSilence(); break;
    case CM_SELECT_ALL:
      if (m_waveform.HasItem()) {
        m_waveform.StartSelection(0.0);
        m_waveform.UpdateSelection(m_waveform.GetItemDuration());
        m_waveform.EndSelection();
        SyncSelectionToReaper();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    case CM_NORMALIZE:      DoNormalize(); break;
    case CM_NORMALIZE_LUFS: DoNormalizeLUFS(-14.0); break;
    case CM_NORMALIZE_LUFS_16: DoNormalizeLUFS(-16.0); break;
    case CM_FADE_IN:   DoFadeIn(); break;
    case CM_FADE_OUT:  DoFadeOut(); break;
    case CM_REVERSE:   DoReverse(); break;
    case CM_GAIN_UP:   DoGain(1.4125); break;  // +3dB
    case CM_GAIN_DOWN: DoGain(0.7079); break;  // -3dB
    case CM_DC_REMOVE: DoDCRemove(); break;
    case CM_ZOOM_IN: {
      double center = m_waveform.GetViewStart() + m_waveform.GetViewDuration() / 2.0;
      m_waveform.ZoomHorizontal(ZOOM_FACTOR * 2.0, center);
      break;
    }
    case CM_ZOOM_OUT: {
      double center = m_waveform.GetViewStart() + m_waveform.GetViewDuration() / 2.0;
      m_waveform.ZoomHorizontal(1.0 / (ZOOM_FACTOR * 2.0), center);
      break;
    }
    case CM_ZOOM_FIT:  m_waveform.ZoomToFit(); break;
    case CM_ZOOM_SEL:  m_waveform.ZoomToSelection(); break;
    case CM_SHOW_MARKERS: m_markers.ToggleShowMarkers(); break;
    case CM_ADD_MARKER:  m_markers.AddMarkerAtCursor(m_waveform); break;
    case CM_ADD_REGION:  m_markers.AddRegionFromSelection(m_waveform); break;
    case CM_DELETE_MARKER:
      if (m_markers.GetRightClickMarkerIdx() >= 0) m_markers.DeleteMarkerByEnumIdx(m_markers.GetRightClickMarkerIdx());
      break;
    case CM_EDIT_MARKER:
      if (m_markers.GetRightClickMarkerIdx() >= 0) m_markers.EditMarkerDialog(m_markers.GetRightClickMarkerIdx());
      break;
    case CM_GAIN_PANEL:
      m_gainPanel.Toggle(m_waveform.GetItem());
      break;
    case CM_MONO_DOWNMIX:
      DBG("[SneakPeak] CM_MONO_DOWNMIX: hasItem=%d hasTake=%d hasAPI=%d\n",
          m_waveform.HasItem(), m_waveform.GetTake() != nullptr, g_GetSetMediaItemTakeInfo != nullptr);
      if (m_waveform.HasItem() && m_waveform.GetTake() && g_GetSetMediaItemTakeInfo) {
        MediaItem* item = m_waveform.GetItem();
        MediaItem_Take* take = m_waveform.GetTake();
        int* pCM = (int*)g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", nullptr);
        int chanMode = pCM ? *pCM : -999;
        int newMode = (chanMode == 2) ? 0 : 2;
        DBG("[SneakPeak] DOWNMIX: pCM=%p chanMode=%d -> newMode=%d\n", (void*)pCM, chanMode, newMode);
        if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
        g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", &newMode);
        if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Toggle Mono Downmix", -1);
        m_lastChanMode = newMode;
        if (g_UpdateArrange) g_UpdateArrange();
        if (g_UpdateTimeline) g_UpdateTimeline();
        // Force reload
        m_waveform.ClearItem();
        m_waveform.SetItem(item);
        if (m_gainPanel.IsVisible()) m_gainPanel.Show(item);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        DBG("[SneakPeak] DOWNMIX: reload done, numCh=%d\n", m_waveform.GetNumChannels());
      }
      break;
    case CM_SNAP_ZERO:
      m_waveform.SetSnapToZero(!m_waveform.GetSnapToZero());
      if (g_SetExtState)
        g_SetExtState("SneakPeak", "snap_zero", m_waveform.GetSnapToZero() ? "1" : "0", true);
      break;
    case CM_MINIMAP:
      m_minimapVisible = !m_minimapVisible;
      DBG("[SneakPeak] Minimap toggled: visible=%d\n", m_minimapVisible);
      if (g_SetExtState)
        g_SetExtState("SneakPeak", "minimap", m_minimapVisible ? "1" : "0", true);
      m_minimap.Invalidate();
      {
        RECT cr;
        GetClientRect(m_hwnd, &cr);
        RecalcLayout(cr.right, cr.bottom);
        m_waveform.Invalidate();
      }
      break;
    case CM_MULTI_MODE_MIX:
      m_waveform.SetMultiItemMode(MultiItemMode::MIX);
      m_waveform.Invalidate();
      if (g_SetExtState) g_SetExtState("SneakPeak", "multi_mode", "mix", true);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    case CM_MULTI_MODE_LAYERED:
      m_waveform.SetMultiItemMode(MultiItemMode::LAYERED);
      m_waveform.Invalidate();
      if (g_SetExtState) g_SetExtState("SneakPeak", "multi_mode", "layered", true);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    case CM_MULTI_MODE_LAYERED_TRACKS:
      m_waveform.SetMultiItemMode(MultiItemMode::LAYERED_TRACKS);
      m_waveform.Invalidate();
      if (g_SetExtState) g_SetExtState("SneakPeak", "multi_mode", "layered_tracks", true);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    case CM_SHOW_JOIN_LINES:
      m_waveform.SetShowJoinLines(!m_waveform.GetShowJoinLines());
      if (g_SetExtState) g_SetExtState("SneakPeak", "show_join_lines",
                                        m_waveform.GetShowJoinLines() ? "1" : "0", true);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    case CM_METER_PEAK:
      m_levels.SetMode(MeterMode::PEAK);
      if (g_SetExtState) g_SetExtState("SneakPeak", "meter_mode", "peak", true);
      InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
      break;
    case CM_METER_RMS:
      m_levels.SetMode(MeterMode::RMS);
      if (g_SetExtState) g_SetExtState("SneakPeak", "meter_mode", "rms", true);
      InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
      break;
    case CM_METER_VU:
      m_levels.SetMode(MeterMode::VU);
      if (g_SetExtState) g_SetExtState("SneakPeak", "meter_mode", "vu", true);
      InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
      break;
    case CM_METER_SOURCE_MASTER:
      m_meterFromMaster = !m_meterFromMaster;
      if (g_SetExtState) g_SetExtState("SneakPeak", "meter_source", m_meterFromMaster ? "master" : "item", true);
      InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
      break;
    case CM_TOGGLE_SPECTRAL:
      m_spectralVisible = !m_spectralVisible;
      m_spectralPainted = false;
      {
        RECT cr;
        GetClientRect(m_hwnd, &cr);
        RecalcLayout(cr.right, cr.bottom);
        m_waveform.Invalidate();
        m_spectral.Invalidate();
      }
      break;
    case CM_SUPPORT_KOFI:
#ifdef _WIN32
      ShellExecute(nullptr, "open", "https://ko-fi.com/quickmd", nullptr, nullptr, SW_SHOWNORMAL);
#else
      system("/usr/bin/open 'https://ko-fi.com/quickmd'");
#endif
      break;
    case CM_SUPPORT_BMAC:
#ifdef _WIN32
      ShellExecute(nullptr, "open", "https://buymeacoffee.com/bsroczynskh", nullptr, nullptr, SW_SHOWNORMAL);
#else
      system("/usr/bin/open 'https://buymeacoffee.com/bsroczynskh'");
#endif
      break;
    case CM_SUPPORT_PAYPAL:
#ifdef _WIN32
      ShellExecute(nullptr, "open", "https://www.paypal.com/paypalme/b451c", nullptr, nullptr, SW_SHOWNORMAL);
#else
      system("/usr/bin/open 'https://www.paypal.com/paypalme/b451c'");
#endif
      break;
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

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
    pthread_mutex_destroy(&reg->mutex);
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
  pthread_mutex_init(&reg->mutex, nullptr);
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
    pthread_mutex_destroy(&reg->mutex);
    delete reg;
    delete src;
    DBG("[SneakPeak] Standalone preview FAILED to start\n");
  }
}

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

// --- Drag & Drop Export ---

void SneakPeak::CleanupDragTemp()
{
  if (!m_dragTempPath.empty() && !m_dragIsOriginal) {
    remove(m_dragTempPath.c_str());
  }
  m_dragTempPath.clear();
  m_dragIsOriginal = false;
}

void SneakPeak::InitiateDragExport()
{
  if (!m_waveform.HasItem()) return;
  CleanupDragTemp();

  bool isStandalone = m_waveform.IsStandaloneMode();
  bool hasSelection = m_waveform.HasSelection();

  // Standalone full-file drag (no selection)
  if (isStandalone && !hasSelection) {
    if (!m_dirty && !m_waveform.HasStandaloneFade()) {
      // Clean file — drag saved path or original
      std::string dragPath = m_savedPath.empty()
          ? m_waveform.GetStandaloneFilePath() : m_savedPath;
      if (!dragPath.empty()) {
        m_dragTempPath = dragPath;
        m_dragIsOriginal = true;
        DBG("[SneakPeak] DragExport: clean file: %s\n", dragPath.c_str());
      }
    } else {
      // Dirty — auto-save first, then drag the saved file
      SaveStandaloneFile();
      if (!m_dirty && !m_savedPath.empty()) {
        m_dragTempPath = m_savedPath;
        m_dragIsOriginal = true;
        DBG("[SneakPeak] DragExport: auto-saved: %s\n", m_savedPath.c_str());
      }
    }
  }

  // Selection export (standalone or REAPER) — create temp WAV
  if (m_dragTempPath.empty()) {
    int startF, endF;
    if (hasSelection) {
      GetSelectionSampleRange(startF, endF);
    } else {
      startF = 0;
      endF = m_waveform.GetAudioSampleCount();
    }
    int nch = m_waveform.GetNumChannels();
    int sr = m_waveform.GetSampleRate();
    int selFrames = endF - startF;
    if (selFrames <= 0 || nch <= 0) return;

    const auto& data = m_waveform.GetAudioData();
    size_t offset = (size_t)startF * (size_t)nch;
    size_t needed = offset + (size_t)selFrames * (size_t)nch;
    if (needed > data.size()) return;

    std::vector<double> exportBuf(data.begin() + offset,
                                   data.begin() + offset + (size_t)selFrames * nch);

    // Bake pending standalone fades into export copy
    if (isStandalone) {
      auto sf = m_waveform.GetStandaloneFade();
      int totalFrames = m_waveform.GetAudioSampleCount();
      if (sf.fadeInLen >= 0.001) {
        int fadeFrames = std::min((int)(sf.fadeInLen * sr), totalFrames);
        if (startF < fadeFrames) {
          int overlap = std::min(fadeFrames - startF, selFrames);
          for (int i = 0; i < overlap; i++) {
            double t = (double)(startF + i) / (double)fadeFrames;
            double gain = ApplyFadeShape(t, sf.fadeInShape, -sf.fadeInDir);
            for (int ch = 0; ch < nch; ch++)
              exportBuf[i * nch + ch] *= gain;
          }
        }
      }
      if (sf.fadeOutLen >= 0.001) {
        int fadeFrames = std::min((int)(sf.fadeOutLen * sr), totalFrames);
        int fadeStart = totalFrames - fadeFrames;
        int overlapStart = std::max(startF, fadeStart);
        int overlapEnd = std::min(endF, totalFrames);
        for (int i = overlapStart; i < overlapEnd; i++) {
          double t = (double)(i - fadeStart) / (double)fadeFrames;
          double gain = ApplyFadeShape(1.0 - t, sf.fadeOutShape, sf.fadeOutDir);
          int bufIdx = i - startF;
          for (int ch = 0; ch < nch; ch++)
            exportBuf[bufIdx * nch + ch] *= gain;
        }
      }
    }

    const char* srcPath = isStandalone
        ? m_waveform.GetStandaloneFilePath().c_str() : nullptr;
    m_dragTempPath = AudioEngine::WriteExportWav(exportBuf.data(), selFrames, nch,
                                                  sr, m_wavBitsPerSample, m_wavAudioFormat,
                                                  srcPath);
    if (m_dragTempPath.empty()) return;
    DBG("[SneakPeak] DragExport: temp WAV: %s\n", m_dragTempPath.c_str());
  }

  // Initiate drag
#ifndef _WIN32
  RECT dragRect = { m_dragStartX - 5, m_dragStartY - 5,
                    m_dragStartX + 5, m_dragStartY + 5 };
  const char* files[] = { m_dragTempPath.c_str() };
  SWELL_InitiateDragDropOfFileList(m_hwnd, &dragRect, files, 1, nullptr);
#endif
}
