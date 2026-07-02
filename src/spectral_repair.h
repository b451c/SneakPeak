// spectral_repair.h — Spectral Repair DSP: STFT heal + AR click repair (v2.3.0 INC-5)
// Pure computation — no REAPER API calls, no GDI (same contract as dynamics_engine).
//
// Engine B — StftRepairRect: heal a time x frequency rectangle. Per-bin linear
// interpolation of log-magnitudes across time from surrounding context frames
// (Magron/Badeau/David, EUSIPCO 2015), per-bin phase propagation at the bin
// frequency, weighted-OLA resynthesis (JOS SASP). Strength blends attenuate ->
// replace and only ever pulls magnitudes DOWN toward the context, so intruders
// vanish while quieter content is left alone.
//
// Engine A — RepairClicksAR: sample-accurate click removal on a time selection.
// AR prediction-error detection with lambda = K * sigma_e + burst fusion
// (Oudre, IPOL 2015/64), AR least-squares interpolation solved per
// missing-sample cluster (Janssen 1986 via IPOL 2018/23: Levinson-Durbin +
// envelope Cholesky).
//
// Licensing: implemented from paper math only. The GPL implementations
// (ADRINAS, Audacity, GNU Wave Cleaner) were behavior references — no code
// ported. CEDAR patent US 7,978,862 expired 2023-12-05 (verified 2026-07-02).
#pragma once

// Selection limits (industry-mirrored). The UI toasts beyond these; the DSP
// also hard-fails as defense in depth.
constexpr double SPECTRAL_HEAL_MAX_SEC = 10.0;
constexpr double CLICK_REPAIR_MAX_SEC = 4.0;

struct SpectralHealResult {
  bool ok = false;          // false: bad args, empty band/selection, no context
  int framesHealed = 0;     // STFT frames modified (same for every channel)
  int binsPerFrame = 0;     // bins in the selected frequency band
  double avgAttenDb = 0.0;  // mean in-band magnitude change in dB (<= 0)
};

// Heal audio[t0, t1] x [freqLo, freqHi] Hz in place. audio = interleaved
// doubles [frame * numChannels + channel], channels healed independently.
// strength in [0, 1]: 0 = no-op, 1 = full replace toward context.
SpectralHealResult StftRepairRect(double* audio, int numFrames, int numChannels,
                                  int sampleRate, double t0, double t1,
                                  double freqLo, double freqHi, double strength);

struct ClickRepairResult {
  bool ok = false;
  int clicksRepaired = 0;  // repaired bursts (max across channels)
  int samplesRepaired = 0; // repaired samples (summed across channels)
  int clicksSkipped = 0;   // bursts over the 23 ms repair cap (max across channels)
};

// Detect + repair clicks inside audio[t0, t1] in place (same buffer layout).
// sensitivityK = detection threshold factor K (paper range 2..3; lower = more
// sensitive). Only detected samples are modified — everything else is
// bit-identical after the call.
ClickRepairResult RepairClicksAR(double* audio, int numFrames, int numChannels,
                                 int sampleRate, double t0, double t1,
                                 double sensitivityK);
