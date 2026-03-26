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
    const char* rulerAbs = g_GetExtState("SneakPeak", "ruler_absolute");
    if (rulerAbs && rulerAbs[0] == '1') m_rulerAbsolute = true;
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

void SneakPeak::ToggleTrackView()
{
  if (m_waveform.IsStandaloneMode()) return;

  // If working set exists (active or dormant), exit it
  if (m_workingSet.active || m_workingSet.dormant) {
    ExitWorkingSet();
    return;
  }

  // Create working set from currently selected items
  if (!g_CountSelectedMediaItems || !g_GetSelectedMediaItem ||
      !g_GetMediaItem_Track || !g_GetMediaItemInfo_Value) return;

  int count = g_CountSelectedMediaItems(nullptr);
  if (count <= 0) return;

  // Collect items and verify all on same track
  MediaTrack* track = nullptr;
  double minPos = 1e30, maxEnd = -1e30;
  for (int i = 0; i < count; i++) {
    MediaItem* mi = g_GetSelectedMediaItem(nullptr, i);
    if (!mi) continue;
    MediaTrack* t = g_GetMediaItem_Track(mi);
    if (!track) track = t;
    else if (t != track) return; // multi-track: not supported
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    double len = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
    if (pos < minPos) minPos = pos;
    if (pos + len > maxEnd) maxEnd = pos + len;
  }
  if (!track || maxEnd <= minPos) return;

  m_workingSet.track = track;
  m_workingSet.startPos = minPos;
  m_workingSet.endPos = maxEnd;
  LoadWorkingSet();
}

void SneakPeak::LoadWorkingSet()
{
  if (!m_workingSet.track) return;
  if (m_previewActive) StandaloneCleanupPreview();

  m_waveform.ClearItem();
  m_waveform.LoadItemsInRange(m_workingSet.track, m_workingSet.startPos, m_workingSet.endPos);

  if (!m_waveform.HasItem()) {
    ExitWorkingSet();
    return;
  }

  m_workingSet.active = true;
  m_workingSet.dormant = false;
  m_rulerAbsolute = true; // default to absolute time in working set

  m_spectralVisible = false;
  m_spectral.ClearSpectrum();
  m_spectral.Invalidate();
  m_minimap.Invalidate();
  m_hasUndo = false;
  m_dirty = false;

  // Batch gain for all items in the working set
  {
    std::vector<MediaItem*> setItems;
    for (const auto& seg : m_waveform.GetSegments())
      if (seg.item) setItems.push_back(seg.item);
    if (!setItems.empty())
      m_gainPanel.ShowBatch(setItems);
  }

  if (m_hwnd) {
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    RecalcLayout(cr.right, cr.bottom);
    m_waveform.Invalidate();

    int itemCount = (int)m_waveform.GetSegments().size();
    char title[512];
    snprintf(title, sizeof(title), "SneakPeak [Set - %d items]", itemCount);
    SetWindowText(m_hwnd, title);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::RefreshWorkingSet()
{
  if (!m_workingSet.track || !g_ValidatePtr2 ||
      !g_ValidatePtr2(nullptr, m_workingSet.track, "MediaTrack*")) {
    ExitWorkingSet();
    return;
  }

  double viewStart = m_waveform.GetViewStart();
  double viewDur = m_waveform.GetViewDuration();

  m_waveform.ClearItem();
  m_waveform.LoadItemsInRange(m_workingSet.track, m_workingSet.startPos, m_workingSet.endPos);
  m_waveform.Invalidate();

  if (!m_waveform.HasItem()) {
    ExitWorkingSet();
    return;
  }

  // Restore approximate view position
  if (m_waveform.GetItemDuration() > 0) {
    m_waveform.SetViewStart(std::min(viewStart, m_waveform.GetItemDuration()));
    m_waveform.SetViewDuration(viewDur);
  }

  if (m_hwnd) {
    int itemCount = (int)m_waveform.GetSegments().size();
    char title[512];
    snprintf(title, sizeof(title), "SneakPeak [Set - %d items]", itemCount);
    SetWindowText(m_hwnd, title);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void SneakPeak::ExitWorkingSet()
{
  m_workingSet = {};
  LoadSelectedItem();
}

bool SneakPeak::IsWorkingSetItem(MediaItem* item) const
{
  if (!m_workingSet.track || (!m_workingSet.active && !m_workingSet.dormant)) return false;
  if (!item || !g_GetMediaItem_Track || !g_GetMediaItemInfo_Value) return false;
  if (g_GetMediaItem_Track(item) != m_workingSet.track) return false;
  double pos = g_GetMediaItemInfo_Value(item, "D_POSITION");
  double len = g_GetMediaItemInfo_Value(item, "D_LENGTH");
  return (pos + len > m_workingSet.startPos) && (pos < m_workingSet.endPos);
}

void SneakPeak::LoadSelectedItem()
{
  if (!g_CountSelectedMediaItems || !g_GetSelectedMediaItem) return;

  if (m_previewActive) StandaloneCleanupPreview();

  int count = g_CountSelectedMediaItems(nullptr);
  if (count <= 0) {
    // No selection - don't destroy dormant working set
    if (!m_workingSet.dormant && !m_workingSet.active) {
      m_waveform.ClearItem();
      m_hasUndo = false;
      if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  MediaItem* item = g_GetSelectedMediaItem(nullptr, 0);
  if (!item) return;

  // Working set: dormant restore - clicking back on a set item restores the view
  if (m_workingSet.dormant) {
    for (int i = 0; i < count; i++) {
      MediaItem* mi = g_GetSelectedMediaItem(nullptr, i);
      if (mi && IsWorkingSetItem(mi)) {
        LoadWorkingSet();
        return;
      }
    }
    // Selection is outside working set - stay dormant, load normally
  }

  // Working set active: if selected item is in range, ignore selection change
  if (m_workingSet.active) {
    if (IsWorkingSetItem(item)) return; // stay locked, don't reload
    // Item outside range - go dormant
    m_workingSet.active = false;
    m_workingSet.dormant = true;
  }

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
    // But NOT when there's a selection (selection uses per-region preview instead)
    m_gainPanel.SetSkipBatchWrite(m_waveform.HasSelection());
    if (m_gainPanel.IsBatch() && !m_waveform.HasSelection()) {
      double offsetLin = pow(10.0, m_gainPanel.GetDb() / 20.0);
      m_waveform.SetBatchGainOffset(offsetLin);
    } else {
      m_waveform.SetBatchGainOffset(1.0);
    }
  }
  // Gain preview: selection-only overlay during knob drag (all modes)
  if (m_gainPanel.IsVisible() && m_gainPanel.IsDragging() && m_waveform.HasSelection()) {
    double gainLin = pow(10.0, m_gainPanel.GetDb() / 20.0);
    WaveformSelection sel = m_waveform.GetSelection();
    double s = std::min(sel.startTime, sel.endTime);
    double e = std::max(sel.startTime, sel.endTime);
    m_waveform.SetStandaloneGain(gainLin, s, e);
  } else {
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

  // Working set: no periodic refresh - refreshes on explicit user actions

  // Pending view restore (after gain split - re-apply until countdown expires)
  if (m_pendingViewRestoreTicks > 0 && m_waveform.HasItem() && m_waveform.GetItemDuration() > 0) {
    m_waveform.SetViewStart(std::min(m_pendingViewStart, m_waveform.GetItemDuration()));
    m_waveform.SetViewDuration(m_pendingViewDur);
    m_waveform.Invalidate();
    m_pendingViewRestoreTicks--;
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  // Cursor + RMS levels - works for both single and multi-item
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
