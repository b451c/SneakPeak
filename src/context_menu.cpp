// ============================================================================
// context_menu.cpp — Right-click context menu and command dispatch for SneakPeak
//
// Menu construction, submenu building, and WM_COMMAND handler dispatch.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_ops.h"
#include "theme.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cmath>
#include <cstring>

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
  if (!m_waveform.IsStandaloneMode() && hasReaperItem) {
    MenuAppendSeparator(editMenu);
    MenuAppend(editMenu, MF_STRING, CM_SPLIT, "Split at Cursor\tS");
  }

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
  if (hasItem) {
    MenuAppend(viewMenu, MF_STRING, CM_RULER_ABSOLUTE,
               m_rulerAbsolute ? "Ruler: Absolute Time  \xE2\x9C\x93" : "Ruler: Absolute Time");
  }
  if (!m_waveform.IsStandaloneMode() && hasItem) {
    MenuAppend(viewMenu, MF_STRING, CM_TRACK_VIEW,
               (m_workingSet.active || m_workingSet.dormant)
                   ? "Working Set (T)  \xE2\x9C\x93" : "Working Set (T)");
  }
  if (m_workingSet.active) {
    // Check if items in range are already grouped
    double rs = m_workingSet.startPos, re = m_workingSet.endPos;
    if (m_waveform.HasSelection()) {
      WaveformSelection sel = m_waveform.GetSelection();
      rs = m_waveform.RelTimeToAbsTime(std::min(sel.startTime, sel.endTime));
      re = m_waveform.RelTimeToAbsTime(std::max(sel.startTime, sel.endTime));
    }
    int gid = GetSetGroupId(rs, re);
    const char* groupLabel = gid ? "Group  \xE2\x9C\x93" : "Group";
    MenuAppend(viewMenu, MF_STRING, CM_GROUP_SET, groupLabel);
  }

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
  MenuAppendSeparator(menu);
  MenuAppend(menu, MF_STRING, CM_DOCK_WINDOW,
             m_isDocked ? "Undock SneakPeak" : "Dock SneakPeak in Docker");

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
    case CM_SPLIT:
      if (!m_waveform.IsStandaloneMode() && m_waveform.HasItem()) {
        if (m_workingSet.active && !m_waveform.HasSelection() && g_SetEditCurPos) {
          double absTime = m_waveform.RelTimeToAbsTime(m_waveform.GetCursorTime());
          g_SetEditCurPos(absTime, true, false);
        }
        SyncSelectionToReaper();
        if (m_waveform.HasSelection() && g_Main_OnCommand)
          g_Main_OnCommand(40061, 0);
        else if (g_Main_OnCommand)
          g_Main_OnCommand(40012, 0);
        if (m_workingSet.active) RefreshWorkingSet();
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
        if (g_PreventUIRefresh) g_PreventUIRefresh(1);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
        g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", &newMode);
        if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Toggle Mono Downmix", -1);
      if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
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
    case CM_RULER_ABSOLUTE:
      m_rulerAbsolute = !m_rulerAbsolute;
      if (g_SetExtState)
        g_SetExtState("SneakPeak", "ruler_absolute", m_rulerAbsolute ? "1" : "0", true);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    case CM_GROUP_SET: {
      double rs = m_workingSet.startPos, re = m_workingSet.endPos;
      if (m_waveform.HasSelection()) {
        WaveformSelection sel = m_waveform.GetSelection();
        rs = m_waveform.RelTimeToAbsTime(std::min(sel.startTime, sel.endTime));
        re = m_waveform.RelTimeToAbsTime(std::max(sel.startTime, sel.endTime));
      }
      int gid = GetSetGroupId(rs, re);
      if (gid) UngroupSetItems();
      else GroupSetItems();
      break;
    }
    case CM_TRACK_VIEW:
      ToggleTrackView();
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
    case CM_DOCK_WINDOW:
      if (m_isDocked) {
        // Undock: destroy docked window, recreate as floating
        KillTimer(m_hwnd, TIMER_REFRESH);
        if (g_DockWindowRemove) g_DockWindowRemove(m_hwnd);
        DestroyWindow(m_hwnd);
        m_hwnd = CreateSneakPeakDialog(g_reaperMainHwnd, DlgProc, (LPARAM)this);
        if (g_GetExtState) {
          const char* wr = g_GetExtState("SneakPeak", "win_rect");
          if (wr && wr[0]) {
            int x, y, w, h;
            if (sscanf(wr, "%d %d %d %d", &x, &y, &w, &h) == 4 && w > 100 && h > 80)
              SetWindowPos(m_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
          }
        }
        ShowWindow(m_hwnd, SW_SHOW);
        SetTimer(m_hwnd, TIMER_REFRESH, TIMER_INTERVAL_MS, nullptr);
        { RECT cr; GetClientRect(m_hwnd, &cr); RecalcLayout(cr.right, cr.bottom); }
        m_isDocked = false;
      } else {
        if (g_DockWindowAddEx)
          g_DockWindowAddEx(m_hwnd, "SneakPeak", "SneakPeak_main", true);
        m_isDocked = true;
      }
      if (g_SetExtState)
        g_SetExtState("SneakPeak", "was_docked", m_isDocked ? "1" : "0", true);
      break;
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

