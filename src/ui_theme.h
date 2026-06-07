// ============================================================================
// ui_theme.h — Premium UI design tokens for the Blend2D-rendered panels
//
// The dark, FabFilter-flavoured design system for the redesigned Dynamics Panel
// (and future Blend2D panels). Distinct from theme.h (which mirrors REAPER's
// active theme as COLORREF); these are a fixed, curated design language.
//
// Values are 0xAARRGGBB, ready for Blend2D's BLRgba32(...). Full spec + rationale
// (contrast ratios, usage discipline) in .harness/design_dynamics_panel.md.
//
// Discipline: AMBER = live/active. RED = gain reduction ONLY. VIOLET = gate.
// CYAN = volume envelope. Everything else neutral grey. No other saturated hues.
// ============================================================================
#pragma once

#include <cstdint>

namespace dynui {

// --- Surfaces (cool near-black, never #000) ---
constexpr uint32_t kBgShadow   = 0xFF08090B;  // drop-shadow ink, bezel hairline
constexpr uint32_t kSurface0   = 0xFF0E1116;  // panel base
constexpr uint32_t kSurface1   = 0xFF14181E;  // active tab body / plot well
constexpr uint32_t kSurface2   = 0xFF1B212A;  // control tracks, cards
constexpr uint32_t kSurface3   = 0xFF232B36;  // hovered/active control, segment fill

// --- Hairlines / translucent overlays ---
constexpr uint32_t kHairline   = 0x0FFFFFFF;  // ~6% white: dividers, top highlight
constexpr uint32_t kCurveUnity = 0x1AFFFFFF;  // ~10% white: 1:1 diagonal reference

// --- Ink (opacity tiers of one off-white, not many greys) ---
constexpr uint32_t kInkPrimary   = 0xFFECEFF3;  // value readouts (~13:1)
constexpr uint32_t kInkSecondary = 0xFFA7B0BC;  // parameter labels (~6.5:1)
constexpr uint32_t kInkMuted     = 0xFF6B7480;  // units, ticks (graphic/large only)
constexpr uint32_t kInkDisabled  = 0xFF444C57;  // disabled controls

// --- Accent (the single signature hue: live / active / focused) ---
constexpr uint32_t kAmber      = 0xFFF5A623;  // active tab, focused fill/handle, live point
constexpr uint32_t kAmberGlow  = 0xFFFFC861;  // glow core / hover brighten

// --- Functional hues (each owns exactly one data layer; grayscale-distinct) ---
constexpr uint32_t kGrRed      = 0xFFE5484D;  // gain reduction ONLY
constexpr uint32_t kGateViolet = 0xFFB57BD6;  // gate region / cliff / handles
constexpr uint32_t kEnvCyan    = 0xFF3CC8DC;  // volume envelope overlay ONLY
constexpr uint32_t kCurveStatic = 0xFFC3CAD2; // static transfer curve at rest

// --- Layout scale (logical px; multiply by device-pixel-ratio at draw time) ---
constexpr int   kGrid        = 8;    // base spacing unit (4px half-step allowed)
constexpr int   kPanelPad    = 16;
constexpr int   kRadiusPanel = 12;
constexpr int   kRadiusCtrl  = 6;
constexpr int   kHeaderH     = 44;
constexpr int   kTabBarH     = 36;
constexpr int   kFooterH     = 44;
constexpr int   kRowH        = 40;   // parameter row (replaces legacy ROW_H 18)
constexpr int   kKnobDia     = 48;
constexpr int   kHitMin      = 44;   // minimum interactive hit target

// --- transfer plot / GR meter render constants (Phase 2 Inc 2; replace magic numbers) ---
constexpr int   kPlotSamples = 96;   // transfer-curve sampling resolution
constexpr int   kTickStepDb  = 12;   // dB spacing for input-axis tick labels
constexpr int   kMeterBarW   = 18;   // GR meter bar width
constexpr int   kMeterGap    = 14;   // gap from plot right edge to the meter bar
constexpr int   kMeterNumGap = 12;   // gap from the meter bar to the numeric column

// --- Type scale (logical px) ---
constexpr float kFsValue     = 16.0f;  // value readouts (tabular)
constexpr float kFsGrHero    = 20.0f;  // big GR readout
constexpr float kFsLabel     = 12.0f;  // parameter labels (uppercase) - HARD FLOOR
constexpr float kFsTab       = 13.0f;  // tab labels (uppercase)
constexpr float kFsUnit      = 11.0f;  // units (dB, ms, :1)
constexpr float kFsTick      = 10.0f;  // plot axis ticks

} // namespace dynui
