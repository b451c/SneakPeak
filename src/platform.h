// platform.h — Cross-platform abstraction for EditView
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
inline HWND CreateEditViewDialog(HWND parent, DLGPROC dlgProc, LPARAM param) {
  #pragma pack(push, 4)
  struct { DLGTEMPLATE tmpl; WORD menu; WORD wndClass; WORD title; } dlg = {};
  #pragma pack(pop)
  dlg.tmpl.style = WS_CHILD | DS_CONTROL;
  dlg.tmpl.cx = 800;
  dlg.tmpl.cy = 400;
  return CreateDialogIndirectParam(GetModuleHandle(nullptr), &dlg.tmpl, parent, dlgProc, param);
}
#else
inline HWND CreateEditViewDialog(HWND parent, DLGPROC dlgProc, LPARAM param) {
  return SWELL_CreateDialog(nullptr, nullptr, parent, dlgProc, param);
}
#endif
