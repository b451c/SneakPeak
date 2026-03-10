// edit_view.cpp — Main EditView window, double-buffered GDI rendering
// Includes: markers, clipboard ops, destructive editing, context menu
#include "edit_view.h"
#include "audio_engine.h"
#include "audio_ops.h"
#include "theme.h"
#include "debug.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>

AudioClipboard EditView::s_clipboard;

// Helper: portable AppendMenu (SWELL doesn't have it directly)
static void MenuAppend(HMENU menu, unsigned int flags, UINT_PTR id, const char* str)
{
#ifdef _WIN32
  AppendMenuA(menu, flags, id, str);
#else
  SWELL_Menu_AddMenuItem(menu, str, (int)id, flags);
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

EditView::EditView() {}
EditView::~EditView() { Destroy(); }

void EditView::Create()
{
  if (m_hwnd) return;

  m_hwnd = CreateEditViewDialog(g_reaperMainHwnd, DlgProc, (LPARAM)this);
  if (!m_hwnd) {
    DBG("[EditView] Failed to create dialog\n");
    return;
  }

  if (g_DockWindowAddEx) {
    g_DockWindowAddEx(m_hwnd, "EditView", "EditView_main", true);
  }

  ShowWindow(m_hwnd, SW_SHOW);
  SetTimer(m_hwnd, TIMER_REFRESH, TIMER_INTERVAL_MS, nullptr);

  // Restore persisted settings
  if (g_GetExtState) {
    const char* snap = g_GetExtState("EditView", "snap_zero");
    if (snap && snap[0] == '1') m_waveform.SetSnapToZero(true);
    const char* mm = g_GetExtState("EditView", "minimap");
    if (mm && mm[0] == '1') m_minimapVisible = true;
    const char* mmh = g_GetExtState("EditView", "minimap_h");
    if (mmh && mmh[0]) {
      int h = atoi(mmh);
      if (h >= MINIMAP_HEIGHT && h <= 120) m_minimapHeight = h;
    }
  }

  DBG("[EditView] Window created: hwnd=%p\n", (void*)m_hwnd);
}

void EditView::Destroy()
{
  if (!m_hwnd) return;
  CleanupDragTemp();
  KillTimer(m_hwnd, TIMER_REFRESH);
  if (g_DockWindowRemove) g_DockWindowRemove(m_hwnd);
  DestroyWindow(m_hwnd);
  m_hwnd = nullptr;
}

void EditView::Toggle()
{
  if (!m_hwnd) return;
  ShowWindow(m_hwnd, IsVisible() ? SW_HIDE : SW_SHOW);
}

bool EditView::IsVisible() const
{
  return m_hwnd && IsWindowVisible(m_hwnd);
}

void EditView::LoadSelectedItem()
{
  if (!g_CountSelectedMediaItems || !g_GetSelectedMediaItem) return;

  CleanupDragTemp();

  int count = g_CountSelectedMediaItems(nullptr);
  if (count <= 0) {
    m_waveform.ClearItem();
    m_hasUndo = false;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  MediaItem* item = g_GetSelectedMediaItem(nullptr, 0);
  if (!item) return;

  // Multi-item: show selected items from same track as one continuous waveform
  if (count > 1 && g_GetMediaItemInfo_Value && g_GetMediaItem_Track) {
    MediaTrack* track0 = g_GetMediaItem_Track(item);
    std::vector<MediaItem*> items;
    for (int i = 0; i < count; i++) {
      MediaItem* mi = g_GetSelectedMediaItem(nullptr, i);
      if (mi && g_GetMediaItem_Track(mi) == track0) items.push_back(mi);
    }

    // Sort by timeline position
    std::sort(items.begin(), items.end(), [](MediaItem* a, MediaItem* b) {
      return g_GetMediaItemInfo_Value(a, "D_POSITION") < g_GetMediaItemInfo_Value(b, "D_POSITION");
    });

    DBG("[EditView] Multi-item: %d items selected\n", (int)items.size());
    for (size_t i = 0; i < items.size() && i < 8; i++) {
      double p = g_GetMediaItemInfo_Value(items[i], "D_POSITION");
      double l = g_GetMediaItemInfo_Value(items[i], "D_LENGTH");
      DBG("[EditView]   item[%d]: pos=%.3f len=%.3f\n", (int)i, p, l);
    }

    if (items.size() > 1) {
      m_waveform.SetItems(items);
      m_spectral.ClearSpectrum();
      m_spectral.Invalidate();
      m_minimap.Invalidate();
      m_gainPanel.Show(items[0]);
      m_hasUndo = false;
      m_dirty = false;
      if (m_hwnd) {
        char title[512];
        snprintf(title, sizeof(title), "EditView [%d items]", (int)items.size());
        SetWindowText(m_hwnd, title);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      DBG("[EditView] Multi-item loaded: segments=%d audioFrames=%d dur=%.3f\n",
          (int)m_waveform.GetSegments().size(), m_waveform.GetAudioSampleCount(),
          m_waveform.GetItemDuration());
      return;
    }
  }

  // Clear first to exit multi-item mode if active
  if (m_waveform.IsMultiItem()) m_waveform.ClearItem();
  m_waveform.SetItem(item);
  m_spectral.ClearSpectrum();
  m_spectral.Invalidate();
  m_minimap.Invalidate();

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
    char title[512];
    GetItemTitle(title, sizeof(title));
    SetWindowText(m_hwnd, title);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void EditView::OnTimer()
{
  if (!m_hwnd || !IsVisible()) return;

  // Auto-scroll when dragging selection near edges
  if (m_dragging && m_waveform.HasItem()) {
    int edgeZone = 40; // pixels from edge to trigger scroll
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

    // Auto-stop: when EditView-initiated playback exits item bounds, stop once
    if (playing && m_startedPlayback && !m_autoStopped && m_playGraceTicks == 0 &&
        g_GetPlayPosition2 && m_waveform.HasItem()) {
      double playPos = g_GetPlayPosition2();
      double itemStart = m_waveform.GetItemPosition();
      double itemEnd = itemStart + m_waveform.GetItemDuration();
      if (playPos >= itemEnd || playPos < itemStart) {
        DBG("[EditView] Auto-stop: playPos=%.3f itemEnd=%.3f\n", playPos, itemEnd);
        m_autoStopped = true;  // prevent re-triggering until next user play
        m_startedPlayback = false;
        if (g_OnStopButton) g_OnStopButton();
      }
    }

    // Detect external playback start (we didn't press Space)
    if (playing && !m_wasPlaying && !m_startedPlayback) {
      // External play — track follow mode is active by default
    }
    if (!playing && m_wasPlaying) {
      m_startedPlayback = false;
      m_autoStopped = false;  // reset for next play
    }
    m_wasPlaying = playing;

    if (playing) {
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
  }

  // Keep repainting while RMS meters are decaying after stop
  if (m_levels.IsDecaying()) {
    InvalidateRect(m_hwnd, &m_bottomPanelRect, FALSE);
  }

  // Keep repainting while spectral is computing (progress bar update)
  if (m_spectralVisible && m_spectral.IsLoading()) {
    InvalidateRect(m_hwnd, &m_spectralRect, FALSE);
  }

  // Update fade/volume cache for paint
  if (m_waveform.HasItem()) m_waveform.UpdateFadeCache();

  // Single-item only: refresh position/duration, detect channel mode changes
  if (m_waveform.HasItem() && !m_waveform.IsMultiItem() && g_GetMediaItemInfo_Value) {
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

  // Cursor + RMS levels — works for both single and multi-item
  if (m_waveform.HasItem()) {
    double itemPos = m_waveform.GetItemPosition();

    if (g_GetCursorPosition) {
      double curPos = g_GetCursorPosition();
      double relPos = curPos - itemPos;
      // Clamp cursor to item bounds
      double dur = m_waveform.GetItemDuration();
      if (relPos < 0.0) relPos = 0.0;
      if (relPos > dur) relPos = dur;
      m_waveform.SetCursorTime(relPos);
    }

    int sr = m_waveform.GetSampleRate();
    int nch = m_waveform.GetNumChannels();
    if (sr > 0 && nch > 0) {
      bool playing = g_GetPlayState && (g_GetPlayState() & 1);
      int startFrame, endFrame;
      if (playing && g_GetPlayPosition2) {
        double playPos = g_GetPlayPosition2() - itemPos;
        int center = static_cast<int>(playPos * sr);
        int halfWin = sr / 40;
        startFrame = center - halfWin;
        endFrame = center + halfWin;
      } else {
        startFrame = static_cast<int>(m_waveform.GetViewStart() * sr);
        endFrame = static_cast<int>(m_waveform.GetViewEnd() * sr);
      }
      double itemVol = m_waveform.IsMultiItem() ? 1.0
        : (g_GetMediaItemInfo_Value ? g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_VOL") : 1.0);
      const bool chActive[2] = { m_waveform.IsChannelActive(0), m_waveform.IsChannelActive(1) };
      m_levels.Update(m_waveform.GetAudioData(), startFrame, endFrame, sr, nch, itemVol, playing, chActive);
    }
  }
}

void EditView::GetItemTitle(char* buf, int bufSize)
{
  const char* prefix = m_dirty ? "* " : "";
  if (!m_waveform.HasItem()) {
    snprintf(buf, bufSize, "%sEditView", prefix);
    return;
  }
  MediaItem_Take* take = g_GetActiveTake ? g_GetActiveTake(m_waveform.GetItem()) : nullptr;
  if (take && g_GetSetMediaItemTakeInfo_String) {
    char nameBuf[256] = {};
    if (g_GetSetMediaItemTakeInfo_String(take, "P_NAME", nameBuf, false)) {
      snprintf(buf, bufSize, "%sEditView: %s", prefix, nameBuf);
      return;
    }
  }
  snprintf(buf, bufSize, "%sEditView", prefix);
}

// --- Dialog Procedure ---

INT_PTR CALLBACK EditView::DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  EditView* self = nullptr;

  if (msg == WM_CREATE || msg == WM_INITDIALOG) {
    self = reinterpret_cast<EditView*>(lParam);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    if (self) self->m_hwnd = hwnd;
    return 0;
  }

  self = reinterpret_cast<EditView*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (!self) return DefWindowProc(hwnd, msg, wParam, lParam);

  INT_PTR result = self->HandleMessage(msg, wParam, lParam);
  if (result == -1) return DefWindowProc(hwnd, msg, wParam, lParam);
  return result;
}

INT_PTR EditView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
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
      if (id >= CM_UNDO && id <= CM_SUPPORT_PAYPAL) {
        OnContextMenuCommand(id);
        return 0;
      }
      break;
    }

    case WM_TIMER:
      if (wParam == TIMER_REFRESH) OnTimer();
      return 0;

    case WM_CLOSE:
      // Docker [x] button or undock — hide window instead of destroying
      ShowWindow(m_hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      KillTimer(m_hwnd, TIMER_REFRESH);
      return 0;
  }

  return -1;
}

// --- Layout ---

void EditView::RecalcLayout(int w, int h)
{
  m_toolbarRect      = { 0, 0, w, TOOLBAR_HEIGHT };
  m_rulerRect        = { 0, TOOLBAR_HEIGHT, w, TOOLBAR_HEIGHT + RULER_HEIGHT };
  m_bottomPanelRect  = { 0, h - BOTTOM_PANEL_HEIGHT, w, h };
  int minimapH = m_minimapVisible ? m_minimapHeight : 0;
  m_scrollbarRect    = { 0, h - BOTTOM_PANEL_HEIGHT - SCROLLBAR_HEIGHT, w, h - BOTTOM_PANEL_HEIGHT };
  m_minimapRect      = { 0, m_scrollbarRect.top - minimapH, w, m_scrollbarRect.top };

  int contentTop = TOOLBAR_HEIGHT + RULER_HEIGHT;
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

void EditView::OnSize(int w, int h)
{
  RecalcLayout(w, h);
  m_waveform.Invalidate();
  m_spectral.Invalidate();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Painting ---

void EditView::OnPaint(HDC hdc)
{
  if (!hdc) return;

  DrawRuler(hdc);
  m_waveform.Paint(hdc);
  if (m_markers.m_showMarkers) m_markers.DrawMarkers(hdc, m_waveformRect, m_rulerRect, m_waveform);
  if (m_waveform.HasItem()) m_gainPanel.Draw(hdc, m_waveformRect);
  if (m_waveform.HasItem()) DrawSoloButton(hdc);
  if (m_spectralVisible) {
    DrawSplitter(hdc);
    m_spectral.Paint(hdc, m_waveform);
  }
  if (m_minimapVisible) m_minimap.Paint(hdc, m_waveform);
  DrawScrollbar(hdc);
  DrawBottomPanel(hdc);
}

void EditView::DrawSplitter(HDC hdc)
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

void EditView::DrawRuler(HDC hdc)
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

  HFONT rulerFont = CreateFont(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
  HFONT oldFont = (HFONT)SelectObject(hdc, rulerFont);

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
  DeleteObject(rulerFont);
}

void EditView::DrawScrollbar(HDC hdc)
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

void EditView::DrawSoloButton(HDC hdc)
{
  if (!m_waveform.HasItem()) return;

  // Position: top-right of waveform area, well left of dB scale and fade handles
  int btnW = 22, btnH = 16;
  int btnX = m_waveformRect.right - DB_SCALE_WIDTH - btnW - 30;
  int btnY = m_waveformRect.top + 10;
  m_soloBtnRect = { btnX, btnY, btnX + btnW, btnY + btnH };

  // Update solo state from REAPER
  UpdateSoloState();

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
  HFONT font = CreateFont(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
  HFONT oldFont = (HFONT)SelectObject(hdc, font);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, m_trackSoloed ? RGB(0, 0, 0) : RGB(140, 140, 140));
  DrawText(hdc, "S", 1, &m_soloBtnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(hdc, oldFont);
  DeleteObject(font);
}

bool EditView::ClickSoloButton(int x, int y)
{
  if (!m_waveform.HasItem()) return false;
  return x >= m_soloBtnRect.left && x < m_soloBtnRect.right &&
         y >= m_soloBtnRect.top && y < m_soloBtnRect.bottom;
}

void EditView::ToggleTrackSolo()
{
  if (!g_GetMediaItem_Track || !g_GetSetMediaTrackInfo) return;

  MediaItem* item = m_waveform.GetItem();
  if (!item) return;

  MediaTrack* track = g_GetMediaItem_Track(item);
  if (!track) return;

  // Read current solo state
  int* pSolo = (int*)g_GetSetMediaTrackInfo(track, "I_SOLO", nullptr);
  int solo = pSolo ? *pSolo : 0;

  // Toggle: 0 = off, 2 = solo-in-place (SIP)
  int newSolo = (solo == 0) ? 2 : 0;
  g_GetSetMediaTrackInfo(track, "I_SOLO", &newSolo);

  m_trackSoloed = (newSolo != 0);
  if (g_UpdateArrange) g_UpdateArrange();
}

void EditView::UpdateSoloState()
{
  if (!g_GetMediaItem_Track || !g_GetSetMediaTrackInfo || !m_waveform.HasItem()) {
    m_trackSoloed = false;
    return;
  }
  MediaTrack* track = g_GetMediaItem_Track(m_waveform.GetItem());
  if (!track) { m_trackSoloed = false; return; }

  int* pSolo = (int*)g_GetSetMediaTrackInfo(track, "I_SOLO", nullptr);
  m_trackSoloed = (pSolo && *pSolo != 0);
}

void EditView::DrawBottomPanel(HDC hdc)
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

  // RMS meters on the left (fills most space)
  RECT metersRect = { m_bottomPanelRect.left, m_bottomPanelRect.top + 1,
                      dividerX - 1, m_bottomPanelRect.bottom };
  m_levels.Draw(hdc, metersRect, m_waveform.GetNumChannels());

  if (!m_waveform.HasItem()) return;

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

// --- Mouse ---

void EditView::OnDoubleClick(int x, int y)
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

void EditView::OnMouseDown(int x, int y, WPARAM wParam)
{
  m_lastMouseX = x;
  m_lastMouseY = y;

  if (y >= m_toolbarRect.top && y < m_toolbarRect.bottom) {
    int btn = m_toolbar.HitTest(x, y);
    if (btn >= 0) OnToolbarClick(btn);
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
        g_SetEditCurPos(m_waveform.GetItemPosition() + time, false, false);
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

  // Minimap click — scroll to that position
  if (m_minimapVisible && y >= m_minimapRect.top && y < m_minimapRect.bottom && m_waveform.HasItem()) {
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
          g_SetEditCurPos(m_waveform.GetItemPosition() + time, false, false);
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

      // Check fade handles first (8px hit zone around handle)
      if (g_GetMediaItemInfo_Value) {
        double fadeInLen = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_FADEINLEN");
        double fadeOutLen = g_GetMediaItemInfo_Value(m_waveform.GetItem(), "D_FADEOUTLEN");
        int fiX = m_waveform.TimeToX(fadeInLen);
        int foX = m_waveform.TimeToX(m_waveform.GetItemDuration() - fadeOutLen);
        if (fadeInLen >= 0.001 && abs(x - fiX) <= 8 && y < (m_waveformRect.top + m_waveformRect.bottom) / 2) {
          m_fadeDragging = FADE_IN;
          m_fadeDragStartY = y;
          m_fadeDragStartShape = (int)g_GetMediaItemInfo_Value(m_waveform.GetItem(), "C_FADEINSHAPE");
          SetCapture(m_hwnd);
          if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
          return;
        }
        if (fadeOutLen >= 0.001 && abs(x - foX) <= 8 && y < (m_waveformRect.top + m_waveformRect.bottom) / 2) {
          m_fadeDragging = FADE_OUT;
          m_fadeDragStartY = y;
          m_fadeDragStartShape = (int)g_GetMediaItemInfo_Value(m_waveform.GetItem(), "C_FADEOUTSHAPE");
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
          DBG("[EditView] Drag export pending: click at t=%.3f inside sel [%.3f..%.3f]\n",
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
          g_SetEditCurPos(m_waveform.GetItemPosition() + time, false, false);
      }
      m_dragging = true;
      SetCapture(m_hwnd);
      InvalidateRect(m_hwnd, nullptr, FALSE);
    }
  }
}

void EditView::OnMouseUp(int x, int y)
{
  if (m_dragExportPending) {
    // Didn't meet drag threshold — treat as click inside selection (place cursor)
    m_dragExportPending = false;
    ReleaseCapture();
    double time = m_waveform.XToTime(x);
    m_waveform.SetCursorTime(time);
    if (g_SetEditCurPos)
      g_SetEditCurPos(m_waveform.GetItemPosition() + time, false, false);
    m_waveform.ClearSelection();
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
      g_SetExtState("EditView", "minimap_h", buf, true);
    }
    return;
  }
  if (m_splitterDragging) {
    m_splitterDragging = false;
    ReleaseCapture();
    return;
  }
  if (m_fadeDragging != FADE_NONE) {
    m_fadeDragging = FADE_NONE;
    m_waveform.SetFadeDragInfo(0, 0);
    ReleaseCapture();
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Adjust fade", -1);
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (m_gainPanel.IsDragging()) {
    m_gainPanel.OnMouseUp();
    ReleaseCapture();
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

void EditView::OnMouseMove(int x, int y, WPARAM wParam)
{
  // Drag export: check threshold
  if (m_dragExportPending) {
    int dx = x - m_dragStartX;
    int dy = y - m_dragStartY;
    if (dx * dx + dy * dy > 25) {  // 5px threshold
      DBG("[EditView] Drag threshold met: dx=%d dy=%d, initiating export\n", dx, dy);
      m_dragExportPending = false;
      InitiateDragExport();
      ReleaseCapture();
      return;
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
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
    int contentTop = TOOLBAR_HEIGHT + RULER_HEIGHT;
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

  if (m_fadeDragging != FADE_NONE && m_waveform.HasItem() && g_SetMediaItemInfo_Value) {
    double time = m_waveform.XToTime(x);
    MediaItem* item = m_waveform.GetItem();

    // Horizontal = fade length
    if (m_fadeDragging == FADE_IN) {
      double fadeLen = std::max(0.0, std::min(time, m_waveform.GetItemDuration()));
      g_SetMediaItemInfo_Value(item, "D_FADEINLEN", fadeLen);
    } else {
      double fadeLen = std::max(0.0, m_waveform.GetItemDuration() - time);
      fadeLen = std::min(fadeLen, m_waveform.GetItemDuration());
      g_SetMediaItemInfo_Value(item, "D_FADEOUTLEN", fadeLen);
    }

    // Vertical = fade shape: down = higher number (more curve), up = lower (more linear)
    // Dead zone of 6px before first shape change, then 12px per step
    int dy = y - m_fadeDragStartY;
    int shapeOffset = 0;
    if (dy > 6) shapeOffset = (dy - 6) / 12 + 1;
    else if (dy < -6) shapeOffset = (dy + 6) / 12 - 1;
    int newShape = std::max(0, std::min(6, m_fadeDragStartShape + shapeOffset));
    const char* shapeParam = (m_fadeDragging == FADE_IN) ? "C_FADEINSHAPE" : "C_FADEOUTSHAPE";
    g_SetMediaItemInfo_Value(item, shapeParam, (double)newShape);

    // Pass drag info to waveform for label display
    m_waveform.SetFadeDragInfo((m_fadeDragging == FADE_IN) ? 1 : 2, newShape);

    if (g_UpdateArrange) g_UpdateArrange();
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

  m_lastMouseX = x;
  m_lastMouseY = y;
}

void EditView::OnMouseWheel(int x, int y, int delta, WPARAM wParam)
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

void EditView::OnKeyDown(WPARAM key)
{
  bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

  switch (key) {
    case VK_HOME:
      if (m_waveform.HasItem()) {
        m_waveform.SetCursorTime(0.0);
        if (g_SetEditCurPos) g_SetEditCurPos(m_waveform.GetItemPosition(), false, false);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    case VK_END:
      if (m_waveform.HasItem()) {
        m_waveform.SetCursorTime(m_waveform.GetItemDuration());
        if (g_SetEditCurPos) g_SetEditCurPos(m_waveform.GetItemPosition() + m_waveform.GetItemDuration(), false, false);
        InvalidateRect(m_hwnd, nullptr, FALSE);
      }
      break;
    case VK_SPACE: {
      bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      if (shift && m_waveform.HasItem() && m_waveform.HasSelection()) {
        DoLoopSelection();
      } else if (g_GetPlayState && g_OnPlayButton && g_OnStopButton) {
        if (g_GetPlayState() & 1) {
          g_OnStopButton();
        } else {
          m_startedPlayback = true;
          m_autoStopped = false;
          m_playGraceTicks = 5; // ~165ms for REAPER to update play position
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
        InvalidateRect(m_hwnd, nullptr, FALSE);
      } else if (g_OnStopButton) {
        g_OnStopButton();
      }
      break;
    case VK_DELETE:
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
    case 'N':
      if (ctrl) DoNormalize();
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

void EditView::OnRightClick(int x, int y)
{
  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  bool hasItem = m_waveform.HasItem();
  bool hasSel = m_waveform.HasSelection();
  bool hasClip = s_clipboard.numFrames > 0;

  // If right-clicking near a marker, show marker actions at top level for quick access
  m_markers.m_rightClickMarkerIdx = m_markers.HitTestMarker(x, m_waveform, 8);
  if (m_markers.m_rightClickMarkerIdx >= 0) {
    MenuAppend(menu, MF_STRING, CM_EDIT_MARKER, "Edit Marker...");
    MenuAppend(menu, MF_STRING, CM_DELETE_MARKER, "Delete Marker");
    MenuAppendSeparator(menu);
  }

  // Edit submenu
  HMENU editMenu = CreatePopupMenu();
  MenuAppend(editMenu, m_hasUndo ? MF_STRING : MF_GRAYED, CM_UNDO, "Undo\tCtrl+Z");
  MenuAppendSeparator(editMenu);
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_CUT, "Cut\tCtrl+X");
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_COPY, "Copy\tCtrl+C");
  MenuAppend(editMenu, (hasItem && hasClip) ? MF_STRING : MF_GRAYED, CM_PASTE, "Paste (destructive)\tCtrl+V");
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_DELETE, "Delete\tDel");
  MenuAppend(editMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_SILENCE, "Silence\tCtrl+Del");
  MenuAppendSeparator(editMenu);
  MenuAppend(editMenu, hasItem ? MF_STRING : MF_GRAYED, CM_SELECT_ALL, "Select All\tCtrl+A");

  // Process submenu
  HMENU procMenu = CreatePopupMenu();
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_NORMALIZE, "Normalize");
  MenuAppend(procMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_FADE_IN, "Fade In");
  MenuAppend(procMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_FADE_OUT, "Fade Out");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_REVERSE, "Reverse (destructive)");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_GAIN_UP, "Gain +3dB");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_GAIN_DOWN, "Gain -3dB");
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_DC_REMOVE, "DC Offset Remove (destructive)");
  MenuAppendSeparator(procMenu);
  MenuAppend(procMenu, hasItem ? MF_STRING : MF_GRAYED, CM_GAIN_PANEL, "Gain Control...\tG");
  MenuAppendSeparator(procMenu);
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
  m_markers.m_rightClickMarkerIdx = m_markers.HitTestMarker(x, m_waveform);
  MenuAppend(markerMenu, (m_markers.m_rightClickMarkerIdx >= 0) ? MF_STRING : MF_GRAYED, CM_EDIT_MARKER, "Edit Marker...");
  MenuAppend(markerMenu, (m_markers.m_rightClickMarkerIdx >= 0) ? MF_STRING : MF_GRAYED, CM_DELETE_MARKER, "Delete Marker");
  MenuAppendSeparator(markerMenu);
  MenuAppend(markerMenu, m_markers.m_showMarkers ? (MF_STRING | MF_CHECKED) : MF_STRING, CM_SHOW_MARKERS, "Show Markers");

  // View submenu
  HMENU viewMenu = CreatePopupMenu();
  MenuAppend(viewMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ZOOM_IN, "Zoom In");
  MenuAppend(viewMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ZOOM_OUT, "Zoom Out");
  MenuAppend(viewMenu, hasItem ? MF_STRING : MF_GRAYED, CM_ZOOM_FIT, "Zoom to Fit");
  MenuAppend(viewMenu, (hasItem && hasSel) ? MF_STRING : MF_GRAYED, CM_ZOOM_SEL, "Zoom to Selection");
  MenuAppendSeparator(viewMenu);
  MenuAppend(viewMenu, MF_STRING, CM_TOGGLE_SPECTRAL,
             m_spectralVisible ? "Spectral View  \xE2\x9C\x93" : "Spectral View");
  {
    UINT snapFlags = MF_STRING;
    if (m_waveform.GetSnapToZero()) snapFlags |= MF_CHECKED;
    MenuAppend(viewMenu, snapFlags, CM_SNAP_ZERO, "Snap to Zero-Crossing");
  }
  {
    UINT mmFlags = MF_STRING;
    if (m_minimapVisible) mmFlags |= MF_CHECKED;
    MenuAppend(viewMenu, mmFlags, CM_MINIMAP, "Minimap");
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
  DestroyMenu(viewMenu);
  DestroyMenu(supportMenu);
#endif
  DestroyMenu(menu);
}

void EditView::OnContextMenuCommand(int id)
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
    case CM_NORMALIZE: DoNormalize(); break;
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
    case CM_SHOW_MARKERS: m_markers.m_showMarkers = !m_markers.m_showMarkers; break;
    case CM_ADD_MARKER:  m_markers.AddMarkerAtCursor(m_waveform); break;
    case CM_ADD_REGION:  m_markers.AddRegionFromSelection(m_waveform); break;
    case CM_DELETE_MARKER:
      if (m_markers.m_rightClickMarkerIdx >= 0) m_markers.DeleteMarkerByEnumIdx(m_markers.m_rightClickMarkerIdx);
      break;
    case CM_EDIT_MARKER:
      if (m_markers.m_rightClickMarkerIdx >= 0) m_markers.EditMarkerDialog(m_markers.m_rightClickMarkerIdx);
      break;
    case CM_GAIN_PANEL:
      m_gainPanel.Toggle(m_waveform.GetItem());
      break;
    case CM_MONO_DOWNMIX:
      DBG("[EditView] CM_MONO_DOWNMIX: hasItem=%d hasTake=%d hasAPI=%d\n",
          m_waveform.HasItem(), m_waveform.GetTake() != nullptr, g_GetSetMediaItemTakeInfo != nullptr);
      if (m_waveform.HasItem() && m_waveform.GetTake() && g_GetSetMediaItemTakeInfo) {
        MediaItem* item = m_waveform.GetItem();
        MediaItem_Take* take = m_waveform.GetTake();
        int* pCM = (int*)g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", nullptr);
        int chanMode = pCM ? *pCM : -999;
        int newMode = (chanMode == 2) ? 0 : 2;
        DBG("[EditView] DOWNMIX: pCM=%p chanMode=%d -> newMode=%d\n", (void*)pCM, chanMode, newMode);
        if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
        int* pSet = (int*)g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", &newMode);
        DBG("[EditView] DOWNMIX: after set pSet=%p\n", (void*)pSet);
        // Verify it was set
        int* pVerify = (int*)g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", nullptr);
        int verifyMode = pVerify ? *pVerify : -999;
        DBG("[EditView] DOWNMIX: verify after set: %d (expected %d)\n", verifyMode, newMode);
        if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Toggle Mono Downmix", -1);
        m_lastChanMode = newMode;
        if (g_UpdateArrange) g_UpdateArrange();
        if (g_UpdateTimeline) g_UpdateTimeline();
        // Force reload
        m_waveform.ClearItem();
        m_waveform.SetItem(item);
        if (m_gainPanel.IsVisible()) m_gainPanel.Show(item);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        DBG("[EditView] DOWNMIX: reload done, numCh=%d\n", m_waveform.GetNumChannels());
      }
      break;
    case CM_SNAP_ZERO:
      m_waveform.SetSnapToZero(!m_waveform.GetSnapToZero());
      if (g_SetExtState)
        g_SetExtState("EditView", "snap_zero", m_waveform.GetSnapToZero() ? "1" : "0", true);
      break;
    case CM_MINIMAP:
      m_minimapVisible = !m_minimapVisible;
      if (g_SetExtState)
        g_SetExtState("EditView", "minimap", m_minimapVisible ? "1" : "0", true);
      m_minimap.Invalidate();
      {
        RECT cr;
        GetClientRect(m_hwnd, &cr);
        RecalcLayout(cr.right, cr.bottom);
        m_waveform.Invalidate();
      }
      break;
    case CM_TOGGLE_SPECTRAL:
      m_spectralVisible = !m_spectralVisible;
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

void EditView::OnToolbarClick(int button)
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

void EditView::GetSelectionSampleRange(int& startFrame, int& endFrame) const
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

void EditView::UndoSave()
{
  m_hasUndo = true;
}

void EditView::UndoRestore()
{
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

// --- Marker Navigation ---

void EditView::NavigateToMarker(bool forward)
{
  if (!m_waveform.HasItem() || !g_EnumProjectMarkers3 || !g_SetEditCurPos) return;

  double itemPos = m_waveform.GetItemPosition();
  double itemEnd = itemPos + m_waveform.GetItemDuration();
  double cursorAbs = itemPos + m_waveform.GetCursorTime();

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
    m_waveform.SetCursorTime(bestTime - itemPos);
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

// --- Loop Playback ---

void EditView::DoLoopSelection()
{
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (!g_GetSet_LoopTimeRange2 || !g_SetEditCurPos || !g_OnPlayButton || !g_Main_OnCommand) return;

  double itemPos = m_waveform.GetItemPosition();
  WaveformSelection sel = m_waveform.GetSelection();
  double s = itemPos + std::min(sel.startTime, sel.endTime);
  double e = itemPos + std::max(sel.startTime, sel.endTime);

  // Set loop range
  g_GetSet_LoopTimeRange2(nullptr, true, true, &s, &e, false);
  // Enable repeat (action 1068 = Toggle repeat)
  if (g_GetPlayState && !(g_GetPlayState() & 4)) {
    g_Main_OnCommand(1068, 0);
  }
  // Play from selection start
  g_SetEditCurPos(s, false, true);
  m_startedPlayback = true;
  m_autoStopped = false;
  m_playGraceTicks = 5;
  g_OnPlayButton();
}

// --- Write back to disk and refresh ---

void EditView::WriteAndRefresh()
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
                 "EditView", MB_OK | MB_ICONWARNING);
      return;
    }
  }

  const auto& data = m_waveform.GetAudioData();
  int nch = m_waveform.GetNumChannels();
  int sr = m_waveform.GetSampleRate();
  int frames = m_waveform.GetAudioSampleCount();

  AudioEngine::WriteWavFile(path, data.data(), frames, nch, sr,
                            m_wavBitsPerSample, m_wavAudioFormat);
  AudioEngine::RefreshItemSource(m_waveform.GetItem(), m_waveform.GetTake());

  m_waveform.Invalidate();
  m_dirty = true;

  // Update title bar with dirty indicator
  if (m_hwnd) {
    char title[512];
    GetItemTitle(title, sizeof(title));
    SetWindowText(m_hwnd, title);
  }
}

// --- Sync EditView selection to REAPER time selection ---

void EditView::SyncSelectionToReaper()
{
  if (!g_GetSet_LoopTimeRange2 || !m_waveform.HasItem()) return;
  double itemPos = m_waveform.GetItemPosition();
  if (m_waveform.HasSelection()) {
    WaveformSelection sel = m_waveform.GetSelection();
    double s = std::min(sel.startTime, sel.endTime) + itemPos;
    double e = std::max(sel.startTime, sel.endTime) + itemPos;
    g_GetSet_LoopTimeRange2(nullptr, true, false, &s, &e, false);
  } else {
    // Clear time selection
    double s = 0.0, e = 0.0;
    g_GetSet_LoopTimeRange2(nullptr, true, false, &s, &e, false);
  }
  if (g_UpdateTimeline) g_UpdateTimeline();
}

// --- Clipboard operations ---

void EditView::DoCopy()
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
  std::copy(data.begin() + (long)srcOffset,
            data.begin() + (long)(srcOffset + (size_t)selFrames * nch),
            s_clipboard.samples.begin());

  // Also trigger REAPER's native copy (40060 = Copy selected area of items)
  if (g_Main_OnCommand) g_Main_OnCommand(40060, 0);

  DBG("[EditView] Copied %d frames to clipboard\n", selFrames);
}

void EditView::DoCut()
{
  // Non-destructive: copy to clipboard, then delete via split+remove
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;

  DoCopy();
  DoDelete();
}

void EditView::DoPaste()
{
  if (!m_waveform.HasItem() || s_clipboard.numFrames <= 0) return;
  if (s_clipboard.numChannels != m_waveform.GetNumChannels()) return;
  if (m_waveform.IsMultiItem()) {
    MessageBox(m_hwnd, "Paste is not supported in multi-item view.", "EditView", MB_OK);
    return;
  }

  int ret = MessageBox(m_hwnd,
    "Paste modifies the audio file on disk. Continue?",
    "EditView — Destructive Operation", MB_YESNO | MB_ICONWARNING);
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

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Paste", -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoDelete()
{
  // Non-destructive: split item at selection edges, delete middle piece
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (!g_SplitMediaItem || !g_DeleteTrackMediaItem || !g_GetMediaItem_Track) return;

  MediaItem* item = m_waveform.GetItem();
  double itemPos = m_waveform.GetItemPosition();
  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  double splitStart = itemPos + selStart;
  double splitEnd = itemPos + selEnd;

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
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Delete (non-destructive)", -1);

  // Reload — item pointer may have changed, re-select
  if (rightPart) {
    // Focus on the right part (or left part if it exists)
    m_waveform.ClearItem();
    LoadSelectedItem();
  }

  m_waveform.ClearSelection();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoSilence()
{
  // Non-destructive: split selection into separate item, set its volume to 0
  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;
  if (!g_SplitMediaItem || !g_SetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double itemPos = m_waveform.GetItemPosition();
  WaveformSelection sel = m_waveform.GetSelection();
  double selStart = std::min(sel.startTime, sel.endTime);
  double selEnd = std::max(sel.startTime, sel.endTime);

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);

  double splitStart = itemPos + selStart;
  double splitEnd = itemPos + selEnd;

  // Split at end first
  g_SplitMediaItem(item, splitEnd);
  // Split at start — middlePart is the silence region
  MediaItem* middlePart = g_SplitMediaItem(item, splitStart);

  // Set middle part volume to 0 (silence)
  if (middlePart) {
    g_SetMediaItemInfo_Value(middlePart, "D_VOL", 0.0);
  }

  if (g_UpdateArrange) g_UpdateArrange();
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Silence (non-destructive)", -1);

  m_waveform.ClearItem();
  LoadSelectedItem();
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Processing ---

void EditView::DoNormalize()
{
  // Non-destructive: measure peak, set D_VOL to reach 0dB
  if (!m_waveform.HasItem()) return;
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
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Normalize (non-destructive)", -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoFadeIn()
{
  // Non-destructive: set item fade-in length via D_FADEINLEN
  if (!m_waveform.HasItem()) return;
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
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Fade In", -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoFadeOut()
{
  // Non-destructive: set item fade-out length via D_FADEOUTLEN
  if (!m_waveform.HasItem()) return;
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
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Fade Out", -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoReverse()
{
  // Destructive — no REAPER non-destructive reverse available
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsMultiItem()) {
    MessageBox(m_hwnd, "Reverse is not supported in multi-item view.", "EditView", MB_OK);
    return;
  }

  int ret = MessageBox(m_hwnd,
    "Reverse modifies the audio file on disk. Continue?",
    "EditView — Destructive Operation", MB_YESNO | MB_ICONWARNING);
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

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: Reverse (destructive)", -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoGain(double factor)
{
  // Non-destructive: multiply current D_VOL by factor
  if (!m_waveform.HasItem()) return;
  if (!g_SetMediaItemInfo_Value || !g_GetMediaItemInfo_Value) return;

  MediaItem* item = m_waveform.GetItem();
  double curVol = g_GetMediaItemInfo_Value(item, "D_VOL");
  double newVol = curVol * factor;

  if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  g_SetMediaItemInfo_Value(item, "D_VOL", newVol);
  if (g_UpdateArrange) g_UpdateArrange();

  char desc[64];
  snprintf(desc, sizeof(desc), "EditView: Gain %.1fdB", 20.0 * log10(factor));
  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, desc, -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void EditView::DoDCRemove()
{
  // Destructive — must modify audio data
  if (!m_waveform.HasItem()) return;
  if (m_waveform.IsMultiItem()) {
    MessageBox(m_hwnd, "DC Remove is not supported in multi-item view.", "EditView", MB_OK);
    return;
  }

  int ret = MessageBox(m_hwnd,
    "DC Offset Remove modifies the audio file on disk. Continue?",
    "EditView — Destructive Operation", MB_YESNO | MB_ICONWARNING);
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

  if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "EditView: DC Offset Remove (destructive)", -1);

  InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Drag & Drop Export ---

void EditView::CleanupDragTemp()
{
  if (!m_dragTempPath.empty()) {
    remove(m_dragTempPath.c_str());
    m_dragTempPath.clear();
  }
}

void EditView::InitiateDragExport()
{
  DBG("[EditView] InitiateDragExport: hasItem=%d hasSel=%d\n",
      m_waveform.HasItem(), m_waveform.HasSelection());

  if (!m_waveform.HasItem() || !m_waveform.HasSelection()) return;

  CleanupDragTemp();

  int startF, endF;
  GetSelectionSampleRange(startF, endF);
  int nch = m_waveform.GetNumChannels();
  int selFrames = endF - startF;

  DBG("[EditView] DragExport: startF=%d endF=%d nch=%d selFrames=%d dataSize=%d\n",
      startF, endF, nch, selFrames, (int)m_waveform.GetAudioData().size());

  if (selFrames <= 0 || nch <= 0) return;

  const auto& data = m_waveform.GetAudioData();
  size_t offset = (size_t)startF * (size_t)nch;
  size_t needed = offset + (size_t)selFrames * (size_t)nch;
  if (needed > data.size()) {
    DBG("[EditView] DragExport: buffer overflow! needed=%d have=%d\n",
        (int)needed, (int)data.size());
    return;
  }
  const double* selData = data.data() + offset;

  m_dragTempPath = AudioEngine::WriteTempWav(selData, selFrames, nch, m_waveform.GetSampleRate());
  DBG("[EditView] DragExport: wrote temp WAV to '%s'\n", m_dragTempPath.c_str());
  if (m_dragTempPath.empty()) return;

#ifndef _WIN32
  // SWELL_InitiateDragDropOfFileList dereferences srcrect — must not be null
  RECT dragRect = { m_dragStartX - 5, m_dragStartY - 5,
                    m_dragStartX + 5, m_dragStartY + 5 };
  const char* files[] = { m_dragTempPath.c_str() };
  DBG("[EditView] DragExport: calling SWELL_InitiateDragDropOfFileList\n");
  SWELL_InitiateDragDropOfFileList(m_hwnd, &dragRect, files, 1, nullptr);
#endif

  DBG("[EditView] DragExport: done, %d frames\n", selFrames);
}
