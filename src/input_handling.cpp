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
  // Settings panel: swallow double-clicks over it (a fast second click on the
  // slider/buttons must never reach the waveform below, e.g. as an envelope edit).
  if (m_settingsPanel.IsVisible() && m_settingsPanel.HitTest(x, y, m_waveformRect))
    return;

  // Double-click on gain panel = reset to 0 dB
  if (m_gainPanel.OnDoubleClick(x, y, m_waveformRect)) {
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

#ifdef SNEAKPEAK_BLEND2D_PANEL
  // Double-click on the premium dynamics panel = open the inline type-value editor on
  // the knob/curve-handle under the cursor. Consume it regardless of whether a control
  // was hit, so it never falls through to the waveform's select-all behaviour below.
  // (Premium only - the GDI panel keeps its prior behaviour for an exact OFF-build match.)
  if (m_dynamicsPanel.IsVisible() && m_dynamicsPanel.HitTest(x, y, m_waveformRect)) {
    if (m_dynamicsPanel.OnDoubleClick(x, y, m_waveformRect))
      InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  // Limiter panel: same consume-always contract as the dynamics panel above.
  if (m_limiterPanel.IsVisible() && m_limiterPanel.HitTest(x, y, m_waveformRect)) {
    if (m_limiterPanel.OnDoubleClick(x, y, m_waveformRect))
      InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
#endif

  // Double-click on waveform area
  if (y >= m_waveformRect.top && y < m_waveformRect.bottom) {
    // Double-click on envelope point = delete it
    if (m_waveform.GetShowVolumeEnvelope() && !m_waveform.IsStandaloneMode() &&
        g_GetTakeEnvelopeByName && g_DeleteEnvelopePointEx && g_Envelope_SortPoints) {
      int hitIdx = m_waveform.HitTestEnvelopePoint(x, y, SPmin(8));
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
    int markerIdx = m_markers.HitTestMarker(x, m_waveform, SPmin(5));
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
    int markerIdx = m_markers.HitTestMarker(x, m_waveform, SPmin(5));
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

  // A pending wheel-nudge fade undo block must not swallow this click's own edits.
  FlushFadeWheelUndo();

  // Stop standalone preview on click (allows repositioning cursor)
  if (m_previewActive) StandaloneCleanupPreview();

  // An open inline dynamics value-editor commits on any click anywhere in the window;
  // the click is then swallowed (it just dismisses the editor). Inert unless editing,
  // so it's safe in the GDI build too (IsEditingValue() is always false there).
  if (m_dynamicsPanel.IsEditingValue()) {
    if (m_dynamicsPanel.CommitValueEdit())
      ReanalyzeDynamicsAfterEdit();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  // Same commit-on-any-click contract for the limiter panel's inline editor.
  if (m_limiterPanel.IsEditingValue()) {
    if (m_limiterPanel.CommitValueEdit()) {
      m_limiterPanel.ClearParamsChanged();
      MarkLimiterParamsChanged();
      SaveLimiterParams();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Settings panel (premium): the topmost overlay. A click inside is handled by the
  // panel; a click anywhere else closes it AND is swallowed (one click = dismiss, so
  // closing can never accidentally edit the waveform underneath). Inert in the GDI
  // build (the panel can never become visible there).
  if (m_settingsPanel.IsVisible()) {
    if (m_settingsPanel.OnMouseDown(x, y, m_waveformRect)) {
      if (m_settingsPanel.IsDragging())
        SetCapture(m_hwnd);
      if (m_settingsPanel.ScaleChangedByClick()) {   // track jump / density preset
        ApplyUiScale(g_uiScale);                     // clamp + font flag + relayout
        SaveUiScale();
        MarkUiScaleUserSet();
      }
      if (m_settingsPanel.FitRequested()) {
        ApplyUiScale(ComputeFitUiScale());
        SaveUiScale();
        MarkUiScaleUserSet();
      }
      // Migrated preference clicks route through the SAME CM_* handlers as the
      // (OFF-build) menu items - one behavior path, no drift.
      const int pref = m_settingsPanel.PrefClicked();
      if (pref != SET_HIT_NONE) {
        int cmd = 0;
        switch (pref) {
          case SET_HIT_RULER0:       cmd = CM_RULER_RELATIVE;       break;
          case SET_HIT_RULER1:       cmd = CM_RULER_ABSOLUTE;       break;
          case SET_HIT_RULER2:       cmd = CM_RULER_BARS_BEATS;     break;
          case SET_HIT_MASTER:       cmd = CM_METER_SOURCE_MASTER;  break;
          case SET_HIT_METER0:       cmd = CM_METER_PEAK;           break;
          case SET_HIT_METER1:       cmd = CM_METER_RMS;            break;
          case SET_HIT_METER2:       cmd = CM_METER_VU;             break;
          case SET_HIT_VIEW_METERS:  cmd = CM_SHOW_METERS;          break;
          case SET_HIT_VIEW_RULER:   cmd = CM_SHOW_RULER;           break;
          case SET_HIT_VIEW_SNAP:    cmd = CM_SNAP_ZERO;            break;
          case SET_HIT_VIEW_MINIMAP: cmd = CM_MINIMAP;              break;
          // Zoom-center is a 2-way selector: only toggle when the clicked side differs.
          case SET_HIT_VIEW_ZOOM0:   if (m_zoomOnEditCursor)  cmd = CM_ZOOM_CENTER; break;
          case SET_HIT_VIEW_ZOOM1:   if (!m_zoomOnEditCursor) cmd = CM_ZOOM_CENTER; break;
          // Waveform style (#83) is the RMS-band state worn as a 2-way selector:
          // Detailed = RMS band on, Simple = single-colour peaks (CM_SHOW_RMS toggles).
          case SET_HIT_VIEW_WAVE0:   if (!m_waveform.GetShowRMS()) cmd = CM_SHOW_RMS; break;
          case SET_HIT_VIEW_WAVE1:   if (m_waveform.GetShowRMS())  cmd = CM_SHOW_RMS; break;
          // Spectral scale (#88): Hz vs note names, same 2-way pattern.
          case SET_HIT_VIEW_SPEC0:   if (m_spectral.GetNoteScale())  cmd = CM_SPECTRAL_NOTES; break;
          case SET_HIT_VIEW_SPEC1:   if (!m_spectral.GetNoteScale()) cmd = CM_SPECTRAL_NOTES; break;
        }
        if (cmd) OnContextMenuCommand(cmd);
      }
    } else {
      m_settingsPanel.Hide();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (y >= m_toolbarRect.top && y < m_toolbarRect.bottom) {
    int btn = m_toolbar.HitTest(x, y);
    if (btn >= 0) OnToolbarClick(btn);
    return;
  }

  // Mode bar click
  if (y >= m_modeBarRect.top && y < m_modeBarRect.bottom) {
#ifdef SNEAKPEAK_BLEND2D_PANEL
    // Settings gear: open the premium Settings panel. (When the panel is already
    // open, the swallow-outside-click block above closed it before reaching here.)
    if (x >= m_gearRect.left && x < m_gearRect.right &&
        y >= m_gearRect.top && y < m_gearRect.bottom) {
      m_settingsPanel.Show();
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
#endif
    // Click on version label: check for updates
    if (x >= m_versionRect.left && x < m_versionRect.right &&
        y >= m_versionRect.top && y < m_versionRect.bottom) {
      DoCheckForUpdate();
      return;
    }
    // Click on Support link: show support menu
    if (x >= m_supportRect.left && x < m_supportRect.right &&
        y >= m_supportRect.top && y < m_supportRect.bottom) {
      HMENU supportMenu = CreatePopupMenu();
      auto addSupportItem = [](HMENU m, unsigned int id, const char* str) {
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
      addSupportItem(supportMenu, CM_SUPPORT_KOFI, "\xe2\x98\x95 Ko-fi");
      addSupportItem(supportMenu, CM_SUPPORT_BMAC, "\xf0\x9f\x8d\xba Buy Me a Coffee");
      addSupportItem(supportMenu, CM_SUPPORT_PAYPAL, "\xf0\x9f\x92\xb3 PayPal");
      addSupportItem(supportMenu, CM_SUPPORT_GITHUB, "\xe2\xad\x90 GitHub");
      POINT pt = { m_supportRect.left, m_modeBarRect.bottom };
      ClientToScreen(m_hwnd, &pt);
      TrackPopupMenu(supportMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
      DestroyMenu(supportMenu);
      return;
    }
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
      SWELL_Menu_AddMenuItem(modeMenu, NULL, 0, 0);   // NULL name -> real separator line (not a blank gap)
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
      int markerIdx = m_markers.HitTestMarker(x, m_waveform, SPmin(5));
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
  if (m_minimapVisible && y >= m_minimapRect.top - SP(3) && y < m_minimapRect.top + SP(3)) {
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

  // Hard Limiter panel interaction (v2.4.0 INC-L1)
  if (m_limiterPanel.IsVisible() && m_limiterPanel.HitTest(x, y, m_waveformRect)) {
    if (m_limiterPanel.OnMouseDown(x, y, m_waveformRect)) {
      if (m_limiterPanel.IsDragging())
        SetCapture(m_hwnd);
      if (m_limiterPanel.ApplyRequested())
        DoApplyLimiter();
      if (m_limiterPanel.PresetMenuRequested())
        ShowLimiterPresetMenu();
      if (m_limiterPanel.ParamsChanged()) {   // pill toggle / Cmd-reset
        m_limiterPanel.ClearParamsChanged();
        MarkLimiterParamsChanged();
        SaveLimiterParams();
      }
      if (!m_limiterPanel.IsVisible())        // closed via X: keep session defaults
        SaveLimiterParams();
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
      // Persist Dyn/Env/GR overlay toggles as global user prefs when one changed
      if (m_dynamicsPanel.ViewPrefsChanged()) {
        m_dynamicsPanel.ClearViewPrefsChanged();
        SaveDynamicsViewPrefs();
      }
      // One-shot toast when a G.Thr change auto-expanded the plot floor (the gate
      // node would otherwise sit invisibly pinned at the plot edge).
      if (m_dynamicsPanel.FloorAutoSwitched()) {
        m_dynamicsPanel.ClearFloorAutoSwitched();
        ShowToast("Plot scale expanded to show the gate threshold");
      }
      // One-shot hint when Up mode is engaged with the gate off (the boost is
      // then capped but the noise floor still rises; we never auto-enable).
      if (m_dynamicsPanel.UpHintPending()) {
        m_dynamicsPanel.ClearUpHintPending();
        ShowToast("Tip: enable the Gate to keep the noise floor quiet");
      }
      // Preset dropdown menu (factory + user presets, Save/Delete) - built in
      // context_menu.cpp where the portable menu helpers live.
      if (m_dynamicsPanel.PresetMenuRequested()) {
        m_dynamicsPanel.ClearPresetMenuRequested();
        ShowDynamicsPresetMenu();
      }
      // Restore envelope on panel close if bypass was active
      bool isBypassed = m_dynamicsPanel.GetBypassed();
      if (wasBypassed && !m_dynamicsPanel.IsVisible())
        isBypassed = false; // panel closed, restore envelope
      // A/B bypass: toggle envelope ACTIVE state on all segments' envelopes
      if (isBypassed != wasBypassed)
        ApplyEnvelopeBypass(isBypassed);
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
      int chTop, chH;
      SpectralChannelAt(y, chTop, chH);

      // Alt+click = frequency band selection
      bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
      if (altDown) {
        double freq = m_spectral.YToFreq(y, chTop, chH);
        m_spectral.StartFreqSelection(freq);
        m_spectral.SetMarqueeGesture(false); // band-only: full-width lines wanted
        m_spectralFreqDragging = true;
        m_spectralFreqDragChTop = chTop;
        m_spectralFreqDragChH = chH;
        SetCapture(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
      }

      // Marquee edge grips: grab an edge/corner of the rectangle to fine-tune
      // it (the cursor already shows the resize arrows). Each axis re-anchors
      // on its opposite edge and rides the existing marquee drag flags
      // (time = m_dragging, freq = m_spectralFreqDragging), so move/up/sync
      // behave exactly like a fresh marquee drag.
      if (!(wParam & MK_SHIFT)) {
        const int grip = HitMarqueeEdge(x, y, chTop, chH);
        if (grip) {
          if (grip & (GRIP_T_START | GRIP_T_END)) {
            WaveformSelection sel = m_waveform.GetSelection();
            const double s = std::min(sel.startTime, sel.endTime);
            const double e = std::max(sel.startTime, sel.endTime);
            m_waveform.StartSelection((grip & GRIP_T_START) ? e : s);
            m_waveform.UpdateSelection(m_waveform.XToTime(x));
            m_dragging = true;
          }
          if (grip & (GRIP_F_LOW | GRIP_F_HIGH)) {
            const double anchor = (grip & GRIP_F_LOW) ? m_spectral.GetFreqSelHigh()
                                                      : m_spectral.GetFreqSelLow();
            m_spectral.StartFreqSelection(anchor);
            m_spectral.UpdateFreqSelection(m_spectral.YToFreq(y, chTop, chH));
            m_spectralFreqDragging = true;
            m_spectralFreqDragChTop = chTop;
            m_spectralFreqDragChH = chH;
          }
          SetCapture(m_hwnd);
          InvalidateRect(m_hwnd, nullptr, FALSE);
          return;
        }
      }

      // Normal click+drag = marquee: time selection AND frequency band in one
      // gesture (a time x frequency rectangle, the spectral-editor standard).
      // A plain click (no vertical drag) collapses the band on mouse-up and
      // behaves like the old cursor click. Shift extends the time axis only.
      double time = m_waveform.XToTime(x);
      if (wParam & MK_SHIFT) {
        m_waveform.UpdateSelection(time);
      } else {
        m_waveform.StartSelection(time);
        m_waveform.SetCursorTime(time);
        if (g_SetEditCurPos)
          g_SetEditCurPos(m_waveform.RelTimeToAbsTime(time), true, false);
        m_spectral.StartFreqSelection(m_spectral.YToFreq(y, chTop, chH));
        m_spectral.SetMarqueeGesture(true); // no band lines until the rect exists
        m_spectralFreqDragging = true;
        m_spectralFreqDragChTop = chTop;
        m_spectralFreqDragChH = chH;
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

      // Channel solo - visual dimming + audio via take pan BALANCE, not
      // I_CHANMODE: REAPER's mono-left/right modes fold the take to CENTRED
      // mono (both ears) and collapse our display to one channel on reload,
      // stranding the badges. Full-left/right pan under REAPER's default
      // balance law mutes the opposite channel while KEEPING the soloed
      // channel on its own side (Audition-style monitoring). The user's pan
      // is saved on first solo and restored on un-solo / item switch.
      if (m_waveform.ClickChannelButton(x, y)) {
        if (m_waveform.GetTake() && g_GetSetMediaItemTakeInfo) {
          MediaItem_Take* take = m_waveform.GetTake();
          const bool L = m_waveform.IsChannelActive(0);
          const bool R = m_waveform.IsChannelActive(1);
          if (L && R) {
            // Un-solo: restore the pre-solo pan (if we set one on this take)
            if (m_chanSoloTake == take) {
              g_GetSetMediaItemTakeInfo(take, "D_PAN", &m_chanSoloPrevPan);
              m_chanSoloTake = nullptr;
            }
          } else {
            if (m_chanSoloTake != take) {        // entering solo: save the user's pan
              double* p = (double*)g_GetSetMediaItemTakeInfo(take, "D_PAN", nullptr);
              m_chanSoloPrevPan = p ? *p : 0.0;
              m_chanSoloTake = take;
            }
            double pan = L ? -1.0 : 1.0;         // balance: the opposite side goes silent
            g_GetSetMediaItemTakeInfo(take, "D_PAN", &pan);
          }
          // Make the pan change audible promptly during playback: REAPER's media
          // buffering would otherwise drain the already-buffered audio first (up
          // to the media buffer length of lag). UpdateItemInProject is NOT usable
          // here - it counts as an item edit and trips REAPER's "stop playback
          // when editing media" preference. Instead re-seek the transport to the
          // current play position (flushes the buffers, playback continues) and
          // put the edit cursor back where the user had it.
          if (g_GetPlayState && (g_GetPlayState() & 1) &&
              g_GetPlayPosition && g_GetCursorPosition && g_SetEditCurPos) {
            const double editPos = g_GetCursorPosition();
            g_SetEditCurPos(g_GetPlayPosition(), false, true);   // seek = buffer flush
            g_SetEditCurPos(editPos, false, false);              // restore edit cursor
          }
          if (g_UpdateArrange) g_UpdateArrange();
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
      }

      // Standalone fade handles — always visible at top corners (16px hit zone)
      if (m_waveform.IsStandaloneMode()) {
        int waveL = m_waveformRect.left;
        int waveR = m_waveformRect.right - SP(DB_SCALE_WIDTH);
        auto sf = m_waveform.GetStandaloneFade();
        int fiX = (sf.fadeInLen >= 0.001) ? m_waveform.TimeToX(sf.fadeInLen) : waveL;
        int foX = (sf.fadeOutLen >= 0.001) ? m_waveform.TimeToX(m_waveform.GetItemDuration() - sf.fadeOutLen) : waveR;
        if (abs(x - fiX) <= SP(FADE_HANDLE_HIT_ZONE) && y < m_waveformRect.top + SP(FADE_HANDLE_TOP_ZONE)) {
          m_fadeDragging = FADE_IN;
          m_standaloneFadeDrag = true;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = sf.fadeInDir;
          m_fadeDragFine = false;   // Shift fine-drag re-anchors on first fine move
          SetCapture(m_hwnd);
          return;
        }
        if (abs(x - foX) <= SP(FADE_HANDLE_HIT_ZONE) && y < m_waveformRect.top + SP(FADE_HANDLE_TOP_ZONE)) {
          m_fadeDragging = FADE_OUT;
          m_standaloneFadeDrag = true;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = sf.fadeOutDir;
          m_fadeDragFine = false;
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

          int hitIdx = m_waveform.HitTestEnvelopePoint(x, y, SPmin(8));
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
            // Grab offset: point's on-screen Y minus cursor Y. The move handler
            // maps the anchor point ABSOLUTELY from cursor Y + this offset, so
            // it tracks the cursor 1:1 (no fader-scale lag) without jumping.
            m_envDragGrabDy = 0;
            if (g_GetEnvelopePoint && g_GetEnvelopeScalingMode && g_ScaleFromEnvelopeMode) {
              double gt = 0, gv = 0, gtn = 0; int gs = 0; bool gsel = false;
              if (g_GetEnvelopePoint(env, hitIdx, &gt, &gv, &gs, &gtn, &gsel)) {
                int sm = g_GetEnvelopeScalingMode(env);
                m_envDragGrabDy =
                  m_waveform.EnvYToGainY(g_ScaleFromEnvelopeMode(sm, gv), sm) - y;
              }
            }
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
            if (abs(y - lineY) <= SP(20)) {
              // T2-1 (#51): Alt+drag ON the line = edit the segment's bezier
              // tension (REAPER's own curvature modifier). Precedence: within
              // this +-SP(20) line zone Alt belongs to the envelope - outside
              // it, Alt+click keeps its existing meanings (drag-export inside
              // a selection, snap-to-segment in timeline/SET/multi-item).
              bool altCurve = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
              if (altCurve && g_GetEnvelopePointByTime && g_GetEnvelopePoint &&
                  g_SetEnvelopePoint && g_CountEnvelopePoints) {
                int owner = g_GetEnvelopePointByTime(env, ei.envTime);
                int cnt2 = g_CountEnvelopePoints(env);
                if (owner >= 0 && owner < cnt2 - 1) { // segment needs a right neighbor
                  double pt = 0, pv = 0, ptn = 0; int psh = 0; bool psl = false;
                  g_GetEnvelopePoint(env, owner, &pt, &pv, &psh, &ptn, &psl);
                  double nt = 0, nv = 0, ntn = 0; int nsh = 0; bool nsl = false;
                  g_GetEnvelopePoint(env, owner + 1, &nt, &nv, &nsh, &ntn, &nsl);
                  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
                  if (psh != 5) { // promote to bezier (keep tension) - REAPER behavior
                    int shape5 = 5; bool noSortP = true;
                    g_SetEnvelopePoint(env, owner, nullptr, nullptr, &shape5,
                                       nullptr, nullptr, &noSortP);
                  }
                  m_envTensionDragging = true;
                  m_envTensionPtIdx = owner;
                  m_envTensionStart = ptn;
                  m_envTensionCur = ptn;
                  m_envTensionStartY = y;
                  m_envTensionDir = (pv >= nv) ? 1 : -1; // drag up bulges the curve up
                  m_envDragEnv = env; // stale-pointer safety: cleared in LoadSelectedItem
                  SetCapture(m_hwnd);
                  InvalidateRect(m_hwnd, nullptr, FALSE);
                  return;
                }
              }
              // Use evaluated envelope value (not pixel-derived) to avoid precision loss
              double newRawVal = rawVal;
              bool cmdDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
              if (cmdDown) {
                // Cmd+click+drag on line = freehand envelope drawing
                if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
                bool noSort = false;
                g_InsertEnvelopePointEx(env, -1, ei.envTime, newRawVal, 0, 0.0, false, &noSort);
                g_Envelope_SortPoints(env);  // unsorted envelope yields invalid Envelope_Evaluate values until mouseUp
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
                  // New point inserted unselected; deselect others and select only it (drag loop filters by psel==true).
                  // Also compute drag clamp bounds from non-selected neighbors (hit-branch pattern, lines 519-537).
                  m_envDragMinTime = 0.0;
                  m_envDragMaxTime = segDuration;
                  if (g_SetEnvelopePoint && g_CountEnvelopePoints && g_GetEnvelopePoint) {
                    int ptCount = g_CountEnvelopePoints(env);
                    bool noSortSel = true;
                    for (int j = 0; j < ptCount; j++) {
                      bool sel = (j == newIdx);
                      g_SetEnvelopePoint(env, j, nullptr, nullptr, nullptr, nullptr, &sel, &noSortSel);
                      if (!sel) {
                        double nt = 0, nv = 0, ntn = 0; int ns = 0; bool npsel = false;
                        g_GetEnvelopePoint(env, j, &nt, &nv, &ns, &ntn, &npsel);
                        if (nt < ei.envTime && nt > m_envDragMinTime) m_envDragMinTime = nt + 0.0001;
                        if (nt > ei.envTime && nt < m_envDragMaxTime) m_envDragMaxTime = nt - 0.0001;
                      }
                    }
                  }
                  m_envDragging = true;
                  m_envDragPointIdx = newIdx;
                  m_envDragEnv = env;
                  m_envDragSegOffset = segOffset;
                  m_envDragSegDuration = segDuration;
                  m_envDragGrabDy = lineY - y; // new point sits on the line
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
        if (abs(x - fiX) <= SP(FADE_HANDLE_HIT_ZONE) && y < m_waveformRect.top + SP(FADE_HANDLE_TOP_ZONE)) {
          m_fadeDragging = FADE_IN;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = fadeInItem ? g_GetMediaItemInfo_Value(fadeInItem, "D_FADEINDIR") : 0.0;
          m_fadeDragFine = false;   // Shift fine-drag re-anchors on first fine move
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          return;
        }
        if (abs(x - foX) <= SP(FADE_HANDLE_HIT_ZONE) && y < m_waveformRect.top + SP(FADE_HANDLE_TOP_ZONE)) {
          m_fadeDragging = FADE_OUT;
          m_fadeDragStartY = y;
          m_fadeDragStartDir = fadeOutItem ? g_GetMediaItemInfo_Value(fadeOutItem, "D_FADEOUTDIR") : 0.0;
          m_fadeDragFine = false;
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

      // #64: grab a selection edge to resize it (the cursor already shows <->
      // over the zone). Checked BEFORE drag-export so the edge zone wins over
      // click-inside-selection. Re-anchors the selection on the OPPOSITE edge
      // and rides the existing m_dragging path - move/up/snap/sync unchanged.
      if (!(wParam & MK_SHIFT)) {
        const int edge = HitSelectionEdge(x, y);
        if (edge) {
          WaveformSelection sel = m_waveform.GetSelection();
          const double s = std::min(sel.startTime, sel.endTime);
          const double e = std::max(sel.startTime, sel.endTime);
          m_waveform.StartSelection(edge == 1 ? e : s);   // anchor = opposite edge
          m_waveform.UpdateSelection(time);
          m_dragging = true;
          SetCapture(m_hwnd);
          InvalidateRect(m_hwnd, nullptr, FALSE);
          return;
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

      // Slip content (forum #51): Alt+drag OUTSIDE the selection in plain ITEM
      // mode slides the take's source under the item (D_STARTOFFS), REAPER's
      // "move contents" edit. v1 scope: single item, non-looped source; the
      // arrange updates live, the in-window waveform reloads once at release.
      // Alt collisions all handled above and win by order: envelope-line zone =
      // curvature, inside selection = immediate drag export, SET/timeline/multi
      // = segment snap. A looped source falls through to normal selection.
      if (altHeld && !(wParam & MK_SHIFT) &&
          !m_waveform.IsStandaloneMode() && !m_waveform.IsMultiItem() &&
          !m_masterMode && m_waveform.GetItem() && m_waveform.GetTake() &&
          g_GetSetMediaItemTakeInfo && g_GetMediaItemInfo_Value &&
          g_GetMediaItemTake_Source) {
        MediaItem* item = m_waveform.GetItem();
        MediaItem_Take* take = m_waveform.GetTake();
        bool looped = g_GetMediaItemInfo_Value(item, "B_LOOPSRC") > 0.5;
        PCM_source* src = g_GetMediaItemTake_Source(take);
        double srcLen = src ? src->GetLength() : 0.0;
        double* pOff = (double*)g_GetSetMediaItemTakeInfo(take, "D_STARTOFFS", nullptr);
        if (!looped && srcLen > 0.0 && pOff) {
          double* pRate = (double*)g_GetSetMediaItemTakeInfo(take, "D_PLAYRATE", nullptr);
          double itemLen = g_GetMediaItemInfo_Value(item, "D_LENGTH");
          m_slipDragging = true;
          m_slipStartX = x;
          m_slipStartOffs = *pOff;
          m_slipPlayrate = (pRate && *pRate > 0.0) ? *pRate : 1.0;
          m_slipMaxOffs = std::max(0.0, srcLen - itemLen * m_slipPlayrate);
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
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

// #61: middle-mouse pan start - grabs the view for a horizontal scroll drag in
// the waveform or spectral area (the move/up handling lives in OnMouseMove /
// WM_MBUTTONUP).
void SneakPeak::OnMiddleDown(int x, int y)
{
  if (!m_waveform.HasItem()) return;
  const bool inWave = y >= m_waveformRect.top && y < m_waveformRect.bottom;
  const bool inSpec = m_spectralVisible &&
                      y >= m_spectralRect.top && y < m_spectralRect.bottom;
  if (!inWave && !inSpec) return;
  m_mmbPanning = true;
  m_mmbLastX = x;
  SetCapture(m_hwnd);
  SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
}

// Which spectral channel band (top pixel + height) is under y. Mirrors the
// layout math used across the spectral view (stereo = two stacked bands).
void SneakPeak::SpectralChannelAt(int y, int& chTop, int& chH)
{
  int specH = m_spectralRect.bottom - m_spectralRect.top;
  int nch = m_waveform.GetNumChannels();
  int chSep = (nch > 1) ? SP(CHANNEL_SEPARATOR_HEIGHT) : 0;
  chH = (nch > 1) ? (specH - chSep) / 2 : specH;
  chTop = m_spectralRect.top;
  if (nch > 1 && y >= m_spectralRect.top + chH + chSep)
    chTop = m_spectralRect.top + chH + chSep;
}

// Marquee edge grip under the cursor (0 = none; corners set two bits). Edge
// zone = +/-SPmin(4) px like the waveform edge resize (#64); the nearer edge
// wins when both are in reach, so tiny rectangles stay adjustable.
int SneakPeak::HitMarqueeEdge(int x, int y, int chTop, int chH)
{
  if (!m_waveform.HasSelection() || !m_spectral.HasFreqSelection()) return 0;
  WaveformSelection sel = m_waveform.GetSelection();
  const double s = std::min(sel.startTime, sel.endTime);
  const double e = std::max(sel.startTime, sel.endTime);
  if (e <= s) return 0;
  const int sx = m_waveform.TimeToX(s);
  const int ex = m_waveform.TimeToX(e);
  const int yHi = m_spectral.FreqToY(m_spectral.GetFreqSelHigh(), chTop, chH); // top
  const int yLo = m_spectral.FreqToY(m_spectral.GetFreqSelLow(), chTop, chH);  // bottom
  const int zone = SPmin(4);
  int grip = 0;
  if (y >= yHi - zone && y <= yLo + zone) {
    const int ds = abs(x - sx), de = abs(x - ex);
    if (ds <= zone && de <= zone) grip |= (ds <= de) ? GRIP_T_START : GRIP_T_END;
    else if (ds <= zone) grip |= GRIP_T_START;
    else if (de <= zone) grip |= GRIP_T_END;
  }
  if (x >= sx - zone && x <= ex + zone) {
    const int dh = abs(y - yHi), dl = abs(y - yLo);
    if (dh <= zone && dl <= zone) grip |= (dh <= dl) ? GRIP_F_HIGH : GRIP_F_LOW;
    else if (dh <= zone) grip |= GRIP_F_HIGH;
    else if (dl <= zone) grip |= GRIP_F_LOW;
  }
  return grip;
}

// #64: which selection edge is under the cursor in the waveform area (0 = none,
// 1 = start edge, 2 = end edge). Edge zone = +/-SPmin(4) px; on a tiny on-screen
// selection the nearer edge wins, so both edges stay grabbable.
int SneakPeak::HitSelectionEdge(int x, int y)
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return 0;
  if (y < m_waveformRect.top || y >= m_waveformRect.bottom) return 0;
  if (x >= m_waveformRect.right - SP(DB_SCALE_WIDTH)) return 0;   // dB scale column
  WaveformSelection sel = m_waveform.GetSelection();
  const double s = std::min(sel.startTime, sel.endTime);
  const double e = std::max(sel.startTime, sel.endTime);
  if (e <= s) return 0;
  const int sx = m_waveform.TimeToX(s);
  const int ex = m_waveform.TimeToX(e);
  const int zone = SPmin(4);
  const int ds = abs(x - sx), de = abs(x - ex);
  if (ds <= zone && de <= zone) return (ds <= de) ? 1 : 2;
  if (ds <= zone) return 1;
  if (de <= zone) return 2;
  return 0;
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
    m_spectral.SetMarqueeGesture(false); // gesture over: normal overlay rules
    // A near-click band (a few px tall) collapses back to "no band", so a
    // plain click on the spectrogram keeps its cursor-click behavior.
    int yLo = m_spectral.FreqToY(m_spectral.GetFreqSelLow(),
                                 m_spectralFreqDragChTop, m_spectralFreqDragChH);
    int yHi = m_spectral.FreqToY(m_spectral.GetFreqSelHigh(),
                                 m_spectralFreqDragChTop, m_spectralFreqDragChH);
    if (std::abs(yLo - yHi) < 3) m_spectral.ClearFreqSelection();
    if (!m_dragging) { // Alt+drag: band only
      ReleaseCapture();
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
    // Marquee: the m_dragging branch below finishes the time axis.
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
  if (m_settingsPanel.IsDragging()) {
    if (m_settingsPanel.OnMouseUp()) {
      g_fontsNeedRescale = true;   // deferred from the live drag: one rebuild at the next paint
      SaveUiScale();
      MarkUiScaleUserSet();
    }
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_limiterPanel.IsDragging()) {
    m_limiterPanel.OnMouseUp();
    if (m_limiterPanel.GeomChanged()) {   // panel drag: persist offsets
      m_limiterPanel.ClearGeomChanged();
      SaveLimiterGeom();
    }
    SaveLimiterParams();                  // knob drags persist session defaults
    ReleaseCapture();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_dynamicsPanel.IsDragging()) {
    m_dynamicsPanel.OnMouseUp();
    // Persist panel size/position if a resize or panel-drag changed it.
    if (m_dynamicsPanel.GeomChanged()) {
      m_dynamicsPanel.ClearGeomChanged();
      SaveDynamicsGeom();
    }
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
  if (m_envTensionDragging) { // T2-1: end of a curvature drag (time untouched - no re-sort)
    m_envTensionDragging = false;
    ReleaseCapture();
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Edit envelope curvature", -1);
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
  if (m_slipDragging) {
    m_slipDragging = false;
    ReleaseCapture();
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Slip item contents", -1);
    // One reload at release: the in-window waveform now shows the new source
    // window (the arrange tracked the slip live during the drag). Same calls
    // as the external-change poll path.
    if (m_waveform.HasItem()) {
      m_waveform.ReloadAfterExternalChange();
      m_spectral.ClearSpectrum();
      m_spectral.Invalidate();
      m_minimap.Invalidate();
    }
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
            StandaloneUndoSaveRange(startFrame, selFrames); // bounded edit (STA-2)
            int fadeFrames = std::min(sr / 100, selFrames / 2); // ~10ms crossfade
            AudioOps::GainWithCrossfade(data.data() + (size_t)startFrame * nch, selFrames, nch, factor, fadeFrames);
          }
        } else {
          StandaloneUndoSave(); // whole-file edit: full snapshot
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
  // Settings-panel UI-scale slider drag: live structural preview. The whole GDI
  // layout re-flows at the new scale on every move; the FONT rebuild is deferred
  // to mouse-up (one rebuild at the next paint - no per-move HFONT churn).
  if (m_settingsPanel.IsDragging()) {
    if (m_settingsPanel.OnMouseMove(x, y, m_waveformRect)) {
      RECT cr;
      GetClientRect(m_hwnd, &cr);
      RecalcLayout(cr.right, cr.bottom);
      m_waveform.Invalidate();
      m_spectral.Invalidate();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    return;
  }

  // #61: middle-mouse horizontal pan (capture held; exclusive while active)
  if (m_mmbPanning) {
    if (x != m_mmbLastX) {
      // Time delta from the pixel delta at the CURRENT view (both X evaluated
      // before the scroll, so the grabbed audio stays under the cursor).
      const double dt = m_waveform.XToTime(m_mmbLastX) - m_waveform.XToTime(x);
      m_waveform.ScrollH(dt);
      m_mmbLastX = x;
      m_spectral.Invalidate();
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
    m_lastMouseX = x;
    m_lastMouseY = y;
    return;
  }

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
    newH = std::max(SPmin(MINIMAP_HEIGHT), std::min(SP(120), newH));
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
    // Content bounds from the live layout rects - RecalcLayout's own definition
    // (was duplicated here with unscaled constants, so the splitter jumped away
    // from the cursor at UI scale != 100%, with meters hidden, or with the
    // minimap or a hidden ruler in play).
    int contentTop = m_rulerRect.bottom;
    int contentBot = m_minimapRect.top;
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
  if (m_envTensionDragging && m_envDragEnv && g_SetEnvelopePoint &&
      g_CountEnvelopePoints && m_envTensionPtIdx < g_CountEnvelopePoints(m_envDragEnv)) {
    // T2-1: full tension throw over SP(150) px of vertical travel; Cmd = 0.2x
    // fine. Time never changes, so noSort=true is always valid here.
    double fine = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ? 0.2 : 1.0;
    double t = m_envTensionStart +
      (double)m_envTensionDir * ((double)(m_envTensionStartY - y) / (double)SP(150)) * fine;
    t = std::max(-1.0, std::min(1.0, t));
    m_envTensionCur = t;
    int shape5 = 5;
    bool noSort = true;
    g_SetEnvelopePoint(m_envDragEnv, m_envTensionPtIdx, nullptr, nullptr,
                       &shape5, &t, nullptr, &noSort);
    if (g_UpdateArrange) g_UpdateArrange();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (m_envDragging && m_envDragPointIdx >= 0 && m_waveform.HasItem()) {
    TrackEnvelope* env = m_envDragEnv;
    if (env && g_SetEnvelopePoint && g_GetEnvelopePoint && g_CountEnvelopePoints &&
        g_GetEnvelopeScalingMode && g_ScaleToEnvelopeMode && g_ScaleFromEnvelopeMode) {
      double timeDelta = m_waveform.XToTime(x) - m_waveform.XToTime(m_lastMouseX);
      int scalingMode = g_GetEnvelopeScalingMode(env);
      // ANCHORED ABSOLUTE drag (user bug: fader-scale is nonlinear in Y, so
      // per-move deltas taken at the CURSOR's Y made the point lag ever more
      // on the way down - the cursor hit the window edge while the point hung
      // mid-lane, forcing a re-grab). The dragged point now maps absolutely
      // from cursor Y + grab offset (tracks 1:1 at any scale position); the
      // other selected points move by the anchor's gain delta, so batch-move
      // semantics are unchanged.
      double gainDelta = 0.0;
      double anchorTargetGain = -1.0;
      {
        double at = 0, av = 0, atn = 0; int as = 0; bool asel = false;
        if (g_GetEnvelopePoint(env, m_envDragPointIdx, &at, &av, &as, &atn, &asel)) {
          double anchorGain = g_ScaleFromEnvelopeMode(scalingMode, av);
          anchorTargetGain =
            m_waveform.EnvPixelToGain(y + m_envDragGrabDy, scalingMode);
          gainDelta = anchorTargetGain - anchorGain;
        }
      }
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
        // The anchor gets its exact absolute target; companions get its delta.
        double newGain = (i == m_envDragPointIdx && anchorTargetGain >= 0.0)
                           ? anchorTargetGain
                           : std::max(0.0, curGain + gainDelta);
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

  if (m_slipDragging && m_waveform.HasItem()) {
    MediaItem* item = m_waveform.GetItem();
    MediaItem_Take* take = m_waveform.GetTake();
    if (item && take && g_GetSetMediaItemTakeInfo) {
      // Dragging content RIGHT reveals EARLIER source: the offset decreases.
      double dt = m_waveform.XToTime(x) - m_waveform.XToTime(m_slipStartX);
      double off = m_slipStartOffs - dt * m_slipPlayrate;
      off = std::max(0.0, std::min(off, m_slipMaxOffs));
      g_GetSetMediaItemTakeInfo(take, "D_STARTOFFS", &off);
      // Live arrange refresh - the v2.1.1 RefreshItemSource precedent: Linux
      // needs UpdateItemInProject for the arrange peaks to actually move.
      if (g_UpdateItemInProject) g_UpdateItemInProject(item);
      if (g_UpdateArrange) g_UpdateArrange();
      // Keep the external-change poll quiet during our own writes; the view
      // reloads once at mouse-up.
      m_timelineEditGuard = TIMELINE_EDIT_GUARD_TICKS;
      char msg[64];
      double shifted = -(off - m_slipStartOffs) / m_slipPlayrate;  // content shift as seen
      snprintf(msg, sizeof(msg), "Slip: %+.3f s", shifted);
      ShowToast(msg);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
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

    // Horizontal: absolute by default (the fade edge tracks the cursor); while
    // Shift is held the drag turns fine-relative at 1/4 speed (forum #51). The
    // anchor rebases whenever Shift is pressed/released mid-drag, so the edge
    // never jumps on a modifier change.
    bool fine = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    auto fadeLenTarget = [&](double curLen, double absLen) {
      if (fine != m_fadeDragFine) {
        m_fadeDragFine = fine;
        m_fadeDragAnchorX = x;
        m_fadeDragAnchorLen = curLen;
      }
      if (!fine) return absLen;
      double dt = m_waveform.XToTime(x) - m_waveform.XToTime(m_fadeDragAnchorX);
      return m_fadeDragAnchorLen + 0.25 * ((m_fadeDragging == FADE_IN) ? dt : -dt);
    };

    if (m_standaloneFadeDrag) {
      auto sf = m_waveform.GetStandaloneFade();
      if (m_fadeDragging == FADE_IN) {
        sf.fadeInLen = std::max(0.0, std::min(fadeLenTarget(sf.fadeInLen, time),
                                              dur - sf.fadeOutLen));
        sf.fadeInDir = newDir;
      } else {
        sf.fadeOutLen = std::max(0.0, fadeLenTarget(sf.fadeOutLen, dur - time));
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
        double curLen = g_GetMediaItemInfo_Value(item, "D_FADEINLEN");
        double fadeLen = std::max(0.0, std::min(fadeLenTarget(curLen, time),
                                                maxLen - fadeOutLen));
        g_SetMediaItemInfo_Value(item, "D_FADEINLEN", fadeLen);
        g_SetMediaItemInfo_Value(item, "D_FADEINDIR", newDir);
        m_waveform.SetFadeDragInfo(1, (int)g_GetMediaItemInfo_Value(item, "C_FADEINSHAPE"));
      } else {
        MediaItem* item = (multi && !segs.empty()) ? segs.back().item : m_waveform.GetItem();
        double maxLen = (multi && !segs.empty()) ? segs.back().duration : dur;
        double fadeInLen = g_GetMediaItemInfo_Value(item, "D_FADEINLEN");
        double curLen = g_GetMediaItemInfo_Value(item, "D_FADEOUTLEN");
        double fadeLen = std::max(0.0, fadeLenTarget(curLen, dur - time));
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
    if (!m_dragging) { // Alt+drag: frequency band only
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
    // Marquee: fall through so the time axis (m_dragging below) updates too.
  }

  if (m_gainPanel.IsDragging()) {
    m_gainPanel.OnMouseMove(x, y, m_waveformRect);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }

  if (m_limiterPanel.IsDragging()) {
    m_limiterPanel.OnMouseMove(x, y, m_waveformRect);
    if (m_limiterPanel.ParamsChanged()) {   // knob drag: debounce the preview
      m_limiterPanel.ClearParamsChanged();
      MarkLimiterParamsChanged();
    }
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
    int waveRight = m_waveform.GetRect().right - SP(DB_SCALE_WIDTH);
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

  // Mode bar hover feedback: brighten the element under the cursor (tabs /
  // MASTER / gear / Support / version) and show a hand cursor. Uses the rects
  // cached by the last DrawModeBar; repaints only the mode bar on change.
  {
    int hov = MB_HOVER_NONE;
    if (y >= m_modeBarRect.top && y < m_modeBarRect.bottom &&
        !m_dragging && !m_mmbPanning && !m_scrollbarDragging) {
      auto inR = [&](const RECT& r) {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
      };
#ifdef SNEAKPEAK_BLEND2D_PANEL
      if (inR(m_gearRect)) hov = MB_HOVER_GEAR;
      else
#endif
      if (inR(m_supportRect)) hov = MB_HOVER_SUPPORT;
      else if (inR(m_versionRect)) hov = MB_HOVER_VERSION;
      else {
        for (int i = 0; i < (int)m_modeBarTabs.size(); ++i)
          if (inR(m_modeBarTabs[i].rect)) { hov = i; break; }
      }
    }
    if (hov != m_modeBarHover) {
      m_modeBarHover = hov;
      InvalidateRect(m_hwnd, &m_modeBarRect, FALSE);
    }
    if (hov != MB_HOVER_NONE) {
      SetCursor(LoadCursor(nullptr, IDC_HAND));
      m_lastMouseX = x;
      m_lastMouseY = y;
      return;   // cursor decided; nothing below applies inside the mode bar
    }
  }

  // Update mouse cursor based on what's under pointer
  {
    HCURSOR cur = LoadCursor(nullptr, IDC_ARROW);

    // Minimap resize edge
    if (m_minimapVisible && y >= m_minimapRect.top - SP(3) && y < m_minimapRect.top + SP(3)) {
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
               x >= m_waveformRect.right - SP(DB_SCALE_WIDTH)) {
        cur = LoadCursor(nullptr, IDC_HAND);
      }
      // Gain panel
      else if (m_gainPanel.IsVisible() && m_gainPanel.HitTest(x, y, m_waveformRect)) {
        cur = LoadCursor(nullptr, IDC_HAND);
      }
#ifdef SNEAKPEAK_BLEND2D_PANEL
      // Settings panel (topmost): hand over the body.
      else if (m_settingsPanel.IsVisible() && m_settingsPanel.HitTest(x, y, m_waveformRect)) {
        cur = LoadCursor(nullptr, IDC_HAND);
      }
      // Dynamics panel: hand over the body, diagonal-resize over the corner grip.
      else if (m_dynamicsPanel.IsVisible() && m_dynamicsPanel.HitTest(x, y, m_waveformRect)) {
        cur = m_dynamicsPanel.IsOverResizeGrip(x, y, m_waveformRect)
                ? LoadCursor(nullptr, IDC_SIZENWSE) : LoadCursor(nullptr, IDC_HAND);
      }
#endif
      // Fade handles (near item edges)
      else if (m_fadeDragging != FADE_NONE) {
        cur = LoadCursor(nullptr, IDC_SIZEWE);
      }
      // #64: selection edge resize affordance
      else if (HitSelectionEdge(x, y) != 0) {
        cur = LoadCursor(nullptr, IDC_SIZEWE);
      }
    }
    // Spectral area: marquee edge/corner grip affordance
    else if (m_spectralVisible && y >= m_spectralRect.top &&
             y < m_spectralRect.bottom && m_waveform.HasItem()) {
      int chTop, chH;
      SpectralChannelAt(y, chTop, chH);
      const int grip = HitMarqueeEdge(x, y, chTop, chH);
      const bool onTime = (grip & (GRIP_T_START | GRIP_T_END)) != 0;
      const bool onFreq = (grip & (GRIP_F_LOW | GRIP_F_HIGH)) != 0;
      if (onTime && onFreq) {
        // Corner orientation: left+top / right+bottom = NWSE, the others NESW
        const bool nwse = ((grip & GRIP_T_START) && (grip & GRIP_F_HIGH)) ||
                          ((grip & GRIP_T_END) && (grip & GRIP_F_LOW));
        cur = LoadCursor(nullptr, nwse ? IDC_SIZENWSE : IDC_SIZENESW);
      } else if (onTime) {
        cur = LoadCursor(nullptr, IDC_SIZEWE);
      } else if (onFreq) {
        cur = LoadCursor(nullptr, IDC_SIZENS);
      }
    }
    // Markers in ruler
    else if (y >= m_rulerRect.top && y < m_rulerRect.bottom && m_waveform.HasItem()) {
      if (m_markers.HitTestMarker(x, m_waveform, SPmin(5)) >= 0) {
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

#ifdef SNEAKPEAK_BLEND2D_PANEL
  // Hover-glow: repaint only when the hovered knob changes (cheap; no per-pixel
  // redraw). Drag paths already repaint, so this just covers free hover.
  if (m_dynamicsPanel.IsVisible() && m_dynamicsPanel.OnHover(x, y, m_waveformRect))
    InvalidateRect(m_hwnd, nullptr, FALSE);
  if (m_settingsPanel.IsVisible() && m_settingsPanel.OnHover(x, y, m_waveformRect))
    InvalidateRect(m_hwnd, nullptr, FALSE);
  if (m_limiterPanel.IsVisible() && m_limiterPanel.OnHover(x, y, m_waveformRect))
    InvalidateRect(m_hwnd, nullptr, FALSE);
#endif

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

#ifdef SNEAKPEAK_BLEND2D_PANEL
  // Settings panel: consume the wheel over it so the waveform underneath does not
  // zoom/pan while the user aims at the panel's controls.
  if (m_settingsPanel.IsVisible() && m_settingsPanel.HitTest(x, y, m_waveformRect))
    return;
  // Scroll over a limiter knob = nudge its value; consume the wheel anywhere
  // over the panel so the waveform underneath does not zoom/pan.
  if (m_limiterPanel.IsVisible() && m_limiterPanel.HitTest(x, y, m_waveformRect)) {
    if (m_limiterPanel.OnMouseWheel(x, y, steps, cmd, m_waveformRect) &&
        m_limiterPanel.ParamsChanged()) {
      m_limiterPanel.ClearParamsChanged();
      MarkLimiterParamsChanged();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  // Scroll over a dynamics knob = nudge its value (no need to grab tiny targets;
  // directly mitigates the DPI/scaling complaint). Consume the wheel whenever the
  // cursor is over the panel so the waveform underneath does not zoom/pan.
  if (m_dynamicsPanel.IsVisible() && m_dynamicsPanel.HitTest(x, y, m_waveformRect)) {
    if (m_dynamicsPanel.OnMouseWheel(x, y, steps, cmd, m_waveformRect) &&
        m_dynamicsPanel.ParamsChanged()) {
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
        if (m_dynamicsPanel.IsLive()) {
          // Single-shot undo for this wheel nudge. Mirror the drag path exactly:
          // mark LiveUndoOpen so ApplyDynamicsToEnvelope suppresses its OWN inner
          // undo block AND its per-apply toast (the audio_commands liveSession
          // gate); the wheel's Begin/End is then the single "Live Dynamics" block.
          // If a drag already holds a live block open (mid-drag wheel), don't nest
          // a second one - leave that block + its mouseup to own the lifecycle.
          bool alreadyOpen = m_dynamicsPanel.LiveUndoOpen();
          if (!alreadyOpen && g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          m_dynamicsPanel.SetLiveUndoOpen(true);
          ApplyDynamicsToEnvelope();
          if (!alreadyOpen) {
            m_dynamicsPanel.SetLiveUndoOpen(false);
            if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Live Dynamics", -1);
          }
        }
      }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
#endif

  // Scroll on gain knob = adjust gain (+/-0.5 dB per notch, Cmd = +/-0.1 dB fine)
  if (m_gainPanel.IsVisible() && m_gainPanel.HitTest(x, y, m_waveformRect)) {
    double dbStep = cmd ? 0.1 : 0.5;
    m_gainPanel.AdjustDb(steps * dbStep);
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // Wheel over a fade handle = nudge the fade length (forum #51): 5% of the item
  // per notch, Cmd = 1 ms fine - no aiming a drag at the tiny handle. Same hit
  // zones as the mouse-down grab; the ruler above is excluded (y >= waveform top).
  if (m_fadeDragging == FADE_NONE &&
      y >= m_waveformRect.top && y < m_waveformRect.top + SP(FADE_HANDLE_TOP_ZONE)) {
    double dur = m_waveform.GetItemDuration();
    double step = cmd ? steps * 0.001 : steps * 0.05 * dur;
    int waveL = m_waveformRect.left;
    int waveR = m_waveformRect.right - SP(DB_SCALE_WIDTH);
    if (m_waveform.IsStandaloneMode()) {
      auto sf = m_waveform.GetStandaloneFade();
      int fiX = (sf.fadeInLen >= 0.001) ? m_waveform.TimeToX(sf.fadeInLen) : waveL;
      int foX = (sf.fadeOutLen >= 0.001) ? m_waveform.TimeToX(dur - sf.fadeOutLen) : waveR;
      bool hitIn = abs(x - fiX) <= SP(FADE_HANDLE_HIT_ZONE);
      bool hitOut = !hitIn && abs(x - foX) <= SP(FADE_HANDLE_HIT_ZONE);
      if (hitIn || hitOut) {
        double& len = hitIn ? sf.fadeInLen : sf.fadeOutLen;
        double other = hitIn ? sf.fadeOutLen : sf.fadeInLen;
        double newLen = std::max(0.0, std::min(len + step, dur - other));
        if (newLen != len) {   // no-op nudge (clamped) must not dirty the file
          len = newLen;
          m_waveform.SetStandaloneFade(sf);
          m_dirty = true;
          UpdateTitle();
          m_waveform.Invalidate();
          InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
      }
    } else if (g_GetMediaItemInfo_Value && g_SetMediaItemInfo_Value) {
      auto& segs = m_waveform.GetSegments();
      bool multi = segs.size() > 1 || m_waveform.IsTrackView();
      MediaItem* fadeInItem = (multi && !segs.empty()) ? segs.front().item : m_waveform.GetItem();
      MediaItem* fadeOutItem = (multi && !segs.empty()) ? segs.back().item : m_waveform.GetItem();
      double fadeInLen = fadeInItem ? g_GetMediaItemInfo_Value(fadeInItem, "D_FADEINLEN") : 0.0;
      double fadeOutLen = fadeOutItem ? g_GetMediaItemInfo_Value(fadeOutItem, "D_FADEOUTLEN") : 0.0;
      int fiX = m_waveform.TimeToX(fadeInLen);
      int foX = m_waveform.TimeToX(dur - fadeOutLen);
      bool hitIn = fadeInItem && abs(x - fiX) <= SP(FADE_HANDLE_HIT_ZONE);
      bool hitOut = !hitIn && fadeOutItem && abs(x - foX) <= SP(FADE_HANDLE_HIT_ZONE);
      if (hitIn || hitOut) {
        MediaItem* item = hitIn ? fadeInItem : fadeOutItem;
        double maxLen = (multi && !segs.empty())
            ? (hitIn ? segs.front().duration : segs.back().duration) : dur;
        double cur = hitIn ? fadeInLen : fadeOutLen;
        double sameItemOther = g_GetMediaItemInfo_Value(
            item, hitIn ? "D_FADEOUTLEN" : "D_FADEINLEN");
        double len = std::max(0.0, std::min(cur + step, maxLen - sameItemOther));
        if (len != cur) {   // no-op nudge (clamped) must not spend an undo point
          // One undo block per nudge burst: opened on the first notch, closed by
          // OnTimer after ~600 ms idle (or flushed by the next mouse-down).
          if (!m_fadeWheelUndoOpen && g_Undo_BeginBlock2) {
            g_Undo_BeginBlock2(nullptr);
            m_fadeWheelUndoOpen = true;
          }
          m_fadeWheelLastTick = GetTickCount();
          g_SetMediaItemInfo_Value(item, hitIn ? "D_FADEINLEN" : "D_FADEOUTLEN", len);
          if (g_UpdateArrange) g_UpdateArrange();
          if (g_UpdateTimeline) g_UpdateTimeline();
          InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
      }
    }
  }

  // Scroll on dB scale column = vertical zoom
  int dbScaleLeft = m_waveformRect.right - SP(DB_SCALE_WIDTH);
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
    // #83: zoom anchor preference - mouse position (default) or the edit cursor.
    double centerTime = m_zoomOnEditCursor ? m_waveform.GetCursorTime()
                                           : m_waveform.XToTime(x);
    m_waveform.ZoomHorizontal(pow(ZOOM_FACTOR, steps), centerTime);
  }

  m_spectral.Invalidate();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Close a pending wheel-nudge fade undo block (idle timeout in OnTimer, any
// mouse-down, or window destroy - a Begin without End must never leak).
void SneakPeak::FlushFadeWheelUndo()
{
  if (!m_fadeWheelUndoOpen) return;
  m_fadeWheelUndoOpen = false;
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Adjust fade", -1);
}

// Re-run the dynamics analysis after an inline type-value commit, mirroring the
// wheel-nudge path exactly: re-analyse + refresh the GR meter, and in Live mode wrap
// the envelope write in a single-shot undo block (suppressing ApplyDynamicsToEnvelope's
// own inner block + per-apply toast). Clears the panel's ParamsChanged flag.
void SneakPeak::ReanalyzeDynamicsAfterEdit()
{
  m_dynamicsPanel.ClearParamsChanged();
  m_dynamics.SetParams(m_dynamicsPanel.GetParams());
  if (m_waveform.GetAudioSampleCount() <= 0) return;
  double ivDb = 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
  m_dynamics.Analyze(m_waveform.GetAudioData().data(),
                     m_waveform.GetAudioSampleCount(),
                     m_waveform.GetNumChannels(),
                     m_waveform.GetSampleRate(),
                     ivDb, m_dynamicsPanel.GetParams());
  m_dynamics.ComputeCompression();
  m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());
  if (m_dynamicsPanel.IsLive()) {
    bool alreadyOpen = m_dynamicsPanel.LiveUndoOpen();
    if (!alreadyOpen && g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
    m_dynamicsPanel.SetLiveUndoOpen(true);
    ApplyDynamicsToEnvelope();
    if (!alreadyOpen) {
      m_dynamicsPanel.SetLiveUndoOpen(false);
      if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Live Dynamics", -1);
    }
  }
}

// Route one key (from the SWS accelerator) to the open inline value editor. A commit
// that changes the value re-analyses like a knob change; otherwise we just repaint
// (the editor text changed, or it closed via ESC/empty commit).
// Route one key to the limiter panel's inline editor (SWS accelerator path).
// A commit that changes the value debounces a preview recompute like a knob.
void SneakPeak::HandleLimiterEditKey(WPARAM key)
{
  if (!m_limiterPanel.IsEditingValue()) return;
  m_limiterPanel.OnEditKey((int)key);
  if (m_limiterPanel.ParamsChanged()) {
    m_limiterPanel.ClearParamsChanged();
    MarkLimiterParamsChanged();
    SaveLimiterParams();
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SneakPeak::HandleDynamicsEditKey(WPARAM key)
{
  if (!m_dynamicsPanel.IsEditingValue()) return;
  m_dynamicsPanel.OnEditKey((int)key);
  if (m_dynamicsPanel.ParamsChanged())
    ReanalyzeDynamicsAfterEdit();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// A/B bypass: write the envelope ACTIVE state on all segments' envelopes. Shared
// by the panel mouse path (A/B click, close button) and CloseDynamicsPanel (ESC/D).
void SneakPeak::ApplyEnvelopeBypass(bool bypassed)
{
  m_waveform.SetEnvBypassed(bypassed);
  if (g_GetTakeEnvelopeByName && g_GetSetEnvelopeInfo_String) {
    char val[4];
    snprintf(val, sizeof(val), "%d", bypassed ? 0 : 1);
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

// Close the dynamics panel from a non-mouse path (ESC / the D hotkey, forum #77),
// mirroring the close-button side effects the OnMouseDown block performs: restore
// the A/B-bypassed envelope and end an open Live undo block.
void SneakPeak::CloseDynamicsPanel()
{
  if (!m_dynamicsPanel.IsVisible()) return;
  const bool wasBypassed = m_dynamicsPanel.GetBypassed();
  const bool wasLiveUndo = m_dynamicsPanel.LiveUndoOpen();
  if (m_dynamicsPanel.IsDragging()) {   // ESC mid-drag: don't leave capture dangling
    m_dynamicsPanel.OnMouseUp();
    ReleaseCapture();
  }
  m_dynamicsPanel.Hide();
  if (wasBypassed)
    ApplyEnvelopeBypass(false);
  if (wasLiveUndo) {
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Live Dynamics", -1);
    m_dynamicsPanel.SetLiveUndoOpen(false);
  }
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
      if (m_dynamicsPanel.IsEditingValue()) {
        // Inline editor open (fallback WM_KEYDOWN path; the accelerator normally
        // consumes this) - ESC cancels the edit, never closes the panel.
        m_dynamicsPanel.OnEditKey(VK_ESCAPE);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (m_settingsPanel.IsVisible()) {
        m_settingsPanel.Hide();
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (m_dynamicsPanel.IsVisible()) {
        CloseDynamicsPanel();   // #77: ESC closes the dynamics panel
      } else if (m_workingSet.active) {
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
      if (ctrl) {
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) RedoRestore();
        else UndoRestore();
      }
      break;
    case 'Y':
      if (ctrl) RedoRestore(); // the Windows-style redo chord
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
    case 'D':
    case 'd':
      // #77: toggle the dynamics panel. Open = the same path as the context-menu
      // command (envelope ensure + analysis + P_EXT load); close = full side effects.
      if (!ctrl) {
        if (m_dynamicsPanel.IsVisible()) CloseDynamicsPanel();
        else OnContextMenuCommand(CM_APPLY_DYNAMICS);
      }
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

// Named-action entry (forum #51): REAPER's Action List can bind any toolbar
// command; the action runs exactly what a toolbar click runs.
void SneakPeak::RunToolbarCommand(int button)
{
  OnToolbarClick(button);
}

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
