/* win32_utf8_unit.c - Windows-only TU wrapping WDL's win32_utf8.c.
 * The target builds with WIN32_LEAN_AND_MEAN, which strips shellapi (HDROP)
 * and commdlg (OPENFILENAME) from <windows.h>; WDL's implementation needs
 * both, and the WDL checkout is upstream (cloned fresh on CI), so the
 * prerequisite includes live here instead of patching WDL.
 */
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include "win32_utf8.c"
