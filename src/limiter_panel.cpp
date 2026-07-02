// limiter_panel.cpp — premium HARD LIMITER overlay (v2.4.0 INC-L1)
#include "limiter_panel.h"
#include "globals.h"
#include "ui_theme.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Knob table (SLIDER_DEFS pattern). Ceiling's unit swaps dBTP/dBFS with the
// TRUE PEAK pill at paint time; ranges mirror the plan's parameter table.
struct LimKnobDef {
  const char* label;
  double minVal, maxVal;
  const char* unit;
  int precision;
  double defaultVal;
};
static const LimKnobDef LIM_DEFS[kLimNumParams] = {
  { "Gain",    0.0,   24.0,  "dB",   1, 0.0 },
  { "Ceiling", -12.0, 0.0,   "dBTP", 1, -1.0 },
  { "Attack",  0.1,   30.0,  "ms",   1, 5.0 },
  { "Hold",    0.0,   50.0,  "ms",   0, 10.0 },
  { "Release", 10.0,  1000.0, "ms",  0, 60.0 },
};

// Factory presets (plan C7). Aggregate order = LimiterParams field order:
// gainDb, ceilingDb, attackMs, holdMs, releaseMs, truePeak, link.
const LimiterPreset g_limiterPresets[kLimPresetCount] = {
  { "Game Asset -1 dBTP", { 0.0, -1.0, 5.0, 10.0, 60.0, true, true } },
  { "Master -0.3 dBTP",   { 0.0, -0.3, 2.0, 10.0, 200.0, true, true } },
  { "Brickwall 0 dBFS",   { 0.0, 0.0, 1.0, 10.0, 30.0, false, true } },
  { "Loud + Proud",       { 8.0, -1.0, 5.0, 10.0, 60.0, true, true } },
};

namespace {

// VK/char -> accepted edit character (copied from dynamics_panel.cpp Inc-8:
// macOS SWELL delivers ASCII punctuation, Windows delivers VK_OEM_*/numpad).
char MapEditChar(int vk)
{
  if (vk >= '0' && vk <= '9') return (char)vk;
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return (char)('0' + (vk - VK_NUMPAD0));
  if (vk == '.' || vk == VK_DECIMAL || vk == 0xBE) return '.';
  if (vk == '-' || vk == VK_SUBTRACT || vk == 0xBD) return '-';
  return 0;
}

// Locale-independent parse (never atof/strtod: a comma locale would misread
// "3.5"). Tolerant of a trailing unit ("12ms" -> 12).
double ParseEditValue(const char* s)
{
  while (*s == ' ') ++s;
  double sign = 1.0;
  if (*s == '-') { sign = -1.0; ++s; }
  else if (*s == '+') ++s;
  double val = 0.0;
  while (*s >= '0' && *s <= '9') { val = val * 10.0 + (double)(*s - '0'); ++s; }
  if (*s == '.') {
    ++s;
    double frac = 0.1;
    while (*s >= '0' && *s <= '9') { val += (double)(*s - '0') * frac; frac *= 0.1; ++s; }
  }
  return sign * val;
}

double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// --- Params by knob index -----------------------------------------------------

double LimiterPanel::GetValue(int idx) const
{
  switch (idx) {
    case 0: return m_params.gainDb;
    case 1: return m_params.ceilingDb;
    case 2: return m_params.attackMs;
    case 3: return m_params.holdMs;
    case 4: return m_params.releaseMs;
  }
  return 0.0;
}

void LimiterPanel::SetValue(int idx, double v)
{
  if (idx < 0 || idx >= kLimNumParams) return;
  v = ClampD(v, LIM_DEFS[idx].minVal, LIM_DEFS[idx].maxVal);
  switch (idx) {
    case 0: m_params.gainDb = v; break;
    case 1: m_params.ceilingDb = v; break;
    case 2: m_params.attackMs = v; break;
    case 3: m_params.holdMs = v; break;
    case 4: m_params.releaseMs = v; break;
  }
}

void LimiterPanel::SetParams(const LimiterParams& p)
{
  SetValue(0, p.gainDb);
  SetValue(1, p.ceilingDb);
  SetValue(2, p.attackMs);
  SetValue(3, p.holdMs);
  SetValue(4, p.releaseMs);
  m_params.truePeak = p.truePeak;
  m_params.link = p.link;
  m_presetIdx = -1;
}

void LimiterPanel::ApplyPreset(int idx)
{
  if (idx < 0 || idx >= kLimPresetCount) return;
  m_params = g_limiterPresets[idx].params;
  m_presetIdx = idx;
  m_paramsChanged = true;
}

void LimiterPanel::SetUserPresetName(const char* name)
{
  std::snprintf(m_userName, sizeof(m_userName), "%s", name ? name : "");
  m_presetIdx = m_userName[0] ? -2 : -1;
}

// --- Locale-safe params serialization (user-preset blob) -----------------------
// Same key=value shape as the dynamics preset strings, but values are x1000
// integers (the lim_* ExtState convention) so a comma decimal locale can
// never corrupt a round-trip. Key collision check: none of g/c/a/h/re/tp/lk
// appears as a substring of another key followed by '='.

void LimiterParamsToString(const LimiterParams& p, char* buf, int bufSize)
{
  std::snprintf(buf, (size_t)bufSize, "g=%d c=%d a=%d h=%d re=%d tp=%d lk=%d",
                (int)std::lround(p.gainDb * 1000.0),
                (int)std::lround(p.ceilingDb * 1000.0),
                (int)std::lround(p.attackMs * 1000.0),
                (int)std::lround(p.holdMs * 1000.0),
                (int)std::lround(p.releaseMs * 1000.0),
                p.truePeak ? 1 : 0, p.link ? 1 : 0);
}

bool LimiterParamsFromString(const char* str, LimiterParams& out)
{
  if (!str || !str[0]) return false;
  out = LimiterParams{};   // absent keys keep the defaults
  auto rd = [&](const char* key, double& val) {
    char search[8];
    std::snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(str, search);
    if (p) val = (double)atoi(p + strlen(search)) / 1000.0;
  };
  rd("g", out.gainDb);
  rd("c", out.ceilingDb);
  rd("a", out.attackMs);
  rd("h", out.holdMs);
  rd("re", out.releaseMs);
  double tp = 1000.0, lk = 1000.0;
  rd("tp", tp);
  rd("lk", lk);
  out.truePeak = tp >= 0.0005;   // x1000-decoded booleans: 1 -> 0.001
  out.link = lk >= 0.0005;
  return true;
}

// --- Lifecycle / geometry ------------------------------------------------------

void LimiterPanel::Show()
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  m_visible = true;
  m_statsValid = false;
  m_statsPending = true;
  m_hover = LIM_HIT_NONE;
#endif
}

void LimiterPanel::Hide()
{
  m_visible = false;
  m_dragKnob = -1;
  m_panelDragging = false;
  m_editIdx = -1;
  m_hover = LIM_HIT_NONE;
}

// g_uiScale, fit-clamped so the panel never outgrows the waveform rect (the
// SettingsPanel EffScale pattern; no per-panel resize grip in v1).
double LimiterPanel::EffScale(RECT wr) const
{
  double s = g_uiScale > 0.0 ? g_uiScale : 1.0;
  const double availW = (double)(wr.right - wr.left) - 8.0;
  const double availH = (double)(wr.bottom - wr.top) - 8.0;
  if (availW > 0.0 && availH > 0.0) {
    const double fitW = availW / (double)dynui::kLimPanelW;
    const double fitH = availH / (double)dynui::kLimPanelH;
    double cap = fitW < fitH ? fitW : fitH;
    if (cap < 0.5) cap = 0.5;
    if (s > cap) s = cap;
  }
  return s;
}

// Bottom-docked like the Dynamics panel (the GR overlay wants the waveform
// visible above it), draggable via offsets, clamped into the waveform rect.
RECT LimiterPanel::GetRect(RECT wr) const
{
  const double S = EffScale(wr);
  const int pw = (int)std::lround((double)dynui::kLimPanelW * S);
  const int ph = (int)std::lround((double)dynui::kLimPanelH * S);
  int cx = (wr.left + wr.right) / 2 + m_offsetX;
  int cy = wr.bottom - ph - 10 + m_offsetY;
  int left = cx - pw / 2;
  if (left < wr.left) left = wr.left;
  if (left + pw > wr.right) left = wr.right - pw;
  if (cy < wr.top) cy = wr.top;
  if (cy + ph > wr.bottom) cy = wr.bottom - ph;
  return { left, cy, left + pw, cy + ph };
}

bool LimiterPanel::HitTest(int x, int y, RECT wr) const
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  return x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom;
}

int LimiterPanel::HitId(double lx, double ly) const
{
  const LimiterLayout L =
      ComputeLimiterLayout((double)dynui::kLimPanelW, (double)dynui::kLimPanelH);
  if (L.closeBtn.contains(lx, ly)) return LIM_HIT_CLOSE;
  if (L.preset.contains(lx, ly))   return LIM_HIT_PRESET;
  if (L.apply.contains(lx, ly))    return LIM_HIT_APPLY;
  if (L.tpPill.contains(lx, ly))   return LIM_HIT_TP;
  if (!m_mono && L.linkPill.contains(lx, ly)) return LIM_HIT_LINK;
  for (int i = 0; i < kLimNumParams; ++i)
    if (L.knob[i].contains(lx, ly)) return LIM_HIT_KNOB0 + i;
  return LIM_HIT_NONE;
}

RECT LimiterPanel::GetPresetButtonRect(RECT wr) const
{
  const RECT pr = GetRect(wr);
  const double S = EffScale(wr);
  const LimiterLayout L =
      ComputeLimiterLayout((double)dynui::kLimPanelW, (double)dynui::kLimPanelH);
  return { pr.left + (int)std::lround(L.preset.x * S),
           pr.top + (int)std::lround(L.preset.y * S),
           pr.left + (int)std::lround((L.preset.x + L.preset.w) * S),
           pr.top + (int)std::lround((L.preset.y + L.preset.h) * S) };
}

// --- Mouse interaction ----------------------------------------------------------

bool LimiterPanel::OnMouseDown(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const double lx = (double)(x - pr.left) / S, ly = (double)(y - pr.top) / S;
  const LimiterLayout L =
      ComputeLimiterLayout((double)dynui::kLimPanelW, (double)dynui::kLimPanelH);

  if (L.closeBtn.contains(lx, ly)) { Hide(); return true; }
  if (L.preset.contains(lx, ly))   { m_presetMenuReq = true; return true; }
  if (L.apply.contains(lx, ly)) {
    if (m_applyPct < 0) m_applyRequested = true;   // ignored while running
    return true;
  }
  if (L.tpPill.contains(lx, ly)) {
    m_params.truePeak = !m_params.truePeak;
    m_paramsChanged = true;
    m_presetIdx = -1;
    return true;
  }
  if (!m_mono && L.linkPill.contains(lx, ly)) {
    m_params.link = !m_params.link;
    m_paramsChanged = true;
    m_presetIdx = -1;
    return true;
  }
  for (int i = 0; i < kLimNumParams; ++i) {
    if (!L.knob[i].contains(lx, ly)) continue;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {  // Cmd-click = reset default
      SetValue(i, LIM_DEFS[i].defaultVal);
      m_paramsChanged = true;
      m_presetIdx = -1;
      return true;
    }
    m_dragKnob = i;                 // velocity-sensitive vertical drag (dyn pattern)
    m_dragLastY = y;
    return true;                    // IsDragging() -> host SetCapture
  }

  // anywhere else on the panel body: drag the panel
  m_panelDragging = true;
  m_dragOffX = x - pr.left;
  m_dragOffY = y - pr.top;
  return true;
}

void LimiterPanel::OnMouseMove(int x, int y, RECT wr)
{
  if (m_dragKnob >= 0) {
    // Velocity-sensitive vertical knob drag - copied from DynamicsPanel so the
    // two panels feel identical (slow = fine, fast covers more range).
    const LimKnobDef& def = LIM_DEFS[m_dragKnob];
    const double range = (def.maxVal - def.minVal) != 0.0 ? (def.maxVal - def.minVal) : 1.0;
    const int dy = m_dragLastY - y;
    m_dragLastY = y;
    const int ady = dy < 0 ? -dy : dy;
    const double speed = ady > 40 ? 40.0 : (double)ady;
    const double gainPerPx = (1.0 / 240.0) * (1.0 + speed * 0.05);
    double nrm = (GetValue(m_dragKnob) - def.minVal) / range;
    nrm = ClampD(nrm + (double)dy * gainPerPx, 0.0, 1.0);
    SetValue(m_dragKnob, def.minVal + nrm * range);
    m_paramsChanged = true;
    m_presetIdx = -1;
    return;
  }
  if (m_panelDragging) {
    const RECT pr = GetRect(wr);
    const int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    const int defaultCX = (wr.left + wr.right) / 2;
    const int defaultCY = wr.bottom - ph - 10;
    const int newLeft = x - m_dragOffX;
    const int newTop = y - m_dragOffY;
    m_offsetX = (newLeft + pw / 2) - defaultCX;
    m_offsetY = newTop - defaultCY;
    m_geomChanged = true;           // host persists on mouse-up
  }
}

void LimiterPanel::OnMouseUp()
{
  m_dragKnob = -1;
  m_panelDragging = false;
}

bool LimiterPanel::OnHover(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  int h = LIM_HIT_NONE;
  const RECT pr = GetRect(wr);
  if (x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom) {
    const double S = EffScale(wr);
    h = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  }
  if (h == m_hover) return false;
  m_hover = h;
  return true;
}

bool LimiterPanel::OnMouseWheel(int x, int y, double steps, bool fine, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const int hit = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  if (hit < LIM_HIT_KNOB0) return false;
  const int i = hit - LIM_HIT_KNOB0;
  const LimKnobDef& def = LIM_DEFS[i];
  const double range = (def.maxVal - def.minVal) != 0.0 ? (def.maxVal - def.minVal) : 1.0;
  double nrm = (GetValue(i) - def.minVal) / range;
  nrm = ClampD(nrm + (fine ? 0.005 : 0.025) * steps, 0.0, 1.0);
  SetValue(i, def.minVal + nrm * range);
  m_paramsChanged = true;
  m_presetIdx = -1;
  return true;
}

// --- Inline type-value editor -----------------------------------------------

void LimiterPanel::BeginValueEdit(int idx)
{
  if (idx < 0 || idx >= kLimNumParams) return;
  m_paramsChanged = false;   // editor is a clean transaction (dyn Inc-8 rule)
  m_editIdx = idx;
  m_editFresh = true;
  std::snprintf(m_editBuf, sizeof(m_editBuf),
                LIM_DEFS[idx].precision == 1 ? "%.1f" : "%.0f", GetValue(idx));
  for (char* s = m_editBuf; *s; ++s)
    if (*s == ',') *s = '.';   // locale comma -> '.' (we only accept '.')
}

bool LimiterPanel::CommitValueEdit()
{
  if (m_editIdx < 0) return false;
  const int idx = m_editIdx;
  m_editIdx = -1;
  bool hasDigit = false;
  for (const char* s = m_editBuf; *s; ++s)
    if (*s >= '0' && *s <= '9') { hasDigit = true; break; }
  if (!hasDigit) return false;   // nothing usable -> cancel, not snap-to-0
  const double oldVal = GetValue(idx);
  SetValue(idx, ParseEditValue(m_editBuf));
  const bool changed = GetValue(idx) != oldVal;
  if (changed) { m_paramsChanged = true; m_presetIdx = -1; }
  return changed;
}

bool LimiterPanel::OnDoubleClick(int x, int y, RECT wr)
{
  if (!m_visible) return false;
  const RECT pr = GetRect(wr);
  if (x < pr.left || x >= pr.right || y < pr.top || y >= pr.bottom) return false;
  const double S = EffScale(wr);
  const int hit = HitId((double)(x - pr.left) / S, (double)(y - pr.top) / S);
  if (hit >= LIM_HIT_KNOB0) { BeginValueEdit(hit - LIM_HIT_KNOB0); return true; }
  return false;
}

bool LimiterPanel::OnEditKey(int vk)
{
  if (m_editIdx < 0) return false;
  if (vk == VK_RETURN) { CommitValueEdit(); return true; }
  if (vk == VK_ESCAPE) { CancelValueEdit(); return true; }
  if (vk == VK_BACK) {
    m_editFresh = false;
    const size_t n = strlen(m_editBuf);
    if (n > 0) m_editBuf[n - 1] = '\0';
    return true;
  }
  const char c = MapEditChar(vk);
  if (c) {
    if (m_editFresh) { m_editBuf[0] = '\0'; m_editFresh = false; }
    const size_t n = strlen(m_editBuf);
    if (n < sizeof(m_editBuf) - 1) { m_editBuf[n] = c; m_editBuf[n + 1] = '\0'; }
  }
  return true;   // trap every key while editing
}

// --- Preview stats + paint -----------------------------------------------------

void LimiterPanel::SetPreviewStats(double inDb, double outDb, double grDb,
                                   bool outPending)
{
  m_inDb = inDb;
  m_outDb = outDb;
  m_grDb = grDb;
  m_outPending = outPending;
  m_statsValid = true;
  m_statsPending = false;
}

void LimiterPanel::DrawPremium(HDC hdc, RECT wr, double dpr)
{
#ifdef SNEAKPEAK_BLEND2D_PANEL
  if (!m_visible) return;
  const RECT pr = GetRect(wr);

  LimiterVM vm;
  vm.presetName = (m_presetIdx >= 0 && m_presetIdx < kLimPresetCount)
                      ? g_limiterPresets[m_presetIdx].name
                      : (m_presetIdx == -2 && m_userName[0]) ? m_userName
                                                             : nullptr;
  vm.truePeak = m_params.truePeak;
  vm.link = m_params.link;
  vm.showLink = !m_mono;
  vm.hover = m_hover;

  for (int i = 0; i < kLimNumParams; ++i) {
    const LimKnobDef& def = LIM_DEFS[i];
    const double range = (def.maxVal - def.minVal) != 0.0 ? (def.maxVal - def.minVal) : 1.0;
    KnobVM& k = vm.knobs[i];
    k.value = GetValue(i);
    k.norm = ClampD((k.value - def.minVal) / range, 0.0, 1.0);
    k.defaultNorm = ClampD((def.defaultVal - def.minVal) / range, 0.0, 1.0);
    k.label = def.label;
    k.unit = (i == 1) ? (m_params.truePeak ? "dBTP" : "dBFS") : def.unit;
    k.precision = def.precision;
    k.hover = (m_hover == LIM_HIT_KNOB0 + i) || m_dragKnob == i;
    k.editing = (m_editIdx == i);
    k.editText = m_editBuf;
    k.caretOn = true;   // non-blinking caret (no animation pump for this panel)
  }

  const char* peakUnit = m_params.truePeak ? "dBTP" : "dBFS";
  if (m_statsPending) {
    if (m_statsPct >= 0)
      std::snprintf(m_inText, sizeof(m_inText), "%d%%", m_statsPct);
    else
      std::snprintf(m_inText, sizeof(m_inText), "...");
    std::snprintf(m_outText, sizeof(m_outText), "...");
    std::snprintf(m_grText, sizeof(m_grText), "...");
    vm.grNorm = 0.0;
  } else if (m_statsValid) {
    if (m_inDb <= -900.0) std::snprintf(m_inText, sizeof(m_inText), "-inf");
    else std::snprintf(m_inText, sizeof(m_inText), "%.1f %s", m_inDb, peakUnit);
    if (m_outPending) std::snprintf(m_outText, sizeof(m_outText), "...");
    else if (m_outDb <= -900.0) std::snprintf(m_outText, sizeof(m_outText), "-inf");
    else std::snprintf(m_outText, sizeof(m_outText), "%.1f %s", m_outDb, peakUnit);
    std::snprintf(m_grText, sizeof(m_grText), "%.1f dB", m_grDb);
    vm.grNorm = ClampD(m_grDb / 24.0, 0.0, 1.0);
  } else {
    std::snprintf(m_inText, sizeof(m_inText), "-");
    std::snprintf(m_outText, sizeof(m_outText), "-");
    std::snprintf(m_grText, sizeof(m_grText), "-");
    vm.grNorm = 0.0;
  }
  vm.inText = m_inText;
  vm.outText = m_outText;
  vm.grText = m_grText;
  vm.applyPct = m_applyPct;
  vm.itemDestructive = m_itemMode;

  m_canvas.RenderLimiterPanel(hdc, pr.left, pr.top, pr.right - pr.left,
                              pr.bottom - pr.top, dpr, vm);
#else
  (void)hdc; (void)wr; (void)dpr;
#endif
}
