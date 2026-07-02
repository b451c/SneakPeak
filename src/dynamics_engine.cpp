// dynamics_engine.cpp — Professional dynamics processor
// Standard compressor: threshold + ratio + soft knee + attack/release + makeup
#include "dynamics_engine.h"
#include "deess_engine.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>

static constexpr double STEP_SIZE = 0.001; // 1ms analysis windows
static constexpr double EPSILON = 1e-12;

// --- Built-in presets (researched from iZotope, Waves, EBU R128, BBC guidelines) ---

// The three gate-enabled voice presets ship -6 dB hysteresis (v2.3.0): a voice
// gate without hysteresis chatters on breathy material hovering at the threshold
// (LSP Gate defaults -12 relative, cycfi Q +12 gap - both on raw input; ours
// detects post-comp, hence the smaller heuristic). Rows list all 19 fields then;
// the gate-off rows keep 15 values + trailing NSDMI legacy defaults.
const DynamicsPreset g_dynamicsPresets[PRESET_COUNT] = {
  { "Default", { 0.0, 1.0, 0.0, 0.0, false, 10.0, 100.0, 0.0, false, 5.0, -100.0, -20.0, 50.0, -60.0, 6.0 } },
  { "Gentle Leveling", { -18.0, 2.0, 12.0, 0.0, true, 20.0, 150.0, 3.0, true, 5.0, -100.0, -40.0, 50.0, -60.0, 6.0 } },
  { "Voice / Podcast", { -24.0, 4.0, 6.0, 0.0, true, 5.0, 80.0, 5.0, true, 5.0, -45.0, -18.0, 80.0, -60.0, 6.0, 2.0, -6.0, 2.0, 100.0 } },
  { "Broadcast", { -20.0, 6.0, 3.0, 0.0, true, 1.0, 50.0, 5.0, true, 5.0, -40.0, -24.0, 60.0, -60.0, 6.0, 2.0, -6.0, 2.0, 100.0 } },
  { "De-breath", { -18.0, 2.0, 8.0, 0.0, true, 10.0, 100.0, 5.0, false, 5.0, -35.0, -12.0, 30.0, -60.0, 6.0, 2.0, -6.0, 2.0, 100.0 } },
  { "Music Bus", { -16.0, 2.0, 9.0, 0.0, true, 30.0, 200.0, 0.0, true, 5.0, -100.0, -40.0, 50.0, -60.0, 6.0 } },
  // v2.3.0 Up-mode showcase: gentle RMS leveling with the gate ON (-50/-40,
  // -6 hyst) so the boosted floor never pumps noise; auto-makeup OFF (Up
  // convention: makeup acts as a trim, user opts in). 21 positional values.
  { "Upward Leveling", { -100.0, 2.0, 12.0, 0.0, false, 20.0, 150.0, 0.0, true, 5.0, -50.0, -40.0, 50.0, -60.0, 6.0, 2.0, -6.0, 2.0, 100.0, true, 8.0 } },
  // v2.3.0 INC-3: Voice/Podcast compressor + wideband de-esser at the research
  // defaults (6 kHz BP, 4:1, -10 dB range, 1/60 ms - all NSDMI, only dsEnable
  // is set). 24 positional values: 21 as above + the two ephemeral bypasses
  // (false, positional necessity - never serialized) + dsEnable.
  { "De-Ess Vocal", { -24.0, 4.0, 6.0, 0.0, true, 5.0, 80.0, 5.0, true, 5.0, -45.0, -18.0, 80.0, -60.0, 6.0, 2.0, -6.0, 2.0, 100.0, 0, 8.0, false, false, true } },
};

// --- P_EXT serialization ---
// INVARIANT: FromString matches keys with a naive strstr("key="), so every NEW
// key must be (a) appended AFTER all existing keys and (b) named so that no
// earlier "key=" pattern occurs inside it before its own position (e.g. "gre="
// contains "re=", which is safe ONLY because the legacy "re=" is serialized
// first). Repeat this collision analysis for every key added here.

void DynamicsParamsToString(const DynamicsParams& p, char* buf, int bufSize)
{
  // ds* collision analysis (v2.3.0 INC-3, per the invariant above): every ds
  // key is appended AFTER all legacy keys, so the embedded legacy patterns
  // ("t=" in "dst=", "r=" in "dsr=", "a=" in "dsa=", "m=" in "dsm=", "re=" in
  // "dsre=") always strstr-match their legacy occurrence first - in BOTH this
  // parser and old binaries reading new strings (graceful ignore). Within the
  // ds family no key's "X=" pattern occurs inside another ds key ("dsre="
  // contains "dsr"+"e", never "dsr="), so ds-vs-ds order is also safe.
  snprintf(buf, bufSize,
    "t=%.1f r=%.1f k=%.1f m=%.1f am=%d a=%.1f re=%.1f la=%.1f rms=%d "
    "gt=%.1f gr=%.1f gh=%.1f gx=%.1f ghy=%.1f gat=%.1f gre=%.1f up=%d mb=%.1f "
    "dse=%d dsm=%d dsf=%.0f dsq=%.2f dst=%.1f dsr=%.1f dsx=%.1f dsa=%.1f dsre=%.1f",
    p.threshold, p.ratio, p.kneeDb, p.makeupDb, p.autoMakeup ? 1 : 0,
    p.attackMs, p.releaseMs, p.lookaheadMs, p.rmsMode ? 1 : 0,
    p.gateThreshDb, p.gateRangeDb, p.gateHoldMs,
    p.gateRatio, p.gateHystDb, p.gateAttackMs, p.gateReleaseMs,
    p.compMode, p.maxBoostDb,
    p.dsEnable ? 1 : 0, p.dsMode, p.dsFreqHz, p.dsQ, p.dsThreshDb,
    p.dsRatio, p.dsRangeDb, p.dsAttackMs, p.dsReleaseMs);
}

bool DynamicsParamsFromString(const char* str, DynamicsParams& out)
{
  if (!str || !str[0]) return false;
  out = DynamicsParams{}; // start with defaults
  auto readKey = [&](const char* key, double& val) {
    char search[16];
    snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(str, search);
    if (p) val = atof(p + strlen(search));
  };
  double am = 1.0, rms = 0.0;
  readKey("t", out.threshold);
  readKey("r", out.ratio);
  readKey("k", out.kneeDb);
  readKey("m", out.makeupDb);
  readKey("am", am); out.autoMakeup = (am > 0.5);
  readKey("a", out.attackMs);
  readKey("re", out.releaseMs);
  readKey("la", out.lookaheadMs);
  readKey("rms", rms); out.rmsMode = (rms > 0.5);
  readKey("gt", out.gateThreshDb);
  readKey("gr", out.gateRangeDb);
  readKey("gh", out.gateHoldMs);
  // v2.3.0 gate extension - absent in old strings, so the NSDMI defaults
  // (2.0 / 0.0 / 2.0 / 100.0) reproduce the legacy hard-coded behavior.
  readKey("gx", out.gateRatio);
  readKey("ghy", out.gateHystDb);
  readKey("gat", out.gateAttackMs);
  readKey("gre", out.gateReleaseMs);
  // v2.3.0 upward compression ("up=" / "mb=" have no earlier key= collisions;
  // legacy "m=" matches first at its original position).
  double up = 0.0;
  readKey("up", up);
  out.compMode = (int)(up + 0.5);
  if (out.compMode < 0 || out.compMode > 2) out.compMode = 0;
  readKey("mb", out.maxBoostDb);
  // v2.3.0 INC-3 de-esser - absent in old strings -> NSDMI defaults (off).
  // Collision analysis at ToString: legacy patterns embedded in ds keys always
  // match their legacy occurrence first.
  double dse = 0.0, dsm = 0.0;
  readKey("dse", dse); out.dsEnable = (dse > 0.5);
  readKey("dsm", dsm);
  out.dsMode = (dsm > 0.5) ? DEESS_MODE_HIGHPASS : DEESS_MODE_BANDPASS;
  readKey("dsf", out.dsFreqHz);
  readKey("dsq", out.dsQ);
  readKey("dst", out.dsThreshDb);
  readKey("dsr", out.dsRatio);
  readKey("dsx", out.dsRangeDb);
  readKey("dsa", out.dsAttackMs);
  readKey("dsre", out.dsReleaseMs);
  return true;
}

void DynamicsEngine::Analyze(const double* audioData, int numFrames, int numChannels,
                             int sampleRate, double itemVolDb, const DynamicsParams& params)
{
  m_params = params;
  m_results.clear();
  m_rawPeaks.clear();
  m_avgGR = 0.0;
  m_itemVolDb = itemVolDb;

  if (!audioData || numFrames <= 0 || sampleRate <= 0) return;

  CollectPeaks(audioData, numFrames, numChannels, sampleRate,
               params.rmsMode, params.rmsWindowMs);
  if (m_rawPeaks.empty()) return;

  // Average peak dB (for auto-threshold when sentinel -100)
  double sumPeak = 0.0;
  for (const auto& p : m_rawPeaks) sumPeak += p.peak;
  double meanLinear = sumPeak / (double)m_rawPeaks.size();
  m_avgPeakDb = 20.0 * log10(std::max(meanLinear, EPSILON)) + itemVolDb;

  BuildEnvelope(itemVolDb, params);

  // De-esser band trace (v2.3.0 INC-3). Cache-keyed: Live re-analyzes on every
  // knob tick, but the trace depends only on the audio and (mode, f0, Q) -
  // threshold/ratio/attack/release tweaks reuse it (load-bearing for Live
  // CPU). The sparse FNV-1a content hash catches same-length destructive
  // edits (e.g. normalize) that the dimensions alone would miss.
  if (params.dsEnable) {
    BandTraceKey key;
    key.numFrames = numFrames;
    key.numChannels = numChannels;
    key.sampleRate = sampleRate;
    key.mode = params.dsMode;
    key.freqHz = params.dsFreqHz;
    key.q = params.dsQ;
    unsigned long long h = 1469598103934665603ULL;
    const size_t totalSamples = (size_t)numFrames * (size_t)std::max(1, numChannels);
    for (size_t i = 0; i < totalSamples; i += 4096) {
      unsigned long long bits;
      memcpy(&bits, &audioData[i], sizeof(bits));
      h = (h ^ bits) * 1099511628211ULL;
    }
    key.contentHash = h;

    if (!(key == m_bandKey) || m_bandPeaks.size() != m_rawPeaks.size()) {
      DeEssBandTrace(audioData, numFrames, numChannels, sampleRate, STEP_SIZE,
                     params.dsMode, params.dsFreqHz, params.dsQ, m_bandPeaks);
      m_bandKey = key;
    }

    // Auto-threshold reference: mean band level (mirrors m_avgPeakDb).
    double sum = 0.0;
    for (double v : m_bandPeaks) sum += v;
    double mean = m_bandPeaks.empty() ? 0.0 : sum / (double)m_bandPeaks.size();
    m_avgBandDb = 20.0 * log10(std::max(mean, EPSILON)) + itemVolDb;
  }
}

// Stage 1: Collect peaks from audio buffer in 1ms windows
void DynamicsEngine::CollectPeaks(const double* audioData, int numFrames, int numChannels,
                                  int sampleRate, bool rmsMode, double rmsWindowMs)
{
  int samplesPerStep = std::max(1, (int)(STEP_SIZE * sampleRate));
  int nch = std::max(1, numChannels);

  // RMS: wider window for averaging (default 5ms, but never smaller than step)
  int rmsWindow = rmsMode
    ? std::max(samplesPerStep, (int)(rmsWindowMs / 1000.0 * sampleRate))
    : 0;

  m_rawPeaks.reserve((size_t)(numFrames / samplesPerStep + 1));

  for (int frame = 0; frame < numFrames; frame += samplesPerStep) {
    int windowEnd = std::min(numFrames, frame + samplesPerStep);
    double value = 0.0;

    if (rmsMode) {
      // RMS: root mean square over rmsWindow centered on this step
      int rmsStart = std::max(0, frame - rmsWindow / 2);
      int rmsEnd = std::min(numFrames, rmsStart + rmsWindow);
      double sumSq = 0.0;
      int count = 0;
      for (int s = rmsStart; s < rmsEnd; s++) {
        for (int ch = 0; ch < nch; ch++) {
          double v = audioData[(size_t)s * nch + ch];
          sumSq += v * v;
        }
        count++;
      }
      value = (count > 0) ? sqrt(sumSq / (double)(count * nch)) : 0.0;
    } else {
      // Peak: max(abs) per channel
      for (int s = frame; s < windowEnd; s++) {
        for (int ch = 0; ch < nch; ch++) {
          double v = fabs(audioData[(size_t)s * nch + ch]);
          if (v > value) value = v;
        }
      }
    }

    double time = (double)frame / (double)sampleRate;
    m_rawPeaks.push_back({time, value});
  }
}

// Stage 2: Asymmetric attack/release envelope follower with lookahead
void DynamicsEngine::BuildEnvelope(double itemVolDb, const DynamicsParams& params)
{
  if (m_rawPeaks.empty()) return;

  double dt = (m_rawPeaks.size() > 1)
    ? (m_rawPeaks[1].time - m_rawPeaks[0].time) : STEP_SIZE;

  double attCoeff = (params.attackMs > 0.0)
    ? exp(-dt / (params.attackMs / 1000.0)) : 0.0;
  double relCoeff = (params.releaseMs > 0.0)
    ? exp(-dt / (params.releaseMs / 1000.0)) : 0.0;

  int lookaheadFrames = (params.lookaheadMs > 0.0)
    ? (int)(params.lookaheadMs / 1000.0 / dt + 0.5) : 0;

  int count = (int)m_rawPeaks.size();
  m_results.resize((size_t)count);

  double env = m_rawPeaks[0].peak;

  for (int i = 0; i < count; i++) {
    double x = m_rawPeaks[i].peak;

    if (lookaheadFrames > 0) {
      int ahead_end = std::min(count, i + 1 + lookaheadFrames);
      for (int j = i + 1; j < ahead_end; j++) {
        if (m_rawPeaks[j].peak > x) x = m_rawPeaks[j].peak;
      }
    }

    if (x >= env) {
      env = attCoeff * env + (1.0 - attCoeff) * x;
    } else {
      env = relCoeff * env + (1.0 - relCoeff) * x;
    }

    double db = 20.0 * log10(std::max(env, EPSILON)) + itemVolDb;
    double norm = (db - params.minDb) / (params.maxDb - params.minDb);
    norm = std::max(0.0, std::min(1.0, norm));

    m_results[i] = {m_rawPeaks[i].time, m_rawPeaks[i].peak, db, norm, 0.0};
  }
}

double DynamicsEngine::GetThreshold() const
{
  return (m_params.threshold <= -99.0) ? m_avgPeakDb : m_params.threshold;
}

// Stage 3: Gain-smoothing compressor model (industry standard).
// Instant GR computed from raw peaks, then attack/release smooth the GR signal.
// This matches FabFilter Pro-C, Waves, UAD, ReaComp behavior.
std::vector<DynamicsEngine::CompressPoint> DynamicsEngine::ComputeCompression()
{
  std::vector<CompressPoint> out;
  if (m_results.empty()) return out;

  double thresh = GetThreshold();
  double knee = std::max(0.0, m_params.kneeDb);
  // Extended ratio encoding (v2.3.0): >=1 classic (bit-identical), 0 = Inf:1,
  // negative = over-compression. See DynSlopeFromRatio in the header.
  double slope = DynSlopeFromRatio(m_params.ratio);
  double halfKnee = knee / 2.0;
  // 0=Down, 1=Up, 2=Both (leveler: both contributions summed per point)
  const int compMode = std::min(2, std::max(0, m_params.compMode));
  const bool hasDown = (compMode != COMP_MODE_UP);
  const bool hasUp   = (compMode != COMP_MODE_DOWN);

  // Attack/release coefficients for GR smoothing
  double dt = (m_results.size() > 1)
    ? (m_results[1].time - m_results[0].time) : STEP_SIZE;
  double attCoeff = (m_params.attackMs > 0.0)
    ? exp(-dt / (m_params.attackMs / 1000.0)) : 0.0;
  double relCoeff = (m_params.releaseMs > 0.0)
    ? exp(-dt / (m_params.releaseMs / 1000.0)) : 0.0;

  // Lookahead: scan N frames ahead for max peak before computing instant GR.
  // This lets the compressor "see" transients before they arrive.
  int lookaheadFrames = (m_params.lookaheadMs > 0.0 && dt > 0.0)
    ? (int)(m_params.lookaheadMs / 1000.0 / dt + 0.5) : 0;

  // Gate parameters (detects on the post-compression, PRE-makeup level - see the second pass)
  bool gateEnabled = (m_params.gateThreshDb > -99.0) && !m_params.gateBypass;
  double gateRange = std::min(0.0, m_params.gateRangeDb); // always <= 0
  double gateThresh = m_params.gateThreshDb;
  // v2.3.0 gate extension: generalized downward expander with Schmitt hysteresis.
  // Static curve (textbook/Calf convention): output slope Rg:1 below the OPEN
  // threshold, i.e. GR = (Rg - 1) * (L - tOpen), clamped to gateRange. Rg = 2
  // reproduces the legacy fixed 2:1 expander bit-for-bit. Hysteresis (cycfi Q /
  // LSP model): gate OPENS at tOpen, stays open down to tClose = tOpen + H
  // (H <= 0); H = 0 degenerates to the legacy single-threshold behavior.
  double gateSlope = std::max(1.0, m_params.gateRatio) - 1.0;
  double tOpen = gateThresh;
  double tClose = tOpen + std::min(0.0, m_params.gateHystDb);
  // Gate smoothing: user-set open/close speeds (legacy constants 2 ms / 100 ms)
  double gateAttCoeff = (m_params.gateAttackMs > 0.0)
    ? exp(-dt / (m_params.gateAttackMs / 1000.0)) : 0.0;
  double gateRelCoeff = (m_params.gateReleaseMs > 0.0)
    ? exp(-dt / (m_params.gateReleaseMs / 1000.0)) : 0.0;
  int gateHoldFrames = (m_params.gateHoldMs > 0.0 && dt > 0.0)
    ? (int)(m_params.gateHoldMs / 1000.0 / dt + 0.5) : 0;

  out.reserve(m_results.size());
  double grSum = 0.0;
  int grCount = 0;
  double smoothGR = 0.0; // smoothed compressor gain reduction (dB, <=0)
  double smoothGateGR = 0.0; // smoothed gate gain reduction (dB, <=0)
  int gateHoldCounter = 0;   // hold timer (frames remaining)
  int n = (int)m_results.size();

  // First pass: compute compressor GR + auto-makeup (needed before gate)
  std::vector<double> compGRs(n);
  for (int i = 0; i < n; i++) {
    auto& pt = m_results[i];

    // With lookahead: use max peak from current position to lookahead window
    double peakVal = pt.peakLinear;
    if (lookaheadFrames > 0) {
      int end = std::min(n, i + 1 + lookaheadFrames);
      for (int j = i + 1; j < end; j++) {
        if (m_results[j].peakLinear > peakVal)
          peakVal = m_results[j].peakLinear;
      }
    }

    // Instant gain computation from peak (with lookahead)
    double rawDb = 20.0 * log10(std::max(peakVal, EPSILON)) + m_itemVolDb;
    double overshoot = rawDb - thresh;
    double instantGR = 0.0;

    // BOTH knee semantics: same-center knees would cancel EXACTLY (the summed
    // quadratics are linear in x for ANY width - a dead knob), so in Both the
    // two sides shift by +-halfKnee. The shifted parabolas meet at the
    // threshold with zero gain: Knee becomes the gentle "leave-alone" band
    // around the target (W = 0 degenerates to the pure linear leveler).
    const double oDn = (compMode == COMP_MODE_BOTH) ? overshoot - halfKnee : overshoot;
    const double oUp = (compMode == COMP_MODE_BOTH) ? overshoot + halfKnee : overshoot;
    if (hasDown && !m_params.compBypass) {
      // Classic downward: reduce above the threshold (JAES 2012 Eq. 4).
      if (knee > 0.0 && oDn > -halfKnee && oDn < halfKnee) {
        double x = oDn + halfKnee;
        instantGR += slope * x * x / (2.0 * knee);
      } else if (oDn > 0.0) {
        instantGR += slope * oDn;
      }
    }
    if (hasUp && !m_params.compBypass) {
      // Upward (v2.3.0): point reflection of the downward gain computer about
      // the threshold - boost below, unity above, C1 at both knee edges
      // (research_v230 'Upward Compression', reviewer-verified derivation).
      // In Both mode the two knee quadratics SUM to an exactly linear
      // S*(x - T) through the threshold - a seamless leveler.
      double gUp = 0.0;
      if (knee > 0.0 && oUp > -halfKnee && oUp < halfKnee) {
        double x = oUp - halfKnee;                       // mirrored knee quadratic
        gUp = -slope * x * x / (2.0 * knee);
      } else if (oUp < 0.0) {
        gUp = slope * oUp;                               // slope<=0, below => boost
      }
      // Mandatory cap - uncapped, the curve boosts the noise floor without bound.
      gUp = std::min(gUp, m_params.maxBoostDb);
      // Gate coupling: the boost is floored at the gate threshold, compared on
      // the SAME level the gate state machine detects on (raw peak WITHOUT
      // lookahead) so "the gate always wins" holds exactly; lookahead keeps
      // driving only the early REMOVAL of boost before transients.
      if (gateEnabled) {
        double rawNoLaDb = 20.0 * log10(std::max(pt.peakLinear, EPSILON)) + m_itemVolDb;
        if (rawNoLaDb < gateThresh) gUp = 0.0;
      }
      instantGR += gUp;
    }

    // Smooth compressor GR (sign-agnostic branch: works for boost too)
    if (instantGR < smoothGR) {
      smoothGR = attCoeff * smoothGR + (1.0 - attCoeff) * instantGR;
    } else {
      smoothGR = relCoeff * smoothGR + (1.0 - relCoeff) * instantGR;
    }

    compGRs[i] = smoothGR;
    // fabs: in Up mode the average must accumulate positive (boost) GR too;
    // in Down mode smoothGR <= 0 always, so this is identical to `< -0.01`.
    if (fabs(smoothGR) > 0.01) { grSum += smoothGR; grCount++; }
  }

  m_avgGR = (grCount > 0) ? grSum / (double)grCount : 0.0;
  // compBypass silences the WHOLE comp stage - makeup is its output gain.
  double makeup = m_params.compBypass ? 0.0
                : m_params.autoMakeup ? -m_avgGR : m_params.makeupDb;
  // Extended-ratio zone (slope < -1, Inf/over-comp): cap auto-makeup at the
  // manual knob ceiling - over-compression GR averages can be huge and -avgGR
  // would explode the output (v2.3 addendum: cap auto-makeup in that zone).
  if (slope < -1.0 && makeup > 24.0) makeup = 24.0;

  // De-ess pass (v2.3.0 INC-3): the band-filtered sidechain (m_bandPeaks,
  // 1:1 with the results grid) drives a third GR component with its own gain
  // computer and attack/release smoothing; dB GRs sum because the envelope
  // multiplies linearly. De-ess reduction is deliberate signal shaping, so it
  // does NOT feed auto-makeup (m_avgGR stays comp-only) and the gate keeps
  // detecting on rawDb + compGR (a band event and a silence detector are
  // orthogonal - research_v230 "De-esser").
  std::vector<double> dsGRs;
  m_avgDsGR = 0.0;
  const bool dsOn = m_params.dsEnable && m_bandPeaks.size() == m_results.size();
  if (dsOn) {
    dsGRs.resize((size_t)n);
    const double dsThresh = GetDeEssThreshold();
    const double dsSlope = 1.0 / std::max(1.0, m_params.dsRatio) - 1.0;
    // Small fixed soft knee: sibilance wants decisive engagement, but a hard
    // corner modulates audibly right at the threshold. No knob (research
    // param table) - 2 dB engineering constant, C1 like the comp knee.
    const double dsKnee = 2.0, dsHalfKnee = dsKnee / 2.0;
    const double dsRange = std::min(0.0, m_params.dsRangeDb);
    const double dsAtt = (m_params.dsAttackMs > 0.0)
      ? exp(-dt / (m_params.dsAttackMs / 1000.0)) : 0.0;
    const double dsRel = (m_params.dsReleaseMs > 0.0)
      ? exp(-dt / (m_params.dsReleaseMs / 1000.0)) : 0.0;
    double smoothDsGR = 0.0, dsSum = 0.0;
    int dsCount = 0;
    for (int i = 0; i < n; i++) {
      double bandDb =
        20.0 * log10(std::max(m_bandPeaks[(size_t)i], EPSILON)) + m_itemVolDb;
      double overshoot = bandDb - dsThresh;
      double g = 0.0;
      if (overshoot > -dsHalfKnee && overshoot < dsHalfKnee) {
        double x = overshoot + dsHalfKnee;
        g = dsSlope * x * x / (2.0 * dsKnee);
      } else if (overshoot >= dsHalfKnee) {
        g = dsSlope * overshoot;
      }
      g = std::max(g, dsRange); // range clamp: wideband ducking stays polite
      // Attack = more reduction (sibilant onset), release = recovery.
      if (g < smoothDsGR)
        smoothDsGR = dsAtt * smoothDsGR + (1.0 - dsAtt) * g;
      else
        smoothDsGR = dsRel * smoothDsGR + (1.0 - dsRel) * g;
      dsGRs[(size_t)i] = smoothDsGR;
      if (smoothDsGR < -0.01) { dsSum += smoothDsGR; dsCount++; }
    }
    m_avgDsGR = (dsCount > 0) ? dsSum / (double)dsCount : 0.0;
  }

  // Frame-0 seeding: start open if the item begins at/above the close
  // threshold (prevents chopping items that start mid-word). Below it the
  // gate starts closed - identical to the legacy hold-exhausted start state.
  bool gateOpen = false;
  if (gateEnabled && n > 0) {
    double rawDb0 = 20.0 * log10(std::max(m_results[0].peakLinear, EPSILON)) + m_itemVolDb;
    gateOpen = ((hasUp ? rawDb0 : rawDb0 + compGRs[0]) >= tClose);
  }

  // Second pass: gate (detects on the post-compression, PRE-makeup level) + output.
  // Makeup/output gain is a final stage and is NOT in the gate detector - the audio-
  // industry standard (FabFilter/Waves/Logic/Cubase/iZotope + Calf/LSP/CTAGDRC): the
  // gate/expander threshold is an input-domain control, makeup only translates the
  // output. Gating on `rawDb + compGR` keeps the deliberate "breath reduction after
  // compression" intent while making the gate threshold makeup-independent (so the
  // transfer-curve gate node at input=threshold always sits on the cliff). At makeup==0
  // this is byte-identical to the previous post-makeup detection.
  for (int i = 0; i < n; i++) {
    auto& pt = m_results[i];
    double compGR = compGRs[i];
    double totalGR = compGR;

    if (gateEnabled) {
      double rawDb = 20.0 * log10(std::max(pt.peakLinear, EPSILON)) + m_itemVolDb;
      // Down: post-comp, pre-makeup (breath-reduction intent). Up/Both: RAW
      // input - boosted noise must never hold the gate open (gate coupling).
      double detectLevel = hasUp ? rawDb : rawDb + compGR;

      // Schmitt state machine: the gate opens at/above tOpen; the hysteresis
      // band [tClose, tOpen) keeps it open WITHOUT consuming hold; hold is
      // consumed only below tClose and re-armed only at/above tOpen.
      // Order: open (attack) -> hold -> close (release).
      if (detectLevel >= tOpen) {
        gateOpen = true;
        gateHoldCounter = gateHoldFrames; // gate open, re-arm hold
      } else if (gateOpen) {
        if (detectLevel >= tClose) {
          // hysteresis band: stay open, hold untouched
        } else if (gateHoldCounter > 0) {
          gateHoldCounter--; // hold: keep gate open
        } else {
          gateOpen = false; // close
        }
      }

      // Closed-state ("basic") curve references tOpen, so GR returns to 0
      // continuously as the level climbs back toward the open threshold.
      double instantGateGR = gateOpen
        ? 0.0
        : std::max(gateSlope * (detectLevel - tOpen), gateRange);

      // Smooth gate GR independently
      // "Attack" = gate opening (GR toward 0), "Release" = gate closing (GR more negative)
      if (instantGateGR < smoothGateGR) {
        smoothGateGR = gateRelCoeff * smoothGateGR + (1.0 - gateRelCoeff) * instantGateGR;
      } else {
        smoothGateGR = gateAttCoeff * smoothGateGR + (1.0 - gateAttCoeff) * instantGateGR;
      }

      totalGR = compGR + smoothGateGR;
    }

    if (dsOn) totalGR += dsGRs[(size_t)i]; // third component (de-ess)

    pt.smoothedGR = totalGR;
    out.push_back({pt.time, totalGR + makeup});
  }

  return out;
}

// Iterative Ramer-Douglas-Peucker curve simplification
std::vector<DynamicsEngine::CompressPoint>
DynamicsEngine::SimplifyCurve(const std::vector<CompressPoint>& pts, double epsilonDb)
{
  int n = (int)pts.size();
  if (n <= 2) return pts;

  std::vector<bool> keep(n, false);
  keep[0] = true;
  keep[n - 1] = true;

  std::vector<std::pair<int, int>> stack;
  stack.reserve(32);
  stack.push_back({0, n - 1});

  while (!stack.empty()) {
    auto range = stack.back();
    stack.pop_back();
    int start = range.first;
    int end = range.second;

    if (end - start < 2) continue;

    double t0 = pts[start].time, db0 = pts[start].dbAdjust;
    double t1 = pts[end].time,   db1 = pts[end].dbAdjust;
    double dt = t1 - t0;

    double maxDist = 0.0;
    int maxIdx = start;

    for (int i = start + 1; i < end; i++) {
      double frac = (dt > 0.0) ? (pts[i].time - t0) / dt : 0.0;
      double interp = db0 + (db1 - db0) * frac;
      double dist = fabs(pts[i].dbAdjust - interp);
      if (dist > maxDist) {
        maxDist = dist;
        maxIdx = i;
      }
    }

    if (maxDist > epsilonDb) {
      keep[maxIdx] = true;
      stack.push_back({start, maxIdx});
      stack.push_back({maxIdx, end});
    }
  }

  std::vector<CompressPoint> result;
  result.reserve(n / 10);
  for (int i = 0; i < n; i++) {
    if (keep[i]) result.push_back(pts[i]);
  }
  return result;
}
