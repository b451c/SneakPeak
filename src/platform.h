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
