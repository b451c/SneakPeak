// ============================================================================
// input_handling.cpp — Mouse, keyboard, and toolbar input for SneakPeak
//
// OnMouseDown/Up/Move/Wheel, OnDoubleClick, OnKeyDown, OnToolbarClick.
// Handles waveform selection, fade handle drag, splitter drag, scrollbar,
// minimap interaction, spectral frequency selection, and keyboard shortcuts.
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "audio_ops.h"
#include "item_split_ops.h"
#include "theme.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>


// --- Mouse ---

void SneakPeak::OnDoubleClick(int x, int y)
{
  // Double-click on gain panel = reset to 0 dB
  if (m_gainPanel.OnDoubleClick(x, y, m_waveformRect)) {
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Double-click on waveform area
  if (y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    // Double-click on envelope point = delete it
    if (m_waveform.GetShowVolumeEnvelope() && !m_waveform.IsStandaloneMode() &&
        g_GetTakeEnvelopeByName && g_DeleteEnvelopePointEx && g_Envelope_SortPoints) {
      int hitIdx = m_waveform.HitTestEnvelopePoint(x, y, 8);
      if (hitIdx >= 0) {
        double dblClickTime = m_waveform.XToTime(x);
        auto dblEi = m_waveform.GetEnvelopeAtTime(dblClickTime);
        TrackEnvelope* env = dblEi.env;
        if (env) {
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          g_DeleteEnvelopePointEx(env, -1, hitIdx);
          g_Envelope_SortPoints(env);
          if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Delete envelope point", -1);
          m_envDragPointIdx = -1;
          if (g_UpdateArrange) g_UpdateArrange();
          InvalidateRect(m_hwnd, nullptr, FALSE);
          return;
        }
      }
    }
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
    // Click on mode label in MULTI mode: show view mode popup menu
    if (m_waveform.IsMultiItemActive() &&
        x >= m_modeLabelRect.left && x < m_modeLabelRect.right &&
        y >= m_modeLabelRect.top && y < m_modeLabelRect.bottom) {
      HMENU modeMenu = CreatePopupMenu();
      MultiItemMode curMode = m_waveform.GetMultiItemMode();
      auto addItem = [](HMENU m, unsigned int flags, unsigned int id, const char* str) {
#ifdef _WIN32
        AppendMenuA(m, flags, id, str);
#else
        MENUITEMINFO mi = { sizeof(mi) };
        mi.fMask = MIIM_ID | MIIM_STATE | MIIM_TYPE;
        mi.fType = MFT_STRING;
        mi.fState = (flags & MF_CHECKED) ? MFS_CHECKED : 0;
        mi.wID = id;
        mi.dwTypeData = (char*)str;
        InsertMenuItem(m, GetMenuItemCount(m), TRUE, &mi);
#endif
      };
      addItem(modeMenu, curMode == MultiItemMode::MIX ? MF_CHECKED : 0,
              CM_MULTI_MODE_MIX, "Mix (Sum)");
      addItem(modeMenu, curMode == MultiItemMode::LAYERED ? MF_CHECKED : 0,
              CM_MULTI_MODE_LAYERED, "Layered (per Item)");
      addItem(modeMenu, curMode == MultiItemMode::LAYERED_TRACKS ? MF_CHECKED : 0,
              CM_MULTI_MODE_LAYERED_TRACKS, "Layered (per Track)");
#ifdef _WIN32
      AppendMenuA(modeMenu, MF_SEPARATOR, 0, nullptr);
#else
      SWELL_Menu_AddMenuItem(modeMenu, "", 0, MF_SEPARATOR);
#endif
      addItem(modeMenu, 0, CM_SWITCH_TIMELINE, "Timeline View");
      POINT pt = { x, m_modeBarRect.bottom };
      ClientToScreen(m_hwnd, &pt);
      TrackPopupMenu(modeMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
      DestroyMenu(modeMenu);
      return;
    }
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
        g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), true, false);
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
        // Immediately set skipBatchWrite for selection/SET/timeline preview
        // (don't wait for timer — prevents first-frame D_VOL write to whole item)
        bool hasSelPreview = m_waveform.HasSelection();
        bool skip = m_workingSet.active || m_waveform.IsTimelineOrMultiItem() || hasSelPreview;
        m_gainPanel.SetSkipBatchWrite(skip);
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  // Dynamics panel interaction
  if (m_dynamicsPanel.IsVisible() && m_dynamicsPanel.HitTest(x, y, m_waveformRect)) {
    bool wasLiveUndo = m_dynamicsPanel.LiveUndoOpen();
    bool wasBypassed = m_dynamicsPanel.GetBypassed();
    if (m_dynamicsPanel.OnMouseDown(x, y, m_waveformRect)) {
      if (m_dynamicsPanel.IsDragging())
        SetCapture(m_hwnd);
      // Check if Apply was clicked
      if (m_dynamicsPanel.ApplyRequested()) {
        m_dynamicsPanel.ClearApplyRequested();
        ApplyDynamicsToEnvelope();
        // Save dynamics params to item P_EXT on Apply
        SaveDynamicsToItem();
      }
      // Preset dropdown menu
      if (m_dynamicsPanel.PresetMenuRequested()) {
        m_dynamicsPanel.ClearPresetMenuRequested();
        HMENU presetMenu = CreatePopupMenu();
        auto addPresetItem = [](HMENU m, unsigned int id, const char* str) {
#ifdef _WIN32
          AppendMenuA(m, MF_STRING, id, str);
#else
          MENUITEMINFO mi = { sizeof(mi) };
          mi.fMask = MIIM_ID | MIIM_STATE | MIIM_TYPE;
          mi.fType = MFT_STRING;
          mi.fState = 0;
          mi.wID = id;
          mi.dwTypeData = (char*)str;
          InsertMenuItem(m, GetMenuItemCount(m), TRUE, &mi);
#endif
        };
        for (int i = 0; i < PRESET_COUNT; i++)
          addPresetItem(presetMenu, CM_PRESET_BASE + i, g_dynamicsPresets[i].name);
        RECT pr = m_dynamicsPanel.GetRect(m_waveformRect);
        RECT pbr = m_dynamicsPanel.GetPresetButtonRect(pr);
        POINT pt = { pbr.left, pbr.bottom + 2 };
        ClientToScreen(m_hwnd, &pt);
        TrackPopupMenu(presetMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(presetMenu);
      }
      // Restore envelope on panel close if bypass was active
      bool isBypassed = m_dynamicsPanel.GetBypassed();
      if (wasBypassed && !m_dynamicsPanel.IsVisible())
        isBypassed = false; // panel closed, restore envelope
      // A/B bypass: toggle envelope ACTIVE state on all segments' envelopes
      if (isBypassed != wasBypassed) {
        m_waveform.SetEnvBypassed(isBypassed);
        if (g_GetTakeEnvelopeByName && g_GetSetEnvelopeInfo_String) {
          char val[4];
          snprintf(val, sizeof(val), "%d", isBypassed ? 0 : 1);
          auto& segs = m_waveform.GetSegments();
          bool isMultiSeg = (m_waveform.IsTimelineView() || m_waveform.IsTrackView()) && segs.size() > 1;
          if (isMultiSeg) {
            for (const auto& seg : segs) {
              if (!seg.take) continue;
              TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
              if (env) g_GetSetEnvelopeInfo_String(env, "ACTIVE", val, true);
            }
          } else if (m_waveform.GetTake()) {
            TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
            if (env) g_GetSetEnvelopeInfo_String(env, "ACTIVE", val, true);
          }
          if (g_UpdateArrange) g_UpdateArrange();
        }
      }
      // Re-analyze on toggle clicks (Peak/RMS) that set ParamsChanged
      if (m_dynamicsPanel.ParamsChanged()) {
        m_dynamicsPanel.ClearParamsChanged();
        m_dynamics.SetParams(m_dynamicsPanel.GetParams());
        if (m_waveform.GetAudioSampleCount() > 0) {
          double ivDb = 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
          m_dynamics.Analyze(m_waveform.GetAudioData().data(),
                             m_waveform.GetAudioSampleCount(),
                             m_waveform.GetNumChannels(),
                             m_waveform.GetSampleRate(),
                             ivDb, m_dynamicsPanel.GetParams());
          m_dynamics.ComputeCompression();
          m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());
          if (m_dynamicsPanel.IsLive())
            ApplyDynamicsToEnvelope();
        }
      }
      // Close live undo block if panel was hidden (close button) or Live toggled off
      if (wasLiveUndo && !m_dynamicsPanel.LiveUndoOpen() &&
          (!m_dynamicsPanel.IsVisible() || !m_dynamicsPanel.IsLive())) {
        if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Live Dynamics", -1);
        m_dynamicsPanel.SetLiveUndoOpen(false);
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
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
          g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), true, false);
      }
      m_dragging = true;
      SetCapture(m_hwnd);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return;
  }

  if (y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    OnMouseDownWaveform(x, y, wParam);
  }
}

void SneakPeak::OnMouseDownWaveform(int x, int y, WPARAM wParam)
{
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
        if (abs(x - fiX) <= FADE_HANDLE_HIT_ZONE && y < m_waveformRect.top + FADE_HANDLE_TOP_ZONE) {
          m_fadeDragging = FADE_IN;
          m_standaloneFadeDrag = true;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = sf.fadeInDir;
          SetCapture(m_hwnd);
          return;
        }
        if (abs(x - foX) <= FADE_HANDLE_HIT_ZONE && y < m_waveformRect.top + FADE_HANDLE_TOP_ZONE) {
          m_fadeDragging = FADE_OUT;
          m_standaloneFadeDrag = true;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = sf.fadeOutDir;
          SetCapture(m_hwnd);
          return;
        }
      }

      // Clear envelope reveal range if clicking outside it (without Cmd)
      if (m_waveform.HasEnvRevealRange() && m_waveform.GetShowVolumeEnvelope()) {
        bool cmdDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!cmdDown) {
          double clickTime = m_waveform.XToTime(x);
          if (clickTime < m_waveform.GetEnvRevealStart() || clickTime > m_waveform.GetEnvRevealEnd()) {
            m_waveform.ClearEnvRevealRange();
            InvalidateRect(m_hwnd, nullptr, FALSE);
          }
        }
      }

      // Envelope point interaction (before fade handles and selection)
      // Uses GetEnvelopeAtTime to find the correct per-segment envelope in timeline/SET
      if (m_waveform.GetShowVolumeEnvelope() && !m_waveform.IsStandaloneMode() &&
          g_GetTakeEnvelopeByName && g_ScaleToEnvelopeMode && g_InsertEnvelopePointEx &&
          g_Envelope_SortPoints && g_Envelope_Evaluate && g_GetEnvelopeScalingMode &&
          g_ScaleFromEnvelopeMode) {
        double clickTime = m_waveform.XToTime(x);
        auto ei = m_waveform.GetEnvelopeAtTime(clickTime);
        TrackEnvelope* env = ei.env;
        if (env) {
          int envCount = g_CountEnvelopePoints ? g_CountEnvelopePoints(env) : 0;
          bool isDenseEnv = (envCount > 100);
          double segOffset = 0.0;
          double segDuration = m_waveform.GetItemDuration();
          if (ei.segmentIdx >= 0) {
            const auto& seg = m_waveform.GetSegments()[ei.segmentIdx];
            segOffset = seg.relativeOffset;
            segDuration = seg.duration;
          }

          int hitIdx = m_waveform.HitTestEnvelopePoint(x, y, 8);
          if (hitIdx >= 0) {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            // Select/toggle point selection
            if (g_GetEnvelopePoint && g_SetEnvelopePoint) {
              double pt = 0, pv = 0, ptn = 0; int ps = 0; bool psel = false;
              g_GetEnvelopePoint(env, hitIdx, &pt, &pv, &ps, &ptn, &psel);
              if (shift) {
                // Toggle this point's selection
                bool newSel = !psel;
                bool noSort = true;
                g_SetEnvelopePoint(env, hitIdx, nullptr, nullptr, nullptr, nullptr, &newSel, &noSort);
              } else if (!psel) {
                // Deselect all, select this one
                int cnt = g_CountEnvelopePoints(env);
                bool noSort = true;
                for (int j = 0; j < cnt; j++) {
                  bool sel = (j == hitIdx);
                  g_SetEnvelopePoint(env, j, nullptr, nullptr, nullptr, nullptr, &sel, &noSort);
                }
              }
              // else: already selected, keep multi-selection for drag
            }
            // Compute time bounds from non-selected neighbors (segment-relative)
            m_envDragMinTime = 0.0;
            m_envDragMaxTime = segDuration;
            if (g_CountEnvelopePoints && g_GetEnvelopePoint) {
              int cnt = g_CountEnvelopePoints(env);
              // Find leftmost selected time and rightmost selected time
              double selMinTime = 1e30, selMaxTime = -1e30;
              for (int j = 0; j < cnt; j++) {
                double t=0,v=0,tn=0; int s=0; bool sel=false;
                g_GetEnvelopePoint(env, j, &t, &v, &s, &tn, &sel);
                if (sel) { if (t < selMinTime) selMinTime = t; if (t > selMaxTime) selMaxTime = t; }
              }
              // Find closest non-selected neighbors outside the selected range
              for (int j = 0; j < cnt; j++) {
                double t=0,v=0,tn=0; int s=0; bool sel=false;
                g_GetEnvelopePoint(env, j, &t, &v, &s, &tn, &sel);
                if (sel) continue;
                if (t < selMinTime && t > m_envDragMinTime) m_envDragMinTime = t + 0.0001;
                if (t > selMaxTime && t < m_envDragMaxTime) m_envDragMaxTime = t - 0.0001;
              }
            }
            // Start drag (moves all selected points)
            m_envDragging = true;
            m_envDragPointIdx = hitIdx;
            m_envDragEnv = env;
            m_envDragSegOffset = segOffset;
            m_envDragSegDuration = segDuration;
            SetCapture(m_hwnd);
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
          }
          // Check if click is near the envelope line (within 20px vertically)
          // Skip for dense envelopes - Cmd+drag falls through to reveal rect instead
          if (!isDenseEnv) {
          if (clickTime >= 0.0 && clickTime <= m_waveform.GetItemDuration()) {
            // ei already computed above via GetEnvelopeAtTime
            double rawVal = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
            g_Envelope_Evaluate(ei.env, ei.envTime, m_waveform.GetSampleRate(), 0,
                                &rawVal, &d1, &d2, &d3);
            int scalingMode = ei.scalingMode;
            double lineGain = g_ScaleFromEnvelopeMode(scalingMode, rawVal);
            int lineY = m_waveform.EnvYToGainY(lineGain, scalingMode);
            if (abs(y - lineY) <= 20) {
              // Use evaluated envelope value (not pixel-derived) to avoid precision loss
              double newRawVal = rawVal;
              bool cmdDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
              if (cmdDown) {
                // Cmd+click+drag on line = freehand envelope drawing
                if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
                bool noSort = true;
                g_InsertEnvelopePointEx(env, -1, ei.envTime, newRawVal, 0, 0.0, false, &noSort);
                m_envFreehand = true;
                m_envFreehandLastX = x;
                m_envDragPointIdx = -1;
                m_envDragEnv = env;
                m_envDragSegOffset = segOffset;
                m_envDragSegDuration = segDuration;
                SetCapture(m_hwnd);
              } else {
                // Plain click on line = add single point, start dragging it
                if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
                bool noSort = false;
                g_InsertEnvelopePointEx(env, -1, ei.envTime, newRawVal, 0, 0.0, false, &noSort);
                g_Envelope_SortPoints(env);
                if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Add envelope point", -1);
                if (g_UpdateArrange) g_UpdateArrange();
                int newIdx = g_GetEnvelopePointByTime ? g_GetEnvelopePointByTime(env, ei.envTime) : -1;
                if (newIdx >= 0) {
                  m_envDragging = true;
                  m_envDragPointIdx = newIdx;
                  m_envDragEnv = env;
                  m_envDragSegOffset = segOffset;
                  m_envDragSegDuration = segDuration;
                  SetCapture(m_hwnd);
                  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
                }
              }
              InvalidateRect(m_hwnd, nullptr, FALSE);
              return;
            }
          }
          } // !isDenseEnv
        }
      }

      // Check REAPER fade handles (16px hit zone around handle)
      if (!m_waveform.IsStandaloneMode() && g_GetMediaItemInfo_Value) {
        auto& segs = m_waveform.GetSegments();
        bool multi = segs.size() > 1 || m_waveform.IsTrackView();
        MediaItem* fadeInItem = (multi && !segs.empty()) ? segs.front().item : m_waveform.GetItem();
        MediaItem* fadeOutItem = (multi && !segs.empty()) ? segs.back().item : m_waveform.GetItem();
        double fadeInLen = fadeInItem ? g_GetMediaItemInfo_Value(fadeInItem, "D_FADEINLEN") : 0.0;
        double fadeOutLen = fadeOutItem ? g_GetMediaItemInfo_Value(fadeOutItem, "D_FADEOUTLEN") : 0.0;
        int fiX = m_waveform.TimeToX(fadeInLen);
        int foX = m_waveform.TimeToX(m_waveform.GetItemDuration() - fadeOutLen);
        if (abs(x - fiX) <= FADE_HANDLE_HIT_ZONE && y < m_waveformRect.top + FADE_HANDLE_TOP_ZONE) {
          m_fadeDragging = FADE_IN;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = fadeInItem ? g_GetMediaItemInfo_Value(fadeInItem, "D_FADEINDIR") : 0.0;
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          return;
        }
        if (abs(x - foX) <= FADE_HANDLE_HIT_ZONE && y < m_waveformRect.top + FADE_HANDLE_TOP_ZONE) {
          m_fadeDragging = FADE_OUT;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = fadeOutItem ? g_GetMediaItemInfo_Value(fadeOutItem, "D_FADEOUTDIR") : 0.0;
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          return;
        }
      }

      // Cmd+drag on empty area = envelope point selection rectangle
      {
        bool cmdDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (cmdDown && m_waveform.GetShowVolumeEnvelope() && !m_waveform.IsStandaloneMode()) {
          m_envRectSelecting = false;
          m_envRectStartX = x;
          m_envRectStartY = y;
          m_envRectEndX = x;
          m_envRectEndY = y;
          m_dragging = false;
          SetCapture(m_hwnd);
          // We'll activate rectangle on 5px threshold in OnMouseMove
          // Mark that we're in "pending rect" state using startX != 0
          return;
        }
      }

      double time = m_waveform.XToTime(x);

      // Option+click: snap selection to segment boundaries (SET, timeline, multi-item)
      bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      if (altHeld && m_waveform.IsMultiItem() && !(wParam & MK_SHIFT)) {
        auto& segs = m_waveform.GetSegments();
        for (const auto& seg : segs) {
          if (time >= seg.relativeOffset && time < seg.relativeOffset + seg.duration) {
            m_waveform.StartSelection(seg.relativeOffset);
            m_waveform.UpdateSelection(seg.relativeOffset + seg.duration);
            m_waveform.EndSelection();
            SyncSelectionToReaper();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
          }
        }
      }

      // Drag export: click inside existing selection
      // Alt+drag: immediate drag export (for external apps / Finder)
      // No modifier: drag export starts when mouse leaves waveform area (for REAPER timeline)
      if (m_waveform.HasSelection() && !(wParam & MK_SHIFT)) {
        WaveformSelection sel = m_waveform.GetSelection();
        double selS = std::min(sel.startTime, sel.endTime);
        double selE = std::max(sel.startTime, sel.endTime);
        if (time >= selS && time <= selE) {
          m_dragExportPending = true;
          m_dragExportImmediate = altHeld; // Alt = immediate, no Alt = on window exit
          m_dragStartX = x;
          m_dragStartY = y;
          SetCapture(m_hwnd);
          return;
        }
      }

      // Clicking on waveform (not on envelope point) deselects envelope point
      m_envDragPointIdx = -1;

      if (wParam & MK_SHIFT) {
        m_waveform.UpdateSelection(time);
      } else {
        m_waveform.StartSelection(time);
        m_waveform.SetCursorTime(time);
        if (g_SetEditCurPos)
          g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), true, false);
      }
      m_dragging = true;
      SetCapture(m_hwnd);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SneakPeak::OnMouseUp(int x, int y)
{
  // Finalize envelope selection rectangle (Cmd+drag)
  if (m_envRectStartX != 0 || m_envRectStartY != 0) {
    bool wasSelecting = m_envRectSelecting;
    int rx1 = std::min(m_envRectStartX, m_envRectEndX);
    int ry1 = std::min(m_envRectStartY, m_envRectEndY);
    int rx2 = std::max(m_envRectStartX, m_envRectEndX);
    int ry2 = std::max(m_envRectStartY, m_envRectEndY);
    m_envRectSelecting = false;
    m_envRectStartX = m_envRectStartY = 0;
    ReleaseCapture();
    if (wasSelecting && g_GetTakeEnvelopeByName && g_CountEnvelopePoints &&
        g_GetEnvelopePoint && g_SetEnvelopePoint && g_ScaleFromEnvelopeMode &&
        g_GetEnvelopeScalingMode) {
      bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      int totalCount = 0;
      // Select points from each segment's envelope (correct for timeline/SET)
      auto selectFromEnv = [&](TrackEnvelope* env, int sm, double segOff) {
        int cnt = g_CountEnvelopePoints(env);
        totalCount += cnt;
        bool noSort = true;
        for (int i = 0; i < cnt; i++) {
          double pt = 0, pv = 0, ptn = 0; int ps = 0; bool psel = false;
          g_GetEnvelopePoint(env, i, &pt, &pv, &ps, &ptn, &psel);
          double gain = g_ScaleFromEnvelopeMode(sm, pv);
          int px = m_waveform.TimeToX(pt + segOff);
          int py = m_waveform.EnvYToGainY(gain, sm);
          bool inside = (px >= rx1 && px <= rx2 && py >= ry1 && py <= ry2);
          bool newSel = shift ? (psel || inside) : inside;
          if (newSel != psel)
            g_SetEnvelopePoint(env, i, nullptr, nullptr, nullptr, nullptr, &newSel, &noSort);
        }
      };
      auto& segs = m_waveform.GetSegments();
      bool isMultiSeg = (m_waveform.IsTimelineView() || m_waveform.IsTrackView()) && segs.size() > 1;
      if (isMultiSeg) {
        for (const auto& seg : segs) {
          if (!seg.take) continue;
          TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
          if (!env) continue;
          int sm = g_GetEnvelopeScalingMode(env);
          selectFromEnv(env, sm, seg.relativeOffset);
        }
      } else if (m_waveform.GetTake()) {
        TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
        if (env) {
          int sm = g_GetEnvelopeScalingMode(env);
          selectFromEnv(env, sm, 0.0);
        }
      }
      // Dense envelope: persist as reveal range so points become visible
      if (totalCount > 100) {
        double tStart = std::max(0.0, m_waveform.XToTime(rx1));
        double tEnd = std::min(m_waveform.GetItemDuration(), m_waveform.XToTime(rx2));
        if (tEnd > tStart)
          m_waveform.SetEnvRevealRange(tStart, tEnd);
      }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_dragExportPending) {
    // Didn't drag outside — treat as click inside selection (place cursor)
    m_dragExportPending = false;
    m_dragExportImmediate = false;
    ReleaseCapture();
    double time = m_waveform.XToTime(x);
    m_waveform.SetCursorTime(time);
    if (g_SetEditCurPos)
      g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), true, false);
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
  if (m_dynamicsPanel.IsDragging()) {
    m_dynamicsPanel.OnMouseUp();
    // Close live undo block after slider drag completes
    if (m_dynamicsPanel.LiveUndoOpen()) {
      if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Live Dynamics", -1);
      m_dynamicsPanel.SetLiveUndoOpen(false);
    }
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_envFreehand) {
    m_envFreehand = false;
    ReleaseCapture();
    if (m_envDragEnv && g_Envelope_SortPoints) g_Envelope_SortPoints(m_envDragEnv);
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Freehand envelope drawing", -1);
    if (g_UpdateArrange) g_UpdateArrange();
    m_envDragPointIdx = -1;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_envDragging) {
    m_envDragging = false;
    // Keep m_envDragPointIdx — tracks "selected" point for Delete key
    ReleaseCapture();
    if (m_envDragEnv && g_Envelope_SortPoints) g_Envelope_SortPoints(m_envDragEnv);
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Move envelope point", -1);
    if (g_UpdateArrange) g_UpdateArrange();
    InvalidateRect(m_hwnd, nullptr, FALSE);
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

    // REAPER mode: apply gain on knob release
    if (wasKnobDrag && !m_waveform.IsStandaloneMode() && m_waveform.HasItem()) {
      double db = m_gainPanel.GetDb();
      bool isTimeline = m_waveform.IsTimelineView();
      bool isMulti = m_waveform.IsMultiItemActive();
      bool hasSel0 = m_waveform.HasSelection();
      DBG("[SneakPeak] GainRelease: db=%.2f isTimeline=%d isMulti=%d isTrackView=%d hasSel=%d batchGainOff=%.4f standaloneGain=%.4f\n",
          db, isTimeline, isMulti, m_waveform.IsTrackView(), hasSel0,
          m_waveform.GetBatchGainOffset(), 1.0 /*no getter for standalone gain*/);
      bool deferClearGain = isTimeline || isMulti || hasSel0;
      if (!deferClearGain) m_waveform.ClearStandaloneGain();
      WaveformSelection savedSel = m_waveform.GetSelection();
      double savedViewStart = m_waveform.GetViewStart();
      double savedViewDur = m_waveform.GetViewDuration();
      double savedCursor = m_waveform.GetCursorTime();

      if (std::abs(db) > 0.01) {
        double factor = pow(10.0, db / 20.0);
        bool hasSel = m_waveform.HasSelection();

        if (m_workingSet.active && hasSel && m_workingSet.track) {
          // SET mode with selection: split + D_VOL + crossfade overlap
          double absStart = m_waveform.RelTimeToAbsTime(std::min(savedSel.startTime, savedSel.endTime));
          double absEnd = m_waveform.RelTimeToAbsTime(std::max(savedSel.startTime, savedSel.endTime));
          MediaTrack* track = m_workingSet.track;
          if (g_ValidatePtr2 && g_ValidatePtr2(nullptr, track, "MediaTrack*")) {
            if (g_PreventUIRefresh) g_PreventUIRefresh(1);
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            SplitGainParams p{absStart, absEnd, factor, GAIN_EDGE_EPS_SET, GAIN_XFADE_SEC};
            SplitAndApplyGain(track, p);
            if (g_UpdateArrange) g_UpdateArrange();
            char desc[64];
            snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB (selection)", db);
            if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
            if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

            // Update working set items — split created new items not in the original list
            if (g_GetTrackNumMediaItems && g_GetTrackMediaItem) {
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
          }
        } else if (m_workingSet.active && g_SetMediaItemInfo_Value && g_GetMediaItemInfo_Value) {
          // SET mode without selection: apply D_VOL to all items in set
          if (g_PreventUIRefresh) g_PreventUIRefresh(1);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          for (const auto& seg : m_waveform.GetSegments()) {
            if (!seg.item) continue;
            if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, seg.item, "MediaItem*")) continue;
            double v = g_GetMediaItemInfo_Value(seg.item, "D_VOL");
            g_SetMediaItemInfo_Value(seg.item, "D_VOL", v * factor);
          }
          if (g_UpdateArrange) g_UpdateArrange();
          char desc[64];
          snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB", db);
          if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
          if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
        } else if (m_waveform.IsTimelineOrMultiItem() && hasSel) {
          // If selection covers entire timeline, use simpler no-split D_VOL path
          double selMin = std::min(savedSel.startTime, savedSel.endTime);
          double selMax = std::max(savedSel.startTime, savedSel.endTime);
          bool coversAll = (selMin < 0.01 && selMax > m_waveform.GetItemDuration() - 0.01);
          if (coversAll) goto applyWholeTimeline;

          // Timeline/Multi-item with selection: split + D_VOL (no crossfade)
          double absStart = m_waveform.RelTimeToAbsTime(selMin);
          double absEnd = m_waveform.RelTimeToAbsTime(selMax);
          MediaTrack* trk = g_GetMediaItem_Track ? g_GetMediaItem_Track(m_waveform.GetItem()) : nullptr;
          if (trk) {
            // Preserve fades from first/last segments before split
            const auto& segs = m_waveform.GetSegments();
            MediaItem* firstSeg = segs.front().item;
            MediaItem* lastSeg = segs.back().item;
            double sFadeInLen = firstSeg ? g_GetMediaItemInfo_Value(firstSeg, "D_FADEINLEN") : 0.0;
            int sFadeInShape = firstSeg ? (int)g_GetMediaItemInfo_Value(firstSeg, "C_FADEINSHAPE") : 0;
            double sFadeInDir = firstSeg ? g_GetMediaItemInfo_Value(firstSeg, "D_FADEINDIR") : 0.0;
            double sFadeOutLen = lastSeg ? g_GetMediaItemInfo_Value(lastSeg, "D_FADEOUTLEN") : 0.0;
            int sFadeOutShape = lastSeg ? (int)g_GetMediaItemInfo_Value(lastSeg, "C_FADEOUTSHAPE") : 0;
            double sFadeOutDir = lastSeg ? g_GetMediaItemInfo_Value(lastSeg, "D_FADEOUTDIR") : 0.0;

            if (g_PreventUIRefresh) g_PreventUIRefresh(1);
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            SplitGainParams p{absStart, absEnd, factor, GAIN_EDGE_EPS, 0.0};
            SplitAndApplyGain(trk, p);
            if (g_UpdateArrange) g_UpdateArrange();
            char desc[64];
            snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB (selection)", db);
            if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
            if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

            // Rebuild timeline (segments are stale after split) + restore fades
            double tlStart = segs.front().position;
            double tlEnd = segs.back().position + segs.back().duration;
            if (g_GetTrackNumMediaItems && g_GetTrackMediaItem) {
              std::vector<MediaItem*> rebuilt;
              int cnt = g_GetTrackNumMediaItems(trk);
              for (int i = 0; i < cnt; i++) {
                MediaItem* mi = g_GetTrackMediaItem(trk, i);
                if (!mi) continue;
                double mpos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
                double mend = mpos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
                if (mpos >= tlStart - 0.001 && mend <= tlEnd + 0.001)
                  rebuilt.push_back(mi);
              }
              if (rebuilt.size() >= 2) {
                std::sort(rebuilt.begin(), rebuilt.end(), [](MediaItem* a, MediaItem* b) {
                  return g_GetMediaItemInfo_Value(a, "D_POSITION") < g_GetMediaItemInfo_Value(b, "D_POSITION");
                });
                if (sFadeInLen > 0.0) {
                  g_SetMediaItemInfo_Value(rebuilt.front(), "D_FADEINLEN", sFadeInLen);
                  g_SetMediaItemInfo_Value(rebuilt.front(), "C_FADEINSHAPE", (double)sFadeInShape);
                  g_SetMediaItemInfo_Value(rebuilt.front(), "D_FADEINDIR", sFadeInDir);
                }
                if (sFadeOutLen > 0.0) {
                  g_SetMediaItemInfo_Value(rebuilt.back(), "D_FADEOUTLEN", sFadeOutLen);
                  g_SetMediaItemInfo_Value(rebuilt.back(), "C_FADEOUTSHAPE", (double)sFadeOutShape);
                  g_SetMediaItemInfo_Value(rebuilt.back(), "D_FADEOUTDIR", sFadeOutDir);
                }
                if (g_UpdateArrange) g_UpdateArrange();
                m_waveform.ClearItem();
                m_waveform.LoadTimelineView(rebuilt);
                { std::vector<MediaItem*> si;
                  for (const auto& s : m_waveform.GetSegments()) if (s.item) si.push_back(s.item);
                  if (!si.empty()) m_gainPanel.ShowBatch(si);
                }
                m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
              }
            }
            db = 0.0; // gain already in D_VOL + freshly loaded audio
          }
        } else if ((m_waveform.IsTimelineOrMultiItem()) && g_SetMediaItemInfo_Value && g_GetMediaItemInfo_Value) {
          applyWholeTimeline:
          DBG("[SneakPeak] GainPath: TIMELINE/MULTI noSel D_VOL all segs factor=%.4f\n", factor);
          if (g_PreventUIRefresh) g_PreventUIRefresh(1);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          for (const auto& seg : m_waveform.GetSegments()) {
            if (!seg.item) continue;
            if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, seg.item, "MediaItem*")) continue;
            double v = g_GetMediaItemInfo_Value(seg.item, "D_VOL");
            g_SetMediaItemInfo_Value(seg.item, "D_VOL", v * factor);
          }
          if (g_UpdateArrange) g_UpdateArrange();
          char desc[64];
          snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB", db);
          if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
          if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
          // Rebuild timeline view to pick up new D_VOL values in audio buffer
          MediaTrack* trk = g_GetMediaItem_Track ? g_GetMediaItem_Track(m_waveform.GetItem()) : nullptr;
          if (trk && g_GetTrackNumMediaItems && g_GetTrackMediaItem) {
            const auto& segs = m_waveform.GetSegments();
            double tlStart = segs.front().position;
            double tlEnd = segs.back().position + segs.back().duration;
            std::vector<MediaItem*> rebuilt;
            int cnt = g_GetTrackNumMediaItems(trk);
            for (int i = 0; i < cnt; i++) {
              MediaItem* mi = g_GetTrackMediaItem(trk, i);
              if (!mi) continue;
              double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
              double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
              if (pos >= tlStart - 0.001 && end <= tlEnd + 0.001)
                rebuilt.push_back(mi);
            }
            if (rebuilt.size() >= 2) {
              m_waveform.ClearItem();
              m_waveform.LoadTimelineView(rebuilt);
              { std::vector<MediaItem*> si;
                for (const auto& s : m_waveform.GetSegments()) if (s.item) si.push_back(s.item);
                if (!si.empty()) m_gainPanel.ShowBatch(si);
              }
              m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
              db = 0.0; // gain already in D_VOL + freshly loaded audio
            }
          }
        } else if (hasSel) {
          // Single-item with selection: split + D_VOL, then enter timeline view
          MediaItem* item = m_waveform.GetItem();
          MediaTrack* trk = g_GetMediaItem_Track ? g_GetMediaItem_Track(item) : nullptr;
          if (item && trk) {
            double absStart = m_waveform.RelTimeToAbsTime(std::min(savedSel.startTime, savedSel.endTime));
            double absEnd = m_waveform.RelTimeToAbsTime(std::max(savedSel.startTime, savedSel.endTime));
            double origPos = m_waveform.GetItemPosition();
            double origEnd = origPos + m_waveform.GetItemDuration();

            // Preserve fade params before split (REAPER may strip them from split fragments)
            double savedFadeInLen = g_GetMediaItemInfo_Value(item, "D_FADEINLEN");
            int savedFadeInShape = (int)g_GetMediaItemInfo_Value(item, "C_FADEINSHAPE");
            double savedFadeInDir = g_GetMediaItemInfo_Value(item, "D_FADEINDIR");
            double savedFadeOutLen = g_GetMediaItemInfo_Value(item, "D_FADEOUTLEN");
            int savedFadeOutShape = (int)g_GetMediaItemInfo_Value(item, "C_FADEOUTSHAPE");
            double savedFadeOutDir = g_GetMediaItemInfo_Value(item, "D_FADEOUTDIR");

            if (g_PreventUIRefresh) g_PreventUIRefresh(1);
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            SplitGainParams p{absStart, absEnd, factor, GAIN_EDGE_EPS, 0.0};
            SplitAndApplyGainSingle(item, p);

            // Collect siblings sorted by position
            std::vector<MediaItem*> siblings;
            if (g_GetTrackNumMediaItems && g_GetTrackMediaItem) {
              int cnt = g_GetTrackNumMediaItems(trk);
              for (int i = 0; i < cnt; i++) {
                MediaItem* mi = g_GetTrackMediaItem(trk, i);
                if (!mi) continue;
                double sp = g_GetMediaItemInfo_Value(mi, "D_POSITION");
                double se = sp + g_GetMediaItemInfo_Value(mi, "D_LENGTH");
                if (sp >= origPos - 0.001 && se <= origEnd + 0.001)
                  siblings.push_back(mi);
              }
              std::sort(siblings.begin(), siblings.end(), [](MediaItem* a, MediaItem* b) {
                return g_GetMediaItemInfo_Value(a, "D_POSITION") < g_GetMediaItemInfo_Value(b, "D_POSITION");
              });
            }

            // Re-apply fade params to leftmost/rightmost surviving items
            if (!siblings.empty()) {
              if (savedFadeInLen > 0.0) {
                g_SetMediaItemInfo_Value(siblings.front(), "D_FADEINLEN", savedFadeInLen);
                g_SetMediaItemInfo_Value(siblings.front(), "C_FADEINSHAPE", (double)savedFadeInShape);
                g_SetMediaItemInfo_Value(siblings.front(), "D_FADEINDIR", savedFadeInDir);
              }
              if (savedFadeOutLen > 0.0) {
                g_SetMediaItemInfo_Value(siblings.back(), "D_FADEOUTLEN", savedFadeOutLen);
                g_SetMediaItemInfo_Value(siblings.back(), "C_FADEOUTSHAPE", (double)savedFadeOutShape);
                g_SetMediaItemInfo_Value(siblings.back(), "D_FADEOUTDIR", savedFadeOutDir);
              }
            }

            if (g_UpdateArrange) g_UpdateArrange();
            char desc[64];
            snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB (selection)", db);
            if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
            if (g_PreventUIRefresh) g_PreventUIRefresh(-1);

            // Enter timeline view
            if (siblings.size() >= 2) {
              m_waveform.ClearItem();
              m_waveform.LoadTimelineView(siblings);
              { std::vector<MediaItem*> segItems;
                for (const auto& seg : m_waveform.GetSegments()) if (seg.item) segItems.push_back(seg.item);
                if (!segItems.empty()) m_gainPanel.ShowBatch(segItems);
              }
              m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
            }
            // Gain already applied via D_VOL + freshly loaded audio — prevent double-apply
            db = 0.0;
          }
        } else {
          DBG("[SneakPeak] GainPath: SINGLE noSel (WriteToItem already applied)\n");
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          char desc[64];
          snprintf(desc, sizeof(desc), "SneakPeak: Gain %.1fdB", db);
          if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);
        }
      }

      ReloadAfterGainChange(savedViewStart, savedViewDur, savedSel, savedCursor, db);
      if (deferClearGain) m_waveform.ClearStandaloneGain();
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
  // Envelope selection rectangle (Cmd+left-drag)
  if (m_envRectStartX != 0 || m_envRectStartY != 0) {
    int dx = x - m_envRectStartX;
    int dy = y - m_envRectStartY;
    if (!m_envRectSelecting && (dx * dx + dy * dy > 25)) {
      m_envRectSelecting = true;
    }
    if (m_envRectSelecting) {
      m_envRectEndX = x;
      m_envRectEndY = y;
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    return;
  }

  // Drag export: check threshold
  if (m_dragExportPending) {
    int dx = x - m_dragStartX;
    int dy = y - m_dragStartY;
    bool shouldExport = false;
    if (m_dragExportImmediate) {
      // Alt+drag: immediate on 5px threshold
      shouldExport = (dx * dx + dy * dy > 25);
    } else {
      // No modifier: export when mouse leaves waveform area
      shouldExport = (y < m_waveformRect.top || y > m_waveformRect.bottom ||
                      x < m_waveformRect.left || x > m_waveformRect.right);
    }
    if (shouldExport) {
      m_dragExportPending = false;
      m_dragExportImmediate = false;
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

  // Freehand envelope drawing (uses m_envDragEnv for correct segment in timeline/SET)
  if (m_envFreehand && m_waveform.HasItem()) {
    // Add point every 4 pixels for smooth but not excessive density
    if (abs(x - m_envFreehandLastX) >= 4) {
      TrackEnvelope* env = m_envDragEnv;
      if (env && g_InsertEnvelopePointEx && g_GetEnvelopeScalingMode &&
          g_ScaleToEnvelopeMode && g_Envelope_SortPoints &&
          g_CountEnvelopePoints && g_GetEnvelopePoint && g_DeleteEnvelopePointEx) {
        // Convert view-relative time to segment-relative envelope time
        double viewTime = m_waveform.XToTime(x);
        double time = viewTime - m_envDragSegOffset;
        time = std::max(0.0, std::min(m_envDragSegDuration, time));
        // Delete existing points between last drawn position and current
        double prevViewTime = m_waveform.XToTime(m_envFreehandLastX);
        double prevTime = prevViewTime - m_envDragSegOffset;
        double tMin = std::min(prevTime, time);
        double tMax = std::max(prevTime, time);
        int cnt = g_CountEnvelopePoints(env);
        for (int i = cnt - 1; i >= 0; i--) {
          double pt = 0, pv = 0, ptn = 0; int ps = 0; bool psel = false;
          g_GetEnvelopePoint(env, i, &pt, &pv, &ps, &ptn, &psel);
          if (pt > tMin && pt < tMax)
            g_DeleteEnvelopePointEx(env, -1, i);
        }
        int scalingMode = g_GetEnvelopeScalingMode(env);
        double gain = m_waveform.EnvPixelToGain(y, scalingMode);
        double rawVal = g_ScaleToEnvelopeMode(scalingMode, gain);
        bool noSort = false;
        g_InsertEnvelopePointEx(env, -1, time, rawVal, 0, 0.0, false, &noSort);
        g_Envelope_SortPoints(env);
        m_envFreehandLastX = x;
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    return;
  }

  // Envelope point dragging (moves all selected points by delta, clamped to neighbors)
  if (m_envDragging && m_envDragPointIdx >= 0 && m_waveform.HasItem()) {
    TrackEnvelope* env = m_envDragEnv;
    if (env && g_SetEnvelopePoint && g_GetEnvelopePoint && g_CountEnvelopePoints &&
        g_GetEnvelopeScalingMode && g_ScaleToEnvelopeMode && g_ScaleFromEnvelopeMode) {
      double timeDelta = m_waveform.XToTime(x) - m_waveform.XToTime(m_lastMouseX);
      int scalingMode = g_GetEnvelopeScalingMode(env);
      double gainDelta = m_waveform.EnvPixelToGain(y, scalingMode) - m_waveform.EnvPixelToGain(m_lastMouseY, scalingMode);
      // Clamp timeDelta so no selected point crosses a non-selected neighbor
      int cnt = g_CountEnvelopePoints(env);
      double selMin = 1e30, selMax = -1e30;
      for (int i = 0; i < cnt; i++) {
        double pt = 0, pv = 0, ptn = 0; int ps = 0; bool psel = false;
        g_GetEnvelopePoint(env, i, &pt, &pv, &ps, &ptn, &psel);
        if (psel) { if (pt < selMin) selMin = pt; if (pt > selMax) selMax = pt; }
      }
      double maxLeftDelta = m_envDragMinTime - selMin;   // negative (move left)
      double maxRightDelta = m_envDragMaxTime - selMax;  // positive (move right)
      timeDelta = std::max(maxLeftDelta, std::min(maxRightDelta, timeDelta));

      bool noSort = true;
      for (int i = 0; i < cnt; i++) {
        double pt = 0, pv = 0, ptn = 0; int ps = 0; bool psel = false;
        if (!g_GetEnvelopePoint(env, i, &pt, &pv, &ps, &ptn, &psel)) continue;
        if (!psel) continue;
        double newTime = pt + timeDelta;
        double curGain = g_ScaleFromEnvelopeMode(scalingMode, pv);
        double newGain = std::max(0.0, curGain + gainDelta);
        double newRawVal = g_ScaleToEnvelopeMode(scalingMode, newGain);
        g_SetEnvelopePoint(env, i, &newTime, &newRawVal, nullptr, nullptr, nullptr, &noSort);
      }
      if (g_UpdateArrange) g_UpdateArrange();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    return;
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
        sf.fadeInLen = std::max(0.0, std::min(time, dur - sf.fadeOutLen));
        sf.fadeInDir = newDir;
      } else {
        sf.fadeOutLen = std::max(0.0, dur - time);
        sf.fadeOutLen = std::min(sf.fadeOutLen, dur - sf.fadeInLen);
        sf.fadeOutDir = newDir;
      }
      m_waveform.SetStandaloneFade(sf);
      m_waveform.SetFadeDragInfo((m_fadeDragging == FADE_IN) ? 1 : 2,
        (m_fadeDragging == FADE_IN) ? sf.fadeInShape : sf.fadeOutShape);
    } else if (g_SetMediaItemInfo_Value) {
      auto& segs = m_waveform.GetSegments();
      bool multi = segs.size() > 1 || m_waveform.IsTrackView();
      if (m_fadeDragging == FADE_IN) {
        MediaItem* item = (multi && !segs.empty()) ? segs.front().item : m_waveform.GetItem();
        double maxLen = (multi && !segs.empty()) ? segs.front().duration : dur;
        double fadeOutLen = g_GetMediaItemInfo_Value(item, "D_FADEOUTLEN");
        double fadeLen = std::max(0.0, std::min(time, maxLen - fadeOutLen));
        g_SetMediaItemInfo_Value(item, "D_FADEINLEN", fadeLen);
        g_SetMediaItemInfo_Value(item, "D_FADEINDIR", newDir);
        m_waveform.SetFadeDragInfo(1, (int)g_GetMediaItemInfo_Value(item, "C_FADEINSHAPE"));
      } else {
        MediaItem* item = (multi && !segs.empty()) ? segs.back().item : m_waveform.GetItem();
        double maxLen = (multi && !segs.empty()) ? segs.back().duration : dur;
        double fadeInLen = g_GetMediaItemInfo_Value(item, "D_FADEINLEN");
        double fadeLen = std::max(0.0, dur - time);
        fadeLen = std::min(fadeLen, maxLen - fadeInLen);
        g_SetMediaItemInfo_Value(item, "D_FADEOUTLEN", fadeLen);
        g_SetMediaItemInfo_Value(item, "D_FADEOUTDIR", newDir);
        m_waveform.SetFadeDragInfo(2, (int)g_GetMediaItemInfo_Value(item, "C_FADEOUTSHAPE"));
      }
      if (g_UpdateArrange) g_UpdateArrange();
      if (g_UpdateTimeline) g_UpdateTimeline();
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

  if (m_dynamicsPanel.IsDragging()) {
    m_dynamicsPanel.OnMouseMove(x, y, m_waveformRect);
    // Real-time reanalysis on slider drag
    if (m_dynamicsPanel.ParamsChanged()) {
      m_dynamicsPanel.ClearParamsChanged();
      m_dynamics.SetParams(m_dynamicsPanel.GetParams());
      if (m_waveform.GetAudioSampleCount() > 0) {
        double ivDb = 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
        m_dynamics.Analyze(m_waveform.GetAudioData().data(),
                           m_waveform.GetAudioSampleCount(),
                           m_waveform.GetNumChannels(),
                           m_waveform.GetSampleRate(),
                           ivDb, m_dynamicsPanel.GetParams());
        // Update GR meter (compute compression to get avg GR)
        m_dynamics.ComputeCompression();
        m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());

        // Live mode: write envelope points in real-time
        if (m_dynamicsPanel.IsLive()) {
          if (!m_dynamicsPanel.LiveUndoOpen()) {
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            m_dynamicsPanel.SetLiveUndoOpen(true);
          }
          ApplyDynamicsToEnvelope();
        }
      }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  if (m_dragging && m_waveform.HasItem()) {
    // Clamp x to waveform area (exclude dB scale)
    int waveRight = m_waveform.GetRect().right - DB_SCALE_WIDTH;
    int clampedX = std::max((int)m_waveform.GetRect().left, std::min(waveRight, x));
    m_waveform.UpdateSelection(m_waveform.XToTime(clampedX));
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

  // Use GetAsyncKeyState for all modifiers — SWELL doesn't populate MK_ flags in wParam
  bool cmd = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; // Cmd on macOS
  bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;    // Option on macOS

  double steps = (double)delta / 120.0;

  // Scroll on dB scale column = vertical zoom
  int dbScaleLeft = m_waveformRect.right - DB_SCALE_WIDTH;
  if (x >= dbScaleLeft && x <= m_waveformRect.right &&
      y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    m_waveform.ZoomVertical((float)pow(1.15, steps));
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (alt) {
    // Option+Scroll = vertical zoom
    m_waveform.ZoomVertical((float)pow(1.15, steps));
  } else if (cmd) {
    // Cmd+Scroll = horizontal pan (may not reach us when docked — trackpad pan via WM_MOUSEHWHEEL)
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
        if (g_SetEditCurPos) g_SetEditCurPos(m_waveform.RelTimeToAbsTime(0.0), true, false);
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
          // Play selection only if cursor is inside it; otherwise play from cursor
          WaveformSelection sel = m_waveform.GetSelection();
          double selMin = std::min(sel.startTime, sel.endTime);
          double selMax = std::max(sel.startTime, sel.endTime);
          double cursor = m_waveform.GetCursorTime();
          if (cursor >= selMin && cursor <= selMax) {
            DoLoopSelection();
          } else {
            m_startedPlayback = true;
            m_autoStopped = false;
            m_playGraceTicks = PLAY_GRACE_TICKS;
            g_OnPlayButton();
          }
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
      if (m_workingSet.active) {
        ExitWorkingSet();
      } else if (m_waveform.HasSelection()) {
        m_waveform.ClearSelection();
        SyncSelectionToReaper();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (g_OnStopButton) {
        g_OnStopButton();
      }
      break;
    case VK_DELETE:
    case VK_BACK: {
      bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      // Delete selected envelope points (or last-clicked if none selected)
      // Iterates all segments' envelopes for correct timeline/SET behavior
      if (!ctrl && !shift && m_waveform.GetShowVolumeEnvelope() && !m_waveform.IsStandaloneMode() &&
          g_GetTakeEnvelopeByName && g_DeleteEnvelopePointEx && g_Envelope_SortPoints &&
          g_CountEnvelopePoints && g_GetEnvelopePoint) {
        auto deleteSelectedFromEnv = [](TrackEnvelope* env) -> bool {
          int cnt = g_CountEnvelopePoints(env);
          bool anyDel = false;
          for (int i = cnt - 1; i >= 0; i--) {
            double t=0,v=0,tn=0; int s=0; bool sel=false;
            g_GetEnvelopePoint(env, i, &t, &v, &s, &tn, &sel);
            if (sel) { g_DeleteEnvelopePointEx(env, -1, i); anyDel = true; }
          }
          if (anyDel) g_Envelope_SortPoints(env);
          return anyDel;
        };
        auto hasSelectedInEnv = [](TrackEnvelope* env) -> bool {
          int cnt = g_CountEnvelopePoints(env);
          for (int i = 0; i < cnt; i++) {
            double t=0,v=0,tn=0; int s=0; bool sel=false;
            g_GetEnvelopePoint(env, i, &t, &v, &s, &tn, &sel);
            if (sel) return true;
          }
          return false;
        };
        bool anySelected = false;
        auto& segs = m_waveform.GetSegments();
        bool isMultiSeg = (m_waveform.IsTimelineView() || m_waveform.IsTrackView()) && segs.size() > 1;
        if (isMultiSeg) {
          for (const auto& seg : segs) {
            if (!seg.take) continue;
            TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
            if (env && hasSelectedInEnv(env)) { anySelected = true; break; }
          }
        } else if (m_waveform.GetTake()) {
          TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
          if (env && hasSelectedInEnv(env)) anySelected = true;
        }
        if (anySelected) {
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          if (isMultiSeg) {
            for (const auto& seg : segs) {
              if (!seg.take) continue;
              TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
              if (env) deleteSelectedFromEnv(env);
            }
          } else if (m_waveform.GetTake()) {
            TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
            if (env) deleteSelectedFromEnv(env);
          }
          if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Delete envelope points", -1);
          m_envDragPointIdx = -1;
          m_envDragEnv = nullptr;
          if (g_UpdateArrange) g_UpdateArrange();
          InvalidateRect(m_hwnd, nullptr, FALSE);
          break;
        } else if (m_envDragPointIdx >= 0 && m_envDragEnv) {
          // Fallback: delete last-clicked point from its specific envelope
          int cnt = g_CountEnvelopePoints(m_envDragEnv);
          if (m_envDragPointIdx < cnt) {
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            g_DeleteEnvelopePointEx(m_envDragEnv, -1, m_envDragPointIdx);
            g_Envelope_SortPoints(m_envDragEnv);
            if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Delete envelope point", -1);
            m_envDragPointIdx = -1;
            m_envDragEnv = nullptr;
            if (g_UpdateArrange) g_UpdateArrange();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            break;
          }
        }
      }
      if (ctrl) {
        DoSilence();
      } else {
        DoDelete(shift); // Shift+Delete = ripple delete
      }
      break;
    }
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
        // In track view, set cursor position for split
        if (m_workingSet.active && !m_waveform.HasSelection() && g_SetEditCurPos) {
          double absTime = m_waveform.RelTimeToAbsTime(m_waveform.GetCursorTime());
          g_SetEditCurPos(absTime, true, false);
        }
        SyncSelectionToReaper();
        if (m_waveform.HasSelection() && g_Main_OnCommand) {
          g_Main_OnCommand(40061, 0); // Split at time selection (both edges)
        } else if (g_Main_OnCommand) {
          g_Main_OnCommand(40012, 0); // Split at edit cursor
        }
        if (m_workingSet.active) RefreshWorkingSet();
      }
      break;
    case 'N':
      if (ctrl) DoNormalize();
      break;
    case 'E':
    case 'e':
      if (!ctrl) {
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        // Delete selected envelope points first (iterates all segments for timeline/SET)
        if (!shift && m_waveform.GetShowVolumeEnvelope() && !m_waveform.IsStandaloneMode() &&
            g_GetTakeEnvelopeByName && g_DeleteEnvelopePointEx && g_Envelope_SortPoints &&
            g_CountEnvelopePoints && g_GetEnvelopePoint) {
          auto hasSelEnv = [](TrackEnvelope* env) -> bool {
            int cnt = g_CountEnvelopePoints(env);
            for (int i = 0; i < cnt; i++) {
              double t=0,v=0,tn=0; int s=0; bool sel=false;
              g_GetEnvelopePoint(env, i, &t, &v, &s, &tn, &sel);
              if (sel) return true;
            }
            return false;
          };
          auto delSelEnv = [](TrackEnvelope* env) {
            int cnt = g_CountEnvelopePoints(env);
            for (int i = cnt - 1; i >= 0; i--) {
              double t=0,v=0,tn=0; int s=0; bool sel=false;
              g_GetEnvelopePoint(env, i, &t, &v, &s, &tn, &sel);
              if (sel) g_DeleteEnvelopePointEx(env, -1, i);
            }
            g_Envelope_SortPoints(env);
          };
          bool anySelected = false;
          auto& segs = m_waveform.GetSegments();
          bool isMultiSeg = (m_waveform.IsTimelineView() || m_waveform.IsTrackView()) && segs.size() > 1;
          if (isMultiSeg) {
            for (const auto& seg : segs) {
              if (!seg.take) continue;
              TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
              if (env && hasSelEnv(env)) { anySelected = true; break; }
            }
          } else if (m_waveform.GetTake()) {
            TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
            if (env && hasSelEnv(env)) anySelected = true;
          }
          if (anySelected) {
            if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
            if (isMultiSeg) {
              for (const auto& seg : segs) {
                if (!seg.take) continue;
                TrackEnvelope* env = g_GetTakeEnvelopeByName(seg.take, "Volume");
                if (env) delSelEnv(env);
              }
            } else if (m_waveform.GetTake()) {
              TrackEnvelope* env = g_GetTakeEnvelopeByName(m_waveform.GetTake(), "Volume");
              if (env) delSelEnv(env);
            }
            if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Delete envelope points", -1);
            m_envDragPointIdx = -1;
            m_envDragEnv = nullptr;
            if (g_UpdateArrange) g_UpdateArrange();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            break;
          }
        }
        DoDelete(shift);
      }
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
    case 'T':
      if (!ctrl) ToggleTrackView();
      break;

    case VK_UP:
    case VK_DOWN: {
      if (!m_gainPanel.IsVisible()) break;
      m_gainPanel.AdjustDb(key == VK_UP ? 1.0 : -1.0);
      m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS; // suppress reload bounce
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    }

    case VK_LEFT:
    case VK_RIGHT: {
      // Alt+Arrow = navigate between segments (SET/timeline/multi-item)
      bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      if (!altHeld) break;
      if (!m_waveform.IsTimelineOrMultiItem() && !m_waveform.IsTrackView()) break;
      const auto& segs = m_waveform.GetSegments();
      if (segs.size() < 2) break;

      // Find current segment (by selection midpoint or cursor)
      double pos = m_waveform.HasSelection()
          ? (m_waveform.GetSelection().startTime + m_waveform.GetSelection().endTime) * 0.5
          : m_waveform.GetCursorTime();
      int curIdx = -1;
      for (int i = 0; i < (int)segs.size(); i++) {
        if (pos >= segs[i].relativeOffset &&
            pos < segs[i].relativeOffset + segs[i].duration) {
          curIdx = i;
          break;
        }
      }

      int nextIdx = (key == VK_RIGHT)
          ? std::min(curIdx + 1, (int)segs.size() - 1)
          : std::max(curIdx - 1, 0);
      if (nextIdx == curIdx && curIdx >= 0) break;
      if (curIdx < 0) nextIdx = 0; // no current segment, go to first

      // Select the target segment in SneakPeak
      const auto& seg = segs[nextIdx];
      m_waveform.StartSelection(seg.relativeOffset);
      m_waveform.UpdateSelection(seg.relativeOffset + seg.duration);
      m_waveform.EndSelection();

      // Suppress LoadSelectedItem from exiting timeline when we change REAPER selection
      m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;

      // Select the corresponding REAPER item (SET/timeline only, not multi-item)
      // Changing REAPER selection in multi-item mode would destroy the view
      if (seg.item && !m_waveform.IsMultiItemActive() &&
          g_SetMediaItemInfo_Value && g_CountSelectedMediaItems &&
          g_GetSelectedMediaItem && g_UpdateArrange) {
        for (int si = g_CountSelectedMediaItems(nullptr) - 1; si >= 0; si--) {
          MediaItem* mi = g_GetSelectedMediaItem(nullptr, si);
          if (mi) g_SetMediaItemInfo_Value(mi, "B_UISEL", 0.0);
        }
        g_SetMediaItemInfo_Value(seg.item, "B_UISEL", 1.0);
        g_UpdateArrange();
      }

      // Move cursor into segment and sync to REAPER
      m_waveform.SetCursorTime(seg.relativeOffset);
      if (g_SetEditCurPos)
        g_SetEditCurPos(m_waveform.RelTimeToAbsTime(seg.relativeOffset), false, false);

      // If playing, jump playback to the new segment
      bool isPlaying = g_GetPlayState && (g_GetPlayState() & 1);
      if (isPlaying) {
        SyncSelectionToReaper();
        DoLoopSelection();
      }

      // Scroll to show the segment, clamped to audio bounds
      double segEnd = seg.relativeOffset + seg.duration;
      if (seg.relativeOffset < m_waveform.GetViewStart() ||
          segEnd > m_waveform.GetViewEnd()) {
        double pad = m_waveform.GetViewDuration() * 0.1;
        double vs = std::max(0.0, seg.relativeOffset - pad);
        double maxStart = std::max(0.0, m_waveform.GetItemDuration() - m_waveform.GetViewDuration());
        m_waveform.SetViewStart(std::min(vs, maxStart));
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
      break;
    }
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

void SneakPeak::ReloadAfterGainChange(double savedViewStart, double savedViewDur,
                                       const WaveformSelection& savedSel, double savedCursor, double db)
{
  // Suppress external audio change detection and preserve selection after any gain change
  m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
  m_pendingSelRestore = savedSel;

  if (m_workingSet.active) {
    RefreshWorkingSet();
    if (m_waveform.GetItemDuration() > 0) {
      m_waveform.SetViewStart(std::min(savedViewStart, m_waveform.GetItemDuration()));
      m_waveform.SetViewDuration(savedViewDur);
    }
    if (savedSel.active) m_waveform.SetSelection(savedSel);
    m_waveform.SetCursorTime(std::min(savedCursor, m_waveform.GetItemDuration()));
  } else if (m_waveform.IsMultiItemActive()) {
    if (savedSel.active && std::abs(db) > 0.01) {
      // Selection gain: scale the visible range in multi-item layers
      // D_VOL already written to REAPER items; defer full reload to timer
      double f = pow(10.0, db / 20.0);
      double selS = std::min(savedSel.startTime, savedSel.endTime);
      double selE = std::max(savedSel.startTime, savedSel.endTime);
      m_waveform.ScaleAudioRange(f, selS, selE);
      m_waveform.SetSelection(savedSel);
    } else if (std::abs(db) > 0.01) {
      double f = pow(10.0, db / 20.0);
      m_waveform.ScaleAudioBuffer(f);
    }
  } else if (m_waveform.IsTimelineView()) {
    m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
    if (savedSel.active && std::abs(db) > 0.01) {
      double f = pow(10.0, db / 20.0);
      double selS = std::min(savedSel.startTime, savedSel.endTime);
      double selE = std::max(savedSel.startTime, savedSel.endTime);
      m_waveform.ScaleAudioRange(f, selS, selE);
      m_waveform.SetSelection(savedSel);
    } else if (std::abs(db) > 0.01) {
      double f = pow(10.0, db / 20.0);
      m_waveform.ScaleAudioBuffer(f);
    }
  }

  // Reset knob and visual state
  if (m_gainPanel.IsBatch()) {
    std::vector<MediaItem*> items;
    for (const auto& seg : m_waveform.GetSegments())
      if (seg.item) items.push_back(seg.item);
    if (!items.empty()) m_gainPanel.ShowBatch(items);
  } else {
    m_gainPanel.Show(m_waveform.GetItem());
  }
  m_waveform.SetBatchGainOffset(1.0);
  m_waveform.UpdateFadeCache();

  // Re-analyze dynamics with updated audio levels
  if ((m_dynamicsVisible || m_dynamicsPanel.IsVisible()) && m_waveform.GetAudioSampleCount() > 0) {
    double ivDb = 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
    m_dynamics.Analyze(m_waveform.GetAudioData().data(),
                       m_waveform.GetAudioSampleCount(),
                       m_waveform.GetNumChannels(),
                       m_waveform.GetSampleRate(),
                       ivDb, m_dynamics.GetParams());
  }

  m_waveform.Invalidate();
  if (savedSel.active) m_waveform.SetSelection(savedSel);
}
