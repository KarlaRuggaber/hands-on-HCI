"""Equalizer: listens to what the speaker plays and turns it into 8 frequency bands + beat.

Spotify no longer offers audio analysis through its API, so the bands come from the real sound:
PipeWire offers a "monitor" of every output, `parec` reads it as raw samples, numpy does the FFT.
"""
import asyncio
import collections
import logging
import time

import numpy as np

log = logging.getLogger("eq")

RATE = 44100
EDGES = [40, 90, 180, 350, 700, 1400, 2800, 5600, 12000]      # 8 bands, log spaced (Hz)


class Analyzer:
    """pure maths, no I/O: feed blocks of samples, get bands 0..255 and a beat flag."""

    def __init__(self, rate=RATE, n_fft=2048):
        self.n_fft = n_fft
        self.window = np.hanning(n_fft).astype(np.float32)
        freqs = np.fft.rfftfreq(n_fft, 1 / rate)
        self.bins = [(freqs >= lo) & (freqs < hi) for lo, hi in zip(EDGES[:-1], EDGES[1:])]
        self.buf = np.zeros(n_fft, np.float32)
        self.peak = np.full(8, 1e-3, np.float32)                  # automatic gain per band
        self.smooth = np.zeros(8, np.float32)
        self.bass_hist = collections.deque(maxlen=40)            # about 1.3 s at 30 fps
        self.last_beat = 0.0

    def feed(self, samples: np.ndarray):
        n = len(samples)
        self.buf = np.concatenate([self.buf[n:], samples])[-self.n_fft:]
        spec = np.abs(np.fft.rfft(self.buf * self.window))
        energy = np.array([spec[b].mean() if b.any() else 0.0 for b in self.bins], np.float32)
        rms = float(np.sqrt(np.mean(self.buf ** 2)))
        if rms < 1e-4:                                            # silence: nothing to show
            self.smooth *= 0.8
            return None, False
        self.peak = np.maximum(self.peak * 0.995, energy)         # slowly forgetting loudest value
        # quiet bands must not light up fully: never divide by less than a share of the loudest band
        ref = np.maximum(self.peak, self.peak.max() * 0.08)
        level = np.clip(energy / ref, 0, 1) ** 1.4
        # fast attack, slower release looks calmer on LEDs
        self.smooth = np.where(level > self.smooth, level, self.smooth * 0.75 + level * 0.25)
        bass = float(energy[0] + energy[1])
        beat = False
        now = time.time()
        if len(self.bass_hist) > 10 and bass > 1.6 * (sum(self.bass_hist) / len(self.bass_hist)) \
                and now - self.last_beat > 0.25:
            beat, self.last_beat = True, now
        self.bass_hist.append(bass)
        return [int(v * 255) for v in self.smooth], beat


class AudioEq:
    def __init__(self, source="@DEFAULT_MONITOR@", fps=30, delay_ms=180, on_frame=None):
        self.source, self.fps, self.delay = source, fps, delay_ms / 1000
        self.on_frame = on_frame                                  # callback(bands, beat)
        self.an = Analyzer()

    async def run(self):
        hop = RATE // self.fps
        cmd = ["parec", "-d", self.source, "--format=s16le", f"--rate={RATE}", "--channels=1", "--raw",
               "--latency-msec=20"]
        pending = collections.deque()
        while True:
            try:
                proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE,
                                                            stderr=asyncio.subprocess.DEVNULL)
            except FileNotFoundError:
                log.error("parec missing: sudo apt install pulseaudio-utils")
                return
            log.info("listening on %s", self.source)
            peak, frames, t_log = 0.0, 0, time.time()
            try:
                while True:
                    raw = await proc.stdout.readexactly(hop * 2)
                    s = np.frombuffer(raw, np.int16).astype(np.float32) / 32768
                    peak = max(peak, float(np.abs(s).max()))
                    bands, beat = self.an.feed(s)
                    if bands is not None:
                        frames += 1
                        pending.append((time.time() + self.delay, bands, beat))
                    if time.time() - t_log > 10:              # every 10 s: does sound arrive at all?
                        db = 20 * np.log10(peak) if peak > 0 else -120
                        if frames:
                            log.info("eq: Pegel %.0f dBFS, %d Bilder in 10 s", db, frames)
                        else:
                            log.warning("eq: nur Stille auf %s (Pegel %.0f dBFS). Falsche Quelle?", self.source, db)
                        peak, frames, t_log = 0.0, 0, time.time()
                    # hand out frames once their delay is over: the speaker is behind the samples we read
                    while pending and pending[0][0] <= time.time():
                        _, b, bt = pending.popleft()
                        if self.on_frame:
                            self.on_frame(b, bt)
            except (asyncio.IncompleteReadError, ConnectionError):
                log.warning("audio capture stopped, restarting")
            finally:
                if proc.returncode is None:
                    proc.kill()
            await asyncio.sleep(2)
