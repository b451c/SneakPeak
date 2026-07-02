// platform.h — Cross-platform abstraction for SneakPeak
#pragma once

#ifdef _WIN32
  #include <windows.h>
#else
  #ifndef WDL_NO_DEFINE_MINMAX
    #define WDL_NO_DEFINE_MINMAX
  #endif
  #ifndef SWELL_PROVIDED_BY_APP
    #define SWELL_PROVIDED_BY_APP
  #endif
  #include "swell/swell.h"

  #ifndef GWLP_USERDATA
    #define GWLP_USERDATA GWL_USERDATA
  #endif
  #ifndef GWLP_WNDPROC
    #define GWLP_WNDPROC GWL_WNDPROC
  #endif
  #ifndef SetWindowLongPtr
    #define SetWindowLongPtr SetWindowLong
  #endif
  #ifndef GetWindowLongPtr
    #define GetWindowLongPtr GetWindowLong
  #endif

  // Mouse key flags (not provided by SWELL)
  #ifndef MK_LBUTTON
    #define MK_LBUTTON 0x0001
  #endif
  #ifndef MK_RBUTTON
    #define MK_RBUTTON 0x0002
  #endif
  #ifndef MK_SHIFT
    #define MK_SHIFT   0x0004
  #endif
  #ifndef MK_CONTROL
    #define MK_CONTROL 0x0008
  #endif
  #ifndef MK_MBUTTON
    #define MK_MBUTTON 0x0010
  #endif
  // Font constants not in SWELL
  #ifndef FF_SWISS
    #define FF_SWISS 0
  #endif
  #ifndef DEFAULT_PITCH
    #define DEFAULT_PITCH 0
  #endif
#endif

// UTF-8 on Win32 (forum #63): REAPER hands us UTF-8 strings everywhere; the Win32
// ANSI APIs would garble them (take/file names, our UTF-8 glyph literals). WDL's
// win32_utf8 remaps the text/file/menu/dialog APIs to UTF-8-aware wrappers on
// Windows (SetWindowText, MessageBox, DragQueryFile, fopen, GetOpen/SaveFileName,
// menus, ...). DrawText is NOT remapped - call DrawTextUTF8 explicitly; on SWELL
// platforms (already UTF-8 native) it maps straight back to DrawText.
#ifdef _WIN32
  // WIN32_LEAN_AND_MEAN strips these from <windows.h>, but win32_utf8.h's
  // prototypes need HDROP (shellapi) and LPOPENFILENAME (commdlg).
  #include <shellapi.h>
  #include <commdlg.h>
#endif
#include "win32_utf8.h"

// Portable dialog creation
#ifdef _WIN32
inline HWND CreateSneakPeakDialog(HWND parent, DLGPROC dlgProc, LPARAM param, bool docked = false) {
  #pragma pack(push, 4)
  struct { DLGTEMPLATE tmpl; WORD menu; WORD wndClass; WORD title; } dlg = {};
  #pragma pack(pop)
  if (docked) {
    dlg.tmpl.style = WS_CHILD | DS_CONTROL;
  } else {
    dlg.tmpl.style = WS_POPUP | WS_CAPTION | WS_SIZEBOX | WS_SYSMENU | WS_VISIBLE;
    dlg.tmpl.cx = 800;
    dlg.tmpl.cy = 400;
  }
  return CreateDialogIndirectParam(GetModuleHandle(nullptr), &dlg.tmpl, parent, dlgProc, param);
}
#else
#include "swell/swell-dlggen.h"

static void _sneakpeak_dlg_noop(HWND, int) {} // SWELL requires non-null createFunc

inline HWND CreateSneakPeakDialog(HWND parent, DLGPROC dlgProc, LPARAM param) {
  // SWELL resource with drop target flag for file drag & drop
  static SWELL_DialogResourceIndex res = {
    nullptr, "SneakPeak",
    SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_OPAQUE | SWELL_DLG_WS_DROPTARGET | SWELL_DLG_WS_RESIZABLE,
    _sneakpeak_dlg_noop, 800, 400, nullptr
  };
  return SWELL_CreateDialog(&res, nullptr, parent, dlgProc, param);
}
#endif

// RAII wrappers for GDI objects - prevent leaks on early return
class OwnedPen {
public:
  OwnedPen(int style, int width, COLORREF color) : m_pen(CreatePen(style, width, color)) {}
  ~OwnedPen() { if (m_pen) DeleteObject(m_pen); }
  operator HPEN() const { return m_pen; }
  OwnedPen(const OwnedPen&) = delete;
  OwnedPen& operator=(const OwnedPen&) = delete;
private:
  HPEN m_pen;
};

class OwnedBrush {
public:
  OwnedBrush(COLORREF color) : m_brush(CreateSolidBrush(color)) {}
  ~OwnedBrush() { if (m_brush) DeleteObject(m_brush); }
  operator HBRUSH() const { return m_brush; }
  void Fill(HDC hdc, const RECT* r) { FillRect(hdc, r, m_brush); }
  OwnedBrush(const OwnedBrush&) = delete;
  OwnedBrush& operator=(const OwnedBrush&) = delete;
private:
  HBRUSH m_brush;
};

class DCPenScope {
public:
  DCPenScope(HDC hdc, HPEN pen) : m_hdc(hdc), m_old((HPEN)SelectObject(hdc, pen)) {}
  ~DCPenScope() { SelectObject(m_hdc, m_old); }
  DCPenScope(const DCPenScope&) = delete;
  DCPenScope& operator=(const DCPenScope&) = delete;
private:
  HDC m_hdc;
  HPEN m_old;
};

// Frosted translucent rectangle: read the already-painted pixels back from the
// paint buffer, brighten them toward white by alpha/256, and write them back -
// true translucency without AlphaBlend (which SWELL does not provide). The same
// Audition-class treatment as the spectral marquee haze (WhitenPx in
// spectral_view.cpp): channel-order agnostic (all three color bytes get the
// same whitening), and stride-safe - the scratch context is allocated at a
// multiple-of-8 width so stride == alloc width on every backend (forum #65).
// Callers pass coordinates already clamped to the painted buffer.
inline void DrawFrostedRect(HDC hdc, int x1, int y1, int x2, int y2, unsigned int alpha)
{
  int w = x2 - x1, h = y2 - y1;
  if (w <= 0 || h <= 0) return;
  const int allocW = (w + 7) & ~7;

  auto whiten = [&](unsigned int* fbuf) {
    for (int y = 0; y < h; y++) {
      unsigned int* row = fbuf + (size_t)y * (size_t)allocW;
      for (int x = 0; x < w; x++) {
        unsigned int c = row[x];
        unsigned int r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
        r += ((255u - r) * alpha) >> 8;
        g += ((255u - g) * alpha) >> 8;
        b += ((255u - b) * alpha) >> 8;
        row[x] = (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
      }
    }
  };

#ifdef _WIN32
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = allocW;
  bmi.bmiHeader.biHeight = -h;   // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp) return;
  HDC mem = CreateCompatibleDC(hdc);
  if (!mem) { DeleteObject(bmp); return; }
  HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, hdc, x1, y1, SRCCOPY);
  whiten((unsigned int*)bits);
  BitBlt(hdc, x1, y1, w, h, mem, 0, 0, SRCCOPY);
  SelectObject(mem, oldBmp);
  DeleteDC(mem);
  DeleteObject(bmp);
#else
  HDC mem = SWELL_CreateMemContext(hdc, allocW, h);
  if (!mem) return;
  BitBlt(mem, 0, 0, w, h, hdc, x1, y1, SRCCOPY);
  unsigned int* fbuf = (unsigned int*)SWELL_GetCtxFrameBuffer(mem);
  if (fbuf) {
    whiten(fbuf);
    BitBlt(hdc, x1, y1, w, h, mem, 0, 0, SRCCOPY);
  }
  SWELL_DeleteGfxContext(mem);
#endif
}
