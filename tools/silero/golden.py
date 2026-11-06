#!/usr/bin/env python3
"""Builds a Silero parity fixture: 16 kHz mono int16 PCM + the official ONNX
model's probability for every 512-sample block (with the 64-sample context the
silero-vad wrapper adds). Used by tests/silero.sh; needs espeak-ng,
onnxruntime, numpy and silero-vad.

  python3 tools/silero/golden.py OUT.pcm OUT.prob
Layout: speech, silence, speech over noise, a held chord, noise, speech.
"""
import os, subprocess, sys, tempfile, wave
import numpy as np, onnxruntime as ort, silero_vad

def fala(texto, voz="en-us"):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "f.wav")
        subprocess.run(["espeak-ng", "-v", voz, "-s", "150", "-w", p, texto], check=True)
        with wave.open(p) as w:
            hz = w.getframerate(); x = np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768.0
    t = np.arange(int(len(x) * 16000 / hz)) * hz / 16000.0
    return np.interp(t, np.arange(len(x)), x)

def main():
    rng = np.random.default_rng(7)
    sil = lambda s: np.zeros(int(16000 * s))
    ruido = lambda s, a: a * rng.standard_normal(int(16000 * s))
    t = np.arange(16000 * 3) / 16000.0
    acorde = 0.15 * sum(np.sin(2 * np.pi * f * t) for f in (220, 277.2, 329.6))
    a = fala("The quick brown fox jumps over the lazy dog, and then it runs away.")
    b = fala("Hej, hur mår du idag? Det är en fin dag.", "sv")
    c = fala("Where were you last night? I waited for hours.")
    x = np.concatenate([sil(0.5), a, sil(1.5), b + ruido(len(b) / 16000, 0.02), sil(1.0), acorde, ruido(2.0, 0.05), c, sil(0.5)])
    pcm = np.clip(np.round(x * 32767 * 0.8), -32768, 32767).astype(np.int16)
    f = pcm.astype(np.float32) / 32768.0
    s = ort.InferenceSession(os.path.join(os.path.dirname(silero_vad.__file__), "data", "silero_vad.onnx"))
    st = np.zeros((2, 1, 128), np.float32); ctx = np.zeros(64, np.float32); pr = []
    for k in range(0, len(f) - 511, 512):
        ch = f[k:k + 512]
        o, st = s.run(None, {"input": np.concatenate([ctx, ch])[None], "state": st, "sr": np.array(16000, np.int64)})
        ctx = ch[-64:]; pr.append(o[0, 0])
    pcm.tofile(sys.argv[1]); np.array(pr, np.float32).tofile(sys.argv[2])
    print("blocks", len(pr), "speech>0.5", int((np.array(pr) > 0.5).sum()))

if __name__ == "__main__":
    main()
