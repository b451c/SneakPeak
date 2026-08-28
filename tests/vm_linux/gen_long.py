import numpy as np, soundfile as sf, sys
from pathlib import Path
out = Path(sys.argv[1]); minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
sr = 44100; total = int(minutes * 60 * sr)
out.parent.mkdir(parents=True, exist_ok=True)
rng = np.random.default_rng(1)
with sf.SoundFile(str(out), "w", samplerate=sr, channels=2, subtype="PCM_16") as f:
    chunk = sr * 10; done = 0
    while done < total:
        n = min(chunk, total - done)
        t = (np.arange(n) + done) / sr
        sig = 0.03 * rng.standard_normal(n)
        head = t < 2.0                      # tone burst in the FIRST 2 s only
        sig[head] += 0.6 * np.sin(2 * np.pi * 1000.0 * t[head])
        f.write(np.stack([sig, sig], axis=1).astype(np.float32))
        done += n
print("wrote", out, total / sr / 60, "min")
