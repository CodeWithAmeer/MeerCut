"""MeerCut's built-in audio library (M9): original music beds and sound effects, synthesised here from scratch (no
samples, no recordings, no third-party material), encoded to AAC with ffmpeg. Everything this script writes is the
project's own work and is released with MeerCut under its licence.

usage: python -I make_audio_library.py <out dir>   (writes <out>/music/*.m4a, <out>/sfx/*.m4a, <out>/library.json)
Re-running gives the same audio (fixed random seeds).
"""
import json
import math
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

SR = 48000
RNG = np.random.default_rng(20261028)


# ---------------------------------------------------------------- building blocks

def t_axis(seconds):
    return np.arange(int(seconds * SR)) / SR


def env_adsr(n, a=0.005, d=0.1, s=0.6, r=0.2, length=None):
    """Attack / decay / sustain / release envelope over n samples (length = held seconds before the release)."""
    length = length if length is not None else n / SR - r
    t = np.arange(n) / SR
    e = np.empty(n)
    e[:] = s
    e[t < a] = t[t < a] / max(a, 1e-6)
    m = (t >= a) & (t < a + d)
    e[m] = 1 - (1 - s) * (t[m] - a) / max(d, 1e-6)
    rel = t >= length
    e[rel] = s * np.maximum(0, 1 - (t[rel] - length) / max(r, 1e-6))
    return e


def midi(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def lowpass(x, cutoff):
    """One-pole low-pass (cutoff in Hz, may be an array)."""
    cutoff = np.broadcast_to(np.asarray(cutoff, dtype=float), x.shape)
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a[i]) * x[i] + a[i] * acc
        y[i] = acc
    return y


def onepole(x, a, prev=0.0):
    """y[n] = (1 - a) x[n] + a y[n-1], vectorised in blocks (closed form per block); returns (y, last y)."""
    b = 1 - a
    y = np.empty_like(x)
    block = 128
    k = np.arange(block)
    pw = a ** k
    ipw = a ** (-k.astype(float))
    for st in range(0, len(x), block):
        seg = x[st:st + block]
        n = len(seg)
        acc = np.cumsum(b * seg * ipw[:n])
        y[st:st + n] = pw[:n] * (acc + prev * a)
        prev = y[st + n - 1]
    return y, prev


def lowpass_fast(x, cutoff):
    """One-pole low-pass with a fixed cutoff."""
    return onepole(np.asarray(x, dtype=float), math.exp(-2 * math.pi * cutoff / SR))[0]


def highpass_fast(x, cutoff):
    return x - lowpass_fast(x, cutoff)


def saw(f, t, detune=0.0):
    ph = (f * (1 + detune)) * t
    return 2 * (ph - np.floor(ph + 0.5))


def square(f, t, duty=0.5):
    ph = (f * t) % 1.0
    return np.where(ph < duty, 1.0, -1.0)


def sine(f, t, phase=0.0):
    return np.sin(2 * np.pi * f * t + phase)


def noise(n):
    return RNG.uniform(-1, 1, n)


def place(buf, sig, at, gain=1.0, pan=0.0):
    """Mix a mono signal into a stereo buffer at `at` seconds with constant-power panning (-1 left .. 1 right)."""
    i = int(at * SR)
    if i >= buf.shape[0]:
        return
    sig = sig[: buf.shape[0] - i]
    ang = (pan + 1) * np.pi / 4
    buf[i:i + len(sig), 0] += sig * gain * np.cos(ang)
    buf[i:i + len(sig), 1] += sig * gain * np.sin(ang)


def reverb(buf, mix=0.25, room=0.82, damp=3500):
    """Small Schroeder reverb: four damped combs per channel (different lengths left / right) and two all-passes,
    each computed a delay-length block at a time."""
    out = buf.copy()
    a = math.exp(-2 * math.pi * damp / SR)
    for ch, offs in ((0, 0), (1, 23)):
        x = buf[:, ch]
        n = len(x)
        wet = np.zeros(n)
        for d in (1557, 1617, 1491, 1422):
            d += offs
            y = np.zeros(n)
            prev_block = np.zeros(d)
            state = 0.0
            for st in range(0, n, d):
                seg = x[st:st + d]
                lp, state = onepole(prev_block[:len(seg)], a, state)
                cur = seg + room * lp
                y[st:st + len(seg)] = cur
                prev_block = np.zeros(d)
                prev_block[:len(cur)] = cur
            wet += y
        for d, g in ((225, 0.5), (556, 0.5)):
            v_prev = np.zeros(d)
            res = np.zeros(n)
            for st in range(0, n, d):
                seg = wet[st:st + d]
                v = seg + g * v_prev[:len(seg)]
                res[st:st + len(seg)] = v_prev[:len(seg)] - g * v
                v_prev = np.zeros(d)
                v_prev[:len(v)] = v
            wet = res
        out[:, ch] = x * (1 - mix) + wet * mix * 0.25
    return out


def normalise(buf, peak=0.89):
    m = np.max(np.abs(buf))
    return buf * (peak / m) if m > 0 else buf


def fade(buf, fin=0.02, fout=0.05):
    n = len(buf)
    a, b = int(fin * SR), int(fout * SR)
    if a:
        buf[:a] *= np.linspace(0, 1, a)[:, None]
    if b:
        buf[n - b:] *= np.linspace(1, 0, b)[:, None]
    return buf


# ---------------------------------------------------------------- instruments

def kick(level=1.0):
    t = t_axis(0.45)
    f = 50 + 120 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return level * np.sin(ph) * np.exp(-t * 7.5)


def snare(level=0.7):
    t = t_axis(0.3)
    body = sine(185, t) * np.exp(-t * 22)
    rattle = highpass_fast(noise(len(t)), 1800) * np.exp(-t * 14)
    return level * (0.5 * body + 0.45 * rattle)


def clap(level=0.6):
    t = t_axis(0.35)
    n = highpass_fast(noise(len(t)), 1200)
    e = np.zeros_like(t)
    for k, d in enumerate((0.0, 0.011, 0.022)):
        m = t >= d
        e[m] = np.maximum(e[m], np.exp(-(t[m] - d) * (60 if k < 2 else 13)))
    return level * n * e


def hat(level=0.25, open_=False):
    t = t_axis(0.35 if open_ else 0.08)
    n = highpass_fast(noise(len(t)), 7000)
    return 0.55 * level * n * np.exp(-t * (9 if open_ else 55))   # hats sit behind the music


def pluck(f, seconds=0.6, bright=4000, level=0.4):
    """Karplus-Strong plucked string (one period of the delay line per step)."""
    n = int(seconds * SR)
    period = max(2, int(SR / f))
    buf = lowpass_fast(noise(period), bright)
    chunks = []
    for _ in range(n // period + 1):
        chunks.append(buf)
        buf = 0.5 * (buf + np.roll(buf, -1)) * 0.996
    out = np.concatenate(chunks)[:n]
    return level * out * env_adsr(n, 0.002, 0.05, 0.9, 0.08)


def epiano(f, seconds, level=0.25):
    t = t_axis(seconds)
    tone = sine(f, t) + 0.3 * sine(2 * f, t) * np.exp(-t * 3) + 0.12 * sine(3.01 * f, t) * np.exp(-t * 6)
    trem = 1 + 0.08 * np.sin(2 * np.pi * 4.5 * t)
    return level * tone * trem * env_adsr(len(t), 0.004, 0.4, 0.45, 0.3, seconds - 0.3)


def pad(freqs, seconds, level=0.12, cutoff=2200, attack=0.6):
    t = t_axis(seconds)
    sig = np.zeros_like(t)
    for f in freqs:
        for dt in (-0.006, 0.0, 0.007):
            sig += saw(f, t, dt)
    sig = lowpass_fast(sig / (3 * len(freqs)), cutoff)
    return level * 3 * sig * env_adsr(len(t), attack, 0.3, 0.85, min(1.2, seconds / 3), seconds - min(1.2, seconds / 3))


def bass(f, seconds, level=0.35, cutoff=600):
    t = t_axis(seconds)
    sig = 0.7 * sine(f, t) + 0.5 * lowpass_fast(saw(f, t), cutoff)
    return level * sig * env_adsr(len(t), 0.005, 0.15, 0.75, 0.05, seconds - 0.05)


def bell_tone(f, seconds=2.5, level=0.25):
    t = t_axis(seconds)
    sig = sum(a * sine(f * r, t) * np.exp(-t * d) for r, a, d in ((1, 1, 1.6), (2.76, 0.45, 3.0), (5.4, 0.25, 5.0), (8.9, 0.12, 8.0)))
    return level * sig


CHORDS = {   # chord tones as MIDI notes (original progressions in common keys)
    "C": [60, 64, 67], "G": [55, 59, 62], "Am": [57, 60, 64], "F": [53, 57, 60], "Dm": [50, 53, 57], "Em": [52, 55, 59],
    "Dm7": [50, 53, 57, 60], "G7": [55, 59, 62, 65], "Cmaj7": [48, 52, 55, 59], "Am7": [45, 48, 52, 55],
    "Fmaj7": [53, 57, 60, 64], "E": [52, 56, 59],
}


# ---------------------------------------------------------------- music beds (original)

def music_bright_morning(seconds=32):
    bpm = 116
    beat = 60 / bpm
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["C", "G", "Am", "F"]
    bars = int(seconds / (4 * beat))
    melody = [76, 74, 72, 74, 76, 79, 76, 74]
    for bar in range(bars):
        ch = CHORDS[prog[bar % 4]]
        t0 = bar * 4 * beat
        for b in range(4):
            place(buf, kick(0.9), t0 + b * beat) if b in (0, 2) else place(buf, snare(0.55), t0 + b * beat)
            for e in range(2):
                place(buf, hat(0.16), t0 + (b + e / 2) * beat, pan=0.3)
        place(buf, bass(midi(ch[0] - 24), 4 * beat - 0.05), t0)
        for k in range(8):   # strummed plucks on the off-beats
            for j, n in enumerate(ch):
                place(buf, pluck(midi(n + 12), 0.5, 5000, 0.18), t0 + k * beat / 2 + j * 0.012, pan=-0.3 + 0.3 * j)
        if bar >= 2:
            for k, n in enumerate(melody):
                if (bar + k) % 3 != 2:
                    place(buf, pluck(midi(n), 0.45, 6000, 0.22), t0 + k * beat / 2, pan=0.15)
    return normalise(fade(reverb(buf, 0.18), 0.01, 1.5))


def music_lofi(seconds=32):
    bpm = 82
    beat = 60 / bpm
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Dm7", "G7", "Cmaj7", "Am7"]
    bars = int(seconds / (4 * beat))
    for bar in range(bars):
        t0 = bar * 4 * beat
        ch = CHORDS[prog[bar % 4]]
        place(buf, kick(0.85), t0)
        place(buf, kick(0.6), t0 + 2.5 * beat)
        place(buf, snare(0.45), t0 + beat)
        place(buf, snare(0.45), t0 + 3 * beat)
        for e in range(8):   # swung eighths
            place(buf, hat(0.1), t0 + e * beat / 2 + (0.06 if e % 2 else 0), pan=0.4)
        for j, n in enumerate(ch):
            place(buf, epiano(midi(n + 12), 4 * beat * 0.95, 0.12), t0 + j * 0.02, pan=-0.2 + 0.13 * j)
        place(buf, bass(midi(ch[0] - 12), 2 * beat, 0.3, 400), t0)
        place(buf, bass(midi(ch[0] - 12 + 7), 2 * beat, 0.25, 400), t0 + 2 * beat)
    crackle = np.zeros(len(buf))
    pos = RNG.integers(0, len(buf), 900)
    crackle[pos] = RNG.uniform(-0.25, 0.25, len(pos))
    buf += (lowpass_fast(crackle, 4000) + 0.004 * lowpass_fast(noise(len(buf)), 2000))[:, None]
    buf[:, 0] = lowpass_fast(buf[:, 0], 6000)
    buf[:, 1] = lowpass_fast(buf[:, 1], 6000)
    return normalise(fade(reverb(buf, 0.22), 0.5, 2.0))


def music_cinematic(seconds=34):
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Am", "F", "C", "G"]
    seg = seconds / 8
    for i in range(8):
        ch = CHORDS[prog[i % 4]]
        notes = [midi(n) for n in ch] + [midi(ch[0] - 12)]
        lvl = 0.10 + 0.06 * i / 7
        place(buf, pad(notes, seg + 1.0, lvl, 1400 + 400 * i, 0.9), i * seg)
        if i >= 2:   # low drum hits
            for k in range(4):
                place(buf, kick(0.75 + 0.05 * i), i * seg + k * seg / 4)
        if i >= 4:
            for k in range(8):
                place(buf, hat(0.08), i * seg + k * seg / 8, pan=0.5)
    t = t_axis(4.0)   # rising noise into the last chord
    rise = highpass_fast(noise(len(t)), 800) * (t / 4.0) ** 2 * 0.35
    place(buf, rise, seconds - 4.6)
    place(buf, kick(1.0), seconds - 0.6)
    return normalise(fade(reverb(buf, 0.35, 0.86), 1.0, 0.6))


def music_corporate(seconds=32):
    bpm = 108
    beat = 60 / bpm
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["C", "Am", "F", "G"]
    bars = int(seconds / (4 * beat))
    for bar in range(bars):
        t0 = bar * 4 * beat
        ch = CHORDS[prog[bar % 4]]
        for b in range(4):
            place(buf, kick(0.7), t0 + b * beat)
            if b % 2:
                place(buf, clap(0.45), t0 + b * beat)
        arp = ch + [ch[0] + 12]
        for k in range(16):
            place(buf, pluck(midi(arp[k % 4] + 12), 0.35, 7000, 0.16), t0 + k * beat / 4, pan=0.25 * math.sin(k))
        place(buf, pad([midi(n) for n in ch], 4 * beat, 0.06, 2600), t0)
        place(buf, bass(midi(ch[0] - 24), 4 * beat, 0.3), t0)
    return normalise(fade(reverb(buf, 0.2), 0.01, 1.5))


def music_ambient(seconds=36):
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Fmaj7", "Cmaj7", "Am7", "Em"]
    seg = seconds / 6
    for i in range(6):
        ch = CHORDS[prog[i % 4]]
        place(buf, pad([midi(n) for n in ch], seg + 2.5, 0.11, 1500, 2.0), i * seg)
    scale = [72, 74, 76, 79, 81, 84]
    for k in range(18):
        place(buf, bell_tone(midi(scale[int(RNG.integers(0, len(scale)))]), 3.0, 0.12), 1.0 + k * seconds / 19,
              pan=float(RNG.uniform(-0.6, 0.6)))
    return normalise(fade(reverb(buf, 0.45, 0.9), 2.0, 3.0))


def music_electro(seconds=32):
    bpm = 124
    beat = 60 / bpm
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Am", "F", "C", "G"]
    bars = int(seconds / (4 * beat))
    for bar in range(bars):
        t0 = bar * 4 * beat
        ch = CHORDS[prog[bar % 4]]
        for b in range(4):
            place(buf, kick(1.0), t0 + b * beat)
            place(buf, hat(0.18, open_=True), t0 + (b + 0.5) * beat, pan=0.2)
            if b % 2:
                place(buf, clap(0.5), t0 + b * beat)
        for k in range(8):   # pumping bass and stabs (ducked after each kick)
            place(buf, bass(midi(ch[0] - 24), beat / 2 * 0.9, 0.32, 900), t0 + k * beat / 2 + beat / 4)
        if bar % 2 == 1:
            for k in (0, 1.5, 3):
                st = sum(lowpass_fast(saw(midi(n + 12), t_axis(0.25)), 3000) for n in ch) / len(ch)
                place(buf, 0.2 * st * env_adsr(len(st), 0.003, 0.1, 0.4, 0.08), t0 + k * beat, pan=-0.2)
    return normalise(fade(reverb(buf, 0.15), 0.01, 1.5))


MUSIC = [
    ("bright_morning", "Bright Morning", "Upbeat", 116, music_bright_morning),
    ("lofi_evening", "Lo-Fi Evening", "Chill", 82, music_lofi),
    ("rising_horizon", "Rising Horizon", "Cinematic", 0, music_cinematic),
    ("clear_progress", "Clear Progress", "Corporate", 108, music_corporate),
    ("quiet_space", "Quiet Space", "Ambient", 0, music_ambient),
    ("night_drive", "Night Drive", "Electronic", 124, music_electro),
]


# ---------------------------------------------------------------- sound effects (original)

def sfx_whoosh(seconds=0.9, up=True):
    t = t_axis(seconds)
    env = np.sin(np.pi * t / seconds) ** 2
    cutoff = (300 + 6000 * (t / seconds if up else 1 - t / seconds))
    sig = lowpass(noise(len(t)), cutoff) * env
    out = np.zeros((len(t), 2))
    for i, p in enumerate(np.linspace(-0.8, 0.8, 8)):   # sweeps left to right
        a, b = i * len(t) // 8, (i + 1) * len(t) // 8
        place(out, sig[a:b], a / SR, 1.0, p)
    return out


def mono(sig):
    return np.stack([sig, sig], axis=1)


def sfx_pop():
    t = t_axis(0.16)
    f = 900 * np.exp(-t * 18) + 220
    return mono(np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 30))


def sfx_click():
    t = t_axis(0.05)
    return mono(highpass_fast(noise(len(t)), 2500) * np.exp(-t * 180) + 0.5 * sine(2400, t) * np.exp(-t * 120))


def sfx_ding():
    return mono(bell_tone(midi(88), 1.8, 0.6))


def sfx_chime():
    out = np.zeros((int(1.6 * SR), 2))
    for k, n in enumerate((79, 84, 88)):
        place(out, bell_tone(midi(n), 1.3, 0.45), k * 0.12, pan=-0.3 + 0.3 * k)
    return out


def sfx_shutter():
    out = np.zeros((int(0.35 * SR), 2))
    for at in (0.0, 0.09):
        t = t_axis(0.06)
        place(out, highpass_fast(noise(len(t)), 1500) * np.exp(-t * 90), at, 0.8)
        place(out, sine(1300, t) * np.exp(-t * 140), at, 0.4)
    return out


def sfx_riser(seconds=3.0):
    t = t_axis(seconds)
    f = 200 * 2 ** (3 * t / seconds)
    tone = sum(np.sin(2 * np.pi * np.cumsum(f * r) / SR) / (k + 1) for k, r in enumerate((1, 1.5, 2.01)))
    nz = highpass_fast(noise(len(t)), 1000)
    env = (t / seconds) ** 2
    return mono((0.5 * tone + 0.4 * nz) * env)


def sfx_impact():
    t = t_axis(2.2)
    f = 40 + 90 * np.exp(-t * 9)
    boom = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 2.2)
    crack = lowpass_fast(noise(len(t)), 3000) * np.exp(-t * 18)
    return reverb(mono(boom + 0.5 * crack), 0.3)


def sfx_glitch():
    out = np.zeros((int(0.7 * SR), 2))
    for k in range(14):
        n = int(RNG.integers(400, 2400))
        f = float(RNG.uniform(200, 2400))
        burst = square(f, t_axis(n / SR)) * 0.4 + noise(n) * 0.3
        place(out, burst, k * 0.045, 0.7, float(RNG.uniform(-0.8, 0.8)))
    return out


def sfx_laser():
    t = t_axis(0.5)
    f = 2400 * np.exp(-t * 7) + 150
    return mono(np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 6))


def sfx_coin():
    out = np.zeros((int(0.6 * SR), 2))
    for at, n in ((0.0, 83), (0.08, 88)):
        t = t_axis(0.5)
        place(out, square(midi(n), t, 0.25) * 0.3 * np.exp(-t * 7), at)
    return out


def sfx_success():
    out = np.zeros((int(1.4 * SR), 2))
    for k, n in enumerate((72, 76, 79, 84)):
        place(out, pluck(midi(n), 0.9, 6000, 0.5), k * 0.11, pan=-0.3 + 0.2 * k)
    return reverb(out, 0.25)


def sfx_error():
    out = np.zeros((int(0.7 * SR), 2))
    for at in (0.0, 0.22):
        t = t_axis(0.18)
        place(out, lowpass_fast(square(160, t), 1800) * 0.5 * env_adsr(len(t), 0.003, 0.02, 0.9, 0.03), at)
    return out


def sfx_typing(seconds=2.0):
    out = np.zeros((int(seconds * SR), 2))
    at = 0.0
    while at < seconds - 0.1:
        t = t_axis(0.03)
        key_ = highpass_fast(noise(len(t)), 1800) * np.exp(-t * 260) + 0.3 * sine(float(RNG.uniform(900, 1400)), t) * np.exp(-t * 300)
        place(out, key_, at, float(RNG.uniform(0.4, 0.8)), float(RNG.uniform(-0.3, 0.3)))
        at += float(RNG.uniform(0.07, 0.17))
    return out


def sfx_bubble():
    t = t_axis(0.25)
    f = 400 + 1400 * (t / 0.25) ** 0.6
    return mono(np.sin(2 * np.pi * np.cumsum(f) / SR) * np.sin(np.pi * t / 0.25) * 0.8)


def sfx_swipe():
    return sfx_whoosh(0.35, up=False)


def sfx_heartbeat():
    out = np.zeros((int(2.4 * SR), 2))
    for beat in range(2):
        for k, lvl in ((0.0, 1.0), (0.22, 0.7)):
            t = t_axis(0.25)
            f = 55 + 25 * np.exp(-t * 30)
            place(out, np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 16) * lvl, beat * 1.1 + k)
    return out


def sfx_drumroll(seconds=2.5):
    out = np.zeros((int((seconds + 0.6) * SR), 2))
    n = int(seconds * 28)
    for k in range(n):
        place(out, snare(0.25 + 0.4 * k / n), k / 28.0, pan=0.2 * math.sin(k))
    place(out, mono(np.zeros(1))[:, 0], 0)
    place(out, clap(0.8), seconds)
    place(out, kick(1.0), seconds)
    return reverb(out, 0.2)


SFX = [
    ("whoosh", "Whoosh", "Transition", lambda: sfx_whoosh(0.9)),
    ("swoosh_fast", "Fast Swoosh", "Transition", lambda: sfx_whoosh(0.45)),
    ("swipe", "Swipe", "Transition", sfx_swipe),
    ("riser", "Riser", "Transition", sfx_riser),
    ("impact", "Impact Boom", "Transition", sfx_impact),
    ("glitch", "Glitch", "Transition", sfx_glitch),
    ("pop", "Pop", "Interface", sfx_pop),
    ("click", "Click", "Interface", sfx_click),
    ("bubble", "Bubble", "Interface", sfx_bubble),
    ("typing", "Typing", "Interface", sfx_typing),
    ("shutter", "Camera Shutter", "Interface", sfx_shutter),
    ("ding", "Ding", "Notification", sfx_ding),
    ("chime", "Chime", "Notification", sfx_chime),
    ("success", "Success", "Notification", sfx_success),
    ("error", "Error Buzz", "Notification", sfx_error),
    ("coin", "Coin", "Game", sfx_coin),
    ("laser", "Laser", "Game", sfx_laser),
    ("heartbeat", "Heartbeat", "Dramatic", sfx_heartbeat),
    ("drumroll", "Drum Roll", "Dramatic", sfx_drumroll),
]


# ---------------------------------------------------------------- M11: more music and effects (original)
# Generated after everything above, so the M9 pieces draw the same random numbers and come out unchanged.

CHORDS.update({"Em7": [52, 55, 59, 62], "D": [50, 54, 57], "Bb": [46, 50, 53], "A7": [45, 49, 52, 55]})


def low(n):
    """The same note in the bass octave (C2 .. B2)."""
    return 36 + (n - 36) % 12


def swept(f):
    """Phase of a tone whose frequency f (Hz per sample) changes over time."""
    return 2 * np.pi * np.cumsum(f) / SR


def pulse(f, t, duty=0.5):
    """A pulse wave without a DC offset, its edges softened (hard square waves overshoot once encoded)."""
    return lowpass_fast(square(f, t, duty) - (2 * duty - 1), 6000)


def loudness(buf, rms_db, peak):
    """As loud as rms_db (RMS, dBFS), but never above peak: the new pieces sit at the same level as the old ones."""
    rms = np.sqrt(np.mean(buf ** 2))
    return buf * min(peak / np.max(np.abs(buf)), 10 ** (rms_db / 20) / rms) if rms > 0 else buf


def tom(f0=110.0, level=0.8):
    t = t_axis(0.7)
    f = f0 * (1 + 0.6 * np.exp(-t * 18))
    return level * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 5.5)


def marimba(f, seconds=0.5, level=0.3):
    t = t_axis(seconds)
    return level * (sine(f, t) + 0.25 * sine(4 * f, t) * np.exp(-t * 25)) * np.exp(-t * 7)


def clav(f, seconds=0.16, level=0.22):
    t = t_axis(seconds)
    return level * lowpass_fast(pulse(f, t, 0.3), 3500) * np.exp(-t * 20)


def sleigh(level=0.25):
    t = t_axis(0.22)
    jingle = (sine(5200, t) + sine(6310, t) + sine(7420, t)) / 3
    return level * (0.55 * highpass_fast(noise(len(t)), 6000) + 0.35 * jingle) * np.exp(-t * 18)


def ride(level=0.12):
    t = t_axis(0.7)
    return level * (0.6 * highpass_fast(noise(len(t)), 5000) + 0.15 * (sine(3150, t) + sine(4620, t))) * np.exp(-t * 5)


def brass(freqs, seconds, level=0.2, cutoff=1800):
    t = t_axis(seconds)
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5.5 * t)
    sig = 0
    for f in freqs:   # sawtooths with a gentle vibrato
        ph = np.cumsum(f * vib) / SR
        sig = sig + 2 * (ph - np.floor(ph + 0.5))
    r = min(0.25, seconds / 3)
    return level * lowpass_fast(sig / len(freqs), cutoff) * env_adsr(len(t), min(0.07, seconds / 4), 0.2, 0.8, r, seconds - r)


def square_note(f, seconds, level=0.18, duty=0.5):
    t = t_axis(seconds)
    return level * pulse(f, t, duty) * env_adsr(len(t), 0.002, 0.05, 0.7, 0.03, seconds - 0.03)


def music_hiphop(seconds=32):
    beat = 60 / 90
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Am7", "Dm7", "Em7", "Am7"]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        for at, lvl in ((0, 1.0), (1.75, 0.75), (2.5, 0.85)):   # boom-bap kicks
            place(buf, kick(lvl), t0 + at * beat)
        for b in (1, 3):
            place(buf, snare(0.6), t0 + b * beat)
            place(buf, clap(0.28), t0 + b * beat + 0.01)
        for e in range(8):   # swung hats
            place(buf, hat(0.12), t0 + e * beat / 2 + (0.07 if e % 2 else 0), pan=0.35)
        for at, dur in ((0, 1.5), (1.5, 2.5)):
            for j, n in enumerate(ch):
                place(buf, epiano(midi(n + 12), dur * beat * 0.95, 0.1), t0 + at * beat + j * 0.015, pan=-0.15 + 0.1 * j)
        place(buf, bass(midi(ch[0] - 12), 1.5 * beat, 0.34, 450), t0)
        place(buf, bass(midi(ch[0] - 12), 1.4 * beat, 0.28, 450), t0 + 2.5 * beat)
    buf[:, 0] = lowpass_fast(buf[:, 0], 7000)
    buf[:, 1] = lowpass_fast(buf[:, 1], 7000)
    return normalise(fade(reverb(buf, 0.16), 0.01, 1.5))


def music_acoustic(seconds=32):
    beat = 60 / 100
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["G", "D", "Em", "C"]
    melody = [74, 76, 79, 76, 74, 71, 74, 72]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        pattern = [ch[0] - 12, ch[2], ch[1] + 12, ch[2], ch[0] + 12, ch[2], ch[1] + 12, ch[2]]   # fingerpicking
        for k, n in enumerate(pattern):
            place(buf, pluck(midi(n), 0.9, 3500, 0.2), t0 + k * beat / 2, pan=-0.25 + 0.07 * k)
        place(buf, bass(midi(ch[0] - 12), 2 * beat, 0.22, 500), t0)
        place(buf, bass(midi(ch[0] - 12), 2 * beat, 0.18, 500), t0 + 2 * beat)
        if bar >= 2:
            for e in range(16):   # shaker
                place(buf, hat(0.05 if e % 2 else 0.08), t0 + e * beat / 4, pan=0.45)
        if bar >= 4 and bar % 2 == 0:
            for k, n in enumerate(melody):
                place(buf, pluck(midi(n), 0.7, 5500, 0.17), t0 + k * beat / 2, pan=0.2)
    return normalise(fade(reverb(buf, 0.24), 0.3, 2.0))


def music_piano(seconds=34):
    beat = 60 / 72
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["C", "G", "Am", "F", "Dm7", "G", "C", "C"]
    tune = [[76, 74, 72, 74], [74, 71, 74, 79], [76, 72, 76, 81], [77, 76, 74, 72],
            [74, 77, 76, 74], [74, 79, 77, 74], [72, 76, 79, 84], [83, 79, 76, 72]]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 8]]
        for j, n in enumerate(ch):   # the left hand, broken gently
            place(buf, epiano(midi(n), 4 * beat * 0.98, 0.1), t0 + j * 0.08, pan=-0.2)
        place(buf, epiano(midi(ch[0] - 12), 4 * beat, 0.12), t0, pan=-0.3)
        if bar >= 1:
            for k, n in enumerate(tune[bar % 8]):
                place(buf, epiano(midi(n), beat * 1.6, 0.13) + 0.3 * bell_tone(midi(n), beat * 1.6, 0.05), t0 + k * beat,
                      pan=0.2)
    return normalise(fade(reverb(buf, 0.42, 0.88), 0.6, 3.0))


def music_epic(seconds=34):
    beat = 60 / 80
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Dm", "Bb", "F", "C"]
    bars = int(seconds / (4 * beat))
    for bar in range(bars):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        lift_ = min(1.0, 0.35 + bar / bars)
        place(buf, pad([midi(n) for n in ch] + [midi(ch[0] - 12)], 4 * beat + 0.8, 0.07 + 0.06 * lift_, 900 + 1800 * lift_, 0.6), t0)
        if bar >= 2:   # big drums
            for at, f0, lvl in ((0, 70, 1.0), (1.5, 90, 0.7), (2.0, 70, 0.9), (2.5, 110, 0.6), (3.0, 80, 0.8), (3.5, 120, 0.6)):
                place(buf, tom(f0, lvl * lift_), t0 + at * beat, pan=0.3 * math.sin(at * 3))
        if bar >= 4:
            place(buf, brass([midi(n) for n in ch], 3.5 * beat, 0.16, 1500 + 900 * lift_), t0, pan=0.1)
        if bar >= bars - 4:
            for e in range(8):
                place(buf, snare(0.25 + 0.04 * e), t0 + e * beat / 2, pan=-0.2)
    t = t_axis(3.0)
    place(buf, highpass_fast(noise(len(t)), 900) * (t / 3.0) ** 2 * 0.3, seconds - 3.8)
    place(buf, sfx_impact()[:, 0] * 0.8, seconds - 0.9)
    return normalise(fade(reverb(buf, 0.32, 0.86), 1.2, 0.8))


def music_funk(seconds=32):
    beat = 60 / 108
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Em7", "Em7", "A7", "A7"]
    line = [0, None, 12, None, None, 0, None, 10, None, 0, None, None, 7, None, 12, 10]   # 16ths, from the root
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        for at in (0, 0.75, 2.5):
            place(buf, kick(0.9), t0 + at * beat)
        for b in (1, 3):
            place(buf, snare(0.62), t0 + b * beat)
        for g in (1.75, 3.25):   # ghost notes
            place(buf, snare(0.16), t0 + g * beat, pan=0.1)
        for e in range(16):
            place(buf, hat(0.1 if e % 2 else 0.15), t0 + e * beat / 4, pan=0.35)
        for k, step in enumerate(line):
            if step is not None:
                place(buf, bass(midi(ch[0] - 12 + step), beat / 4 * 0.9, 0.36, 800), t0 + k * beat / 4)
        for k in (1, 3, 6, 9, 11, 14):   # clavinet stabs on the off-beat 16ths
            for j, n in enumerate(ch[1:]):
                place(buf, clav(midi(n + 12)), t0 + k * beat / 4 + j * 0.004, pan=-0.3)
    return normalise(fade(reverb(buf, 0.12), 0.01, 1.5))


def music_chiptune(seconds=30):
    beat = 60 / 140
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["C", "Am", "F", "G"]
    tune = [72, 76, 79, 76, 81, 79, 76, 74, 72, 74, 76, 79, 77, 76, 74, 72]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        for b in range(4):
            t = t_axis(0.12)   # a blip kick and a noise snare, as on old consoles
            blip = np.where(np.sin(swept(90 * np.exp(-t * 18) + 40)) >= 0, 1.0, -1.0)
            place(buf, blip * 0.35 * np.exp(-t * 20), t0 + b * beat)
            if b % 2:
                place(buf, noise(int(0.09 * SR)) * 0.22 * np.exp(-t_axis(0.09) * 30), t0 + b * beat)
        for k in range(16):   # fast arpeggio
            place(buf, square_note(midi(ch[k % 3] + 12), beat / 4 * 0.85, 0.07, 0.25), t0 + k * beat / 4, pan=-0.25)
        for b in range(4):
            place(buf, square_note(midi(ch[0] - 12), beat * 0.9, 0.14, 0.5), t0 + b * beat)
        if bar >= 2:
            for k in range(8):
                place(buf, square_note(midi(tune[(bar % 2) * 8 + k]), beat / 2 * 0.9, 0.12, 0.5), t0 + k * beat / 2, pan=0.2)
    return normalise(fade(buf, 0.01, 1.2))


def music_reggae(seconds=32):
    beat = 60 / 76
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["C", "F", "G", "F"]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        place(buf, kick(0.9), t0 + 2 * beat)   # one drop
        place(buf, snare(0.5), t0 + 2 * beat)
        for e in range(8):
            place(buf, hat(0.1), t0 + e * beat / 2 + (0.05 if e % 2 else 0), pan=0.4)
        for b in range(4):   # the skank: short chords on the off-beats
            for j, n in enumerate(ch):
                place(buf, pluck(midi(n + 12), 0.18, 4000, 0.14), t0 + (b + 0.5) * beat + j * 0.006, pan=-0.3)
        for at, step in ((0, 0), (1, 7), (1.5, 12), (2.5, 7), (3, 5)):   # a melodic bass line
            place(buf, bass(midi(low(ch[0]) + step), 0.45 * beat, 0.42, 380), t0 + at * beat)
    return normalise(fade(reverb(buf, 0.2), 0.01, 1.5))


def music_tropical(seconds=32):
    beat = 60 / 104
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Am", "F", "C", "G"]
    hook = [69, 72, 76, 72, 74, 72, 69, 67]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        for b in range(4):
            place(buf, kick(0.7), t0 + b * beat)
            place(buf, hat(0.14, open_=True), t0 + (b + 0.5) * beat, pan=0.25)
            if b % 2:
                place(buf, clap(0.35), t0 + b * beat)
        place(buf, pad([midi(n) for n in ch], 4 * beat, 0.05, 1800), t0)
        place(buf, bass(midi(low(ch[0])), 0.9 * beat, 0.3, 700), t0 + 0.5 * beat)
        place(buf, bass(midi(low(ch[0])), 0.9 * beat, 0.26, 700), t0 + 2.5 * beat)
        if bar >= 2:
            for k, at in enumerate((0, 0.75, 1.5, 2.25, 2.5, 3.0, 3.25, 3.5)):   # a syncopated marimba hook
                place(buf, marimba(midi(hook[k]), 0.4, 0.24), t0 + at * beat, pan=0.15)
    return normalise(fade(reverb(buf, 0.22), 0.01, 1.5))


def music_jazz(seconds=32):
    beat = 60 / 120
    swing = beat * 2 / 3   # the swung eighth
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["Dm7", "G7", "Cmaj7", "Am7"]
    bars = int(seconds / (4 * beat))
    for bar in range(bars):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        nxt = CHORDS[prog[(bar + 1) % 4]][0]
        for b, n in enumerate((ch[0], ch[1], ch[2], nxt + 1 if bar % 2 else nxt - 1)):   # walking bass
            place(buf, bass(midi(n - 12), beat * 0.9, 0.38, 700), t0 + b * beat)
        for at in (0, 1, 1 + swing / beat, 2, 3, 3 + swing / beat):   # ride: ding, ding-ga-ding
            place(buf, ride(0.1), t0 + at * beat, pan=0.35)
        for b in (1, 3):
            place(buf, hat(0.08), t0 + b * beat, pan=0.2)
        for at in (1 + swing / beat, 3):   # comping on the swung off-beat and on four
            for j, n in enumerate(ch):
                place(buf, epiano(midi(n + 12), 0.45, 0.09), t0 + at * beat + j * 0.01, pan=-0.2)
    return normalise(fade(reverb(buf, 0.2), 0.01, 1.5))


def music_suspense(seconds=32):
    beat = 60 / 60
    buf = np.zeros((int(seconds * SR), 2))
    drone = pad([midi(38), midi(45), midi(50), midi(51)], seconds, 0.09, 700, 3.0)   # D, A, D and a dissonant E flat
    place(buf, drone, 0)
    for k in range(int(seconds / beat) - 3):   # heartbeat pulses that slowly quicken
        at = k * beat * (1 - 0.15 * k / seconds)
        place(buf, tom(55, 0.55), at)
        place(buf, tom(50, 0.35), at + 0.22)
    for k in range(9):   # glassy high notes, a semitone apart
        place(buf, bell_tone(midi(84 + (k % 2)), 2.5, 0.06), 2.0 + k * 3.1, pan=-0.5 + 0.12 * k)
    t = t_axis(5.0)
    place(buf, highpass_fast(noise(len(t)), 1200) * (t / 5.0) ** 3 * 0.25, seconds - 6.0)
    place(buf, sfx_impact()[:, 0] * 0.7, seconds - 1.0)
    return normalise(fade(reverb(buf, 0.4, 0.88), 2.0, 1.0))


def music_holiday(seconds=32):
    beat = 60 / 120
    buf = np.zeros((int(seconds * SR), 2))
    prog = ["C", "Am", "F", "G7"]
    tune = [[76, 76, 79, 76], [72, 74, 76, 0], [77, 77, 76, 74], [74, 76, 74, 79]]
    for bar in range(int(seconds / (4 * beat))):
        t0, ch = bar * 4 * beat, CHORDS[prog[bar % 4]]
        for e in range(8):
            place(buf, sleigh(0.18 if e % 2 else 0.24), t0 + e * beat / 2, pan=0.3)
        place(buf, kick(0.6), t0)
        place(buf, kick(0.5), t0 + 2 * beat)
        for b in (1, 3):
            place(buf, snare(0.35), t0 + b * beat)
        place(buf, bass(midi(low(ch[0])), 2 * beat, 0.26, 600), t0)
        place(buf, bass(midi(low(ch[2])), 2 * beat, 0.22, 600), t0 + 2 * beat)
        for k in range(8):   # pizzicato chords
            place(buf, pluck(midi(ch[k % len(ch)] + 12), 0.25, 5000, 0.12), t0 + k * beat / 2, pan=-0.25)
        if bar >= 2:
            for k, n in enumerate(tune[bar % 4]):
                if n:
                    place(buf, bell_tone(midi(n), 1.2, 0.16), t0 + k * beat, pan=0.15)
    return normalise(fade(reverb(buf, 0.24), 0.01, 1.5))


MUSIC_M11 = [
    ("hiphop_groove", "Hip Hop Groove", "Chill", 90, music_hiphop),
    ("acoustic_morning", "Acoustic Morning", "Acoustic", 100, music_acoustic),
    ("piano_reflections", "Piano Reflections", "Acoustic", 72, music_piano),
    ("epic_trailer", "Epic Trailer", "Cinematic", 80, music_epic),
    ("funky_groove", "Funky Groove", "Upbeat", 108, music_funk),
    ("eight_bit_adventure", "8-Bit Adventure", "Fun", 140, music_chiptune),
    ("island_breeze", "Island Breeze", "Chill", 76, music_reggae),
    ("tropical_sunset", "Tropical Sunset", "Electronic", 104, music_tropical),
    ("jazz_cafe", "Jazz Cafe", "Chill", 120, music_jazz),
    ("suspense", "Suspense", "Cinematic", 60, music_suspense),
    ("holiday_bells", "Holiday Bells", "Fun", 120, music_holiday),
]


def sfx_applause(seconds=3.0):
    out = np.zeros((int((seconds + 0.6) * SR), 2))
    for _ in range(170):
        at = float(RNG.uniform(0, 1) ** 1.6 * seconds)   # dense at first, thinning out
        place(out, clap(float(RNG.uniform(0.12, 0.32))), at, 1.0, float(RNG.uniform(-0.8, 0.8)))
    t = t_axis(seconds)
    bed = highpass_fast(lowpass_fast(noise(len(t)), 6000), 900) * np.minimum(1, t / 0.2) * np.exp(-t * 0.9) * 0.08
    place(out, bed, 0)
    return reverb(out, 0.25)


def sfx_camera_flash():
    out = np.zeros((int(1.0 * SR), 2))
    t = t_axis(0.6)   # the flash charging up, then the shutter
    place(out, np.sin(swept(1500 + 2500 * (t / 0.6) ** 1.5)) * 0.12 * np.minimum(1, t / 0.1), 0)
    place(out, sfx_shutter()[:, 0], 0.62)
    place(out, sfx_pop()[:, 0] * 0.5, 0.64)
    return out


def sfx_cash_register():
    out = np.zeros((int(1.6 * SR), 2))
    t = t_axis(0.16)
    place(out, lowpass_fast(noise(len(t)), 2500) * np.exp(-t * 25) * 0.6, 0, pan=-0.2)   # the drawer
    for k in range(5):
        place(out, highpass_fast(noise(400), 3000) * 0.3, 0.02 + k * 0.025, pan=0.2)
    place(out, bell_tone(midi(96), 1.3, 0.5), 0.18, pan=0.1)   # ka-ching
    place(out, bell_tone(midi(100), 1.1, 0.35), 0.24, pan=-0.1)
    return out


def sfx_notification():
    out = np.zeros((int(0.9 * SR), 2))
    for k, n in enumerate((88, 93)):
        t = t_axis(0.5)
        place(out, (sine(midi(n), t) + 0.2 * sine(2 * midi(n), t)) * np.exp(-t * 9) * 0.5, k * 0.12)
    return out


def sfx_level_up():
    out = np.zeros((int(1.1 * SR), 2))
    for k, n in enumerate((72, 76, 79, 84, 88, 91)):
        place(out, square_note(midi(n), 0.07 if k < 5 else 0.4, 0.25, 0.25), k * 0.065)
    return out


def sfx_game_over():
    out = np.zeros((int(2.0 * SR), 2))
    for k, n in enumerate((67, 66, 65)):
        place(out, square_note(midi(n), 0.22, 0.22, 0.5), k * 0.25)
    t = t_axis(1.0)
    wobble = lowpass_fast(np.where(np.sin(swept(midi(64) * (1 + 0.01 * np.sin(2 * np.pi * 6 * t)))) >= 0, 1.0, -1.0), 6000)
    place(out, wobble * 0.22 * env_adsr(len(t), 0.01, 0.1, 0.8, 0.4, 0.6), 0.75)
    return out


def sfx_boing():
    t = t_axis(0.9)
    f = 180 + 130 * np.sin(2 * np.pi * 11 * t) * np.exp(-t * 4)
    return mono(np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 4.5) * 0.8)


def sfx_slide_whistle():
    t = t_axis(1.0)
    f = (500 + 1100 * (t / 1.0) ** 1.3) * (1 + 0.02 * np.sin(2 * np.pi * 6 * t))
    tone = np.sin(2 * np.pi * np.cumsum(f) / SR)
    breath = highpass_fast(noise(len(t)), 2000) * 0.06
    return mono((0.6 * tone + breath) * np.sin(np.pi * t / 1.0) ** 0.5)


def sfx_cymbal():
    t = t_axis(2.8)
    metal = sum(square(f, t, 0.5) for f in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)) / 6
    sig = highpass_fast(metal + 0.6 * noise(len(t)), 4500) * np.exp(-t * 1.6)
    return reverb(mono(sig * 0.7), 0.2)


def sfx_thunder():
    t = t_axis(4.5)
    crack = highpass_fast(noise(len(t)), 1500) * np.exp(-t * 9) * 0.5
    rumble = lowpass_fast(noise(len(t)), 220) * (1 - np.exp(-t * 6)) * np.exp(-t * 0.8) * 3.0
    return reverb(mono(crack + rumble), 0.35, 0.88)


def sfx_rain(seconds=5.0):
    t = t_axis(seconds)
    bed = highpass_fast(lowpass_fast(noise(len(t)), 9000), 1500) * 0.25
    out = mono(bed)
    for _ in range(260):   # drops
        d = t_axis(0.02)
        place(out, sine(float(RNG.uniform(2500, 6000)), d) * np.exp(-d * 300) * float(RNG.uniform(0.05, 0.2)),
              float(RNG.uniform(0, seconds - 0.03)), 1.0, float(RNG.uniform(-0.9, 0.9)))
    return fade(out, 0.4, 0.4)


def sfx_wind(seconds=5.0):
    t = t_axis(seconds)
    gust = 0.5 + 0.5 * np.sin(2 * np.pi * 0.25 * t) * np.sin(2 * np.pi * 0.11 * t + 1.0)
    sig = lowpass(noise(len(t)), 300 + 900 * gust) * (0.4 + 0.6 * gust)
    return fade(np.stack([sig, np.roll(sig, int(0.013 * SR))], axis=1), 0.5, 0.6)


def sfx_door_knock():
    out = np.zeros((int(1.0 * SR), 2))
    for k in range(3):
        t = t_axis(0.12)
        knock = lowpass_fast(noise(len(t)), 900) * np.exp(-t * 60) + 0.6 * sine(180, t) * np.exp(-t * 40)
        place(out, knock * (0.9 if k < 2 else 1.0), k * 0.19)
    return reverb(out, 0.15)


def sfx_clock_tick(seconds=4.0):
    out = np.zeros((int(seconds * SR), 2))
    for k in range(int(seconds * 2)):
        t = t_axis(0.03)
        f = 2600 if k % 2 == 0 else 1900
        place(out, (highpass_fast(noise(len(t)), 2000) * 0.4 + sine(f, t)) * np.exp(-t * 220) * 0.5, k * 0.5)
    return out


def sfx_countdown_beeps():
    out = np.zeros((int(3.9 * SR), 2))
    for k in range(3):
        t = t_axis(0.13)
        place(out, sine(880, t) * env_adsr(len(t), 0.003, 0.02, 0.9, 0.02) * 0.5, k * 1.0)
    t = t_axis(0.7)
    place(out, sine(1760, t) * env_adsr(len(t), 0.003, 0.05, 0.9, 0.1) * 0.5, 3.0)
    return out


def sfx_tada():
    out = np.zeros((int(2.2 * SR), 2))
    place(out, brass([midi(60), midi(64), midi(67)], 0.16, 0.3, 2400), 0.0)
    place(out, brass([midi(72), midi(76), midi(79), midi(84)], 1.4, 0.32, 2600), 0.2)
    place(out, sfx_cymbal()[:, 0] * 0.3, 0.2, pan=0.2)
    return reverb(out, 0.2)


def sfx_record_scratch():
    t = t_axis(1.0)
    src = lowpass_fast(saw(110, t) + saw(165, t) + 0.3 * noise(len(t)), 3500)
    tt = t_axis(0.55)
    speed = 2.2 * np.sin(2 * np.pi * 3.2 * tt) + 0.4
    pos = np.clip(0.2 * SR + np.cumsum(speed), 0, len(src) - 1)
    sig = np.interp(pos, np.arange(len(src)), src) * np.minimum(1, np.abs(speed)) * 0.6
    return mono(sig)


def sfx_sad_trombone():
    out = np.zeros((int(3.0 * SR), 2))
    for k, n in enumerate((58, 57, 56)):
        t = t_axis(0.45)
        place(out, lowpass_fast(saw(midi(n), t), 1400) * env_adsr(len(t), 0.03, 0.1, 0.8, 0.1) * 0.35, k * 0.5)
    t = t_axis(1.4)
    f = midi(55) * (1 + 0.012 * np.sin(2 * np.pi * 5 * t))
    wah = lowpass(np.sin(2 * np.pi * np.cumsum(f) / SR) + 0.5 * saw(midi(55), t), 500 + 600 * (0.5 + 0.5 * np.sin(2 * np.pi * 5 * t)))
    place(out, wah * env_adsr(len(t), 0.03, 0.1, 0.8, 0.5, 0.9) * 0.4, 1.5)
    return out


def sfx_magic():
    out = np.zeros((int(1.8 * SR), 2))
    for k, n in enumerate((84, 88, 91, 96, 100)):
        place(out, bell_tone(midi(n), 1.0, 0.22), k * 0.06, pan=-0.5 + 0.25 * k)
    t = t_axis(1.2)
    shimmer = highpass_fast(noise(len(t)), 7000) * (0.5 + 0.5 * np.sin(2 * np.pi * 14 * t)) * np.exp(-t * 3) * 0.12
    place(out, shimmer, 0.05)
    return reverb(out, 0.35)


def sfx_water_drop():
    t = t_axis(0.3)
    f = 600 + 1300 * (1 - np.exp(-t * 40))
    return mono(np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 18) * 0.7)


def sfx_punch():
    t = t_axis(0.35)
    f = 45 + 50 * np.exp(-t * 30)
    thump = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 14)
    snap = highpass_fast(noise(len(t)), 2500) * np.exp(-t * 90) * 0.5
    return reverb(mono(thump + snap), 0.12)


SFX_M11 = [
    ("applause", "Applause", "Everyday", sfx_applause),
    ("camera_flash", "Camera Flash", "Everyday", sfx_camera_flash),
    ("cash_register", "Cash Register", "Everyday", sfx_cash_register),
    ("door_knock", "Door Knock", "Everyday", sfx_door_knock),
    ("clock_tick", "Clock Ticking", "Everyday", sfx_clock_tick),
    ("thunder", "Thunder", "Nature", sfx_thunder),
    ("rain", "Rain", "Nature", sfx_rain),
    ("wind", "Wind", "Nature", sfx_wind),
    ("water_drop", "Water Drop", "Nature", sfx_water_drop),
    ("boing", "Boing", "Comedy", sfx_boing),
    ("slide_whistle", "Slide Whistle", "Comedy", sfx_slide_whistle),
    ("record_scratch", "Record Scratch", "Comedy", sfx_record_scratch),
    ("sad_trombone", "Sad Trombone", "Comedy", sfx_sad_trombone),
    ("level_up", "Level Up", "Game", sfx_level_up),
    ("game_over", "Game Over", "Game", sfx_game_over),
    ("notification", "Notification", "Notification", sfx_notification),
    ("countdown_beeps", "Countdown Beeps", "Notification", sfx_countdown_beeps),
    ("tada", "Ta-Da", "Notification", sfx_tada),
    ("magic", "Magic Sparkle", "Transition", sfx_magic),
    ("cymbal", "Cymbal Crash", "Dramatic", sfx_cymbal),
    ("punch", "Punch", "Dramatic", sfx_punch),
]


# ---------------------------------------------------------------- output

def write(buf, path):
    tmp = path.with_suffix(".wav")
    pcm = (np.clip(buf, -1, 1) * 32767).astype("<i2")
    with wave.open(str(tmp), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(tmp), "-c:a", "aac", "-b:a", "160k", "-movflags", "+faststart",
                    "-metadata", "comment=MeerCut built-in audio library (original synthesis)", str(path)], check=True)
    tmp.unlink()
    return len(buf) / SR


def main():
    out = Path(sys.argv[1])
    (out / "music").mkdir(parents=True, exist_ok=True)
    (out / "sfx").mkdir(parents=True, exist_ok=True)
    items = []
    for ident, title, mood, bpm, fn in MUSIC:
        seconds = write(fn(), out / "music" / f"{ident}.m4a")
        items.append({"id": "music." + ident, "title": title, "kind": "music", "category": mood, "bpm": bpm,
                      "file": f"music/{ident}.m4a", "seconds": round(seconds, 3)})
        print("music", ident, round(seconds, 1), flush=True)
    for ident, title, cat, fn in SFX:
        seconds = write(fade(normalise(fn(), 0.79), 0.002, 0.01), out / "sfx" / f"{ident}.m4a")   # headroom: AAC overshoots
        items.append({"id": "sfx." + ident, "title": title, "kind": "sfx", "category": cat, "file": f"sfx/{ident}.m4a",
                      "seconds": round(seconds, 3)})
        print("sfx", ident, round(seconds, 2), flush=True)
    # M11, after the M9 pieces (their random numbers are drawn first, so they come out as before); levelled to the
    # M9 beds' loudness (about -17 dB RMS) and the loudest sound effects' (-12 dB)
    for ident, title, mood, bpm, fn in MUSIC_M11:
        seconds = write(loudness(fn(), -17, 0.89), out / "music" / f"{ident}.m4a")
        items.append({"id": "music." + ident, "title": title, "kind": "music", "category": mood, "bpm": bpm,
                      "file": f"music/{ident}.m4a", "seconds": round(seconds, 3)})
        print("music", ident, round(seconds, 1), flush=True)
    for ident, title, cat, fn in SFX_M11:
        seconds = write(fade(loudness(fn(), -12, 0.79), 0.002, 0.01), out / "sfx" / f"{ident}.m4a")
        items.append({"id": "sfx." + ident, "title": title, "kind": "sfx", "category": cat, "file": f"sfx/{ident}.m4a",
                      "seconds": round(seconds, 3)})
        print("sfx", ident, round(seconds, 2), flush=True)
    (out / "library.json").write_text(json.dumps({"format": "meercut-audio-library", "version": 1,
                                                  "licence": "MeerCut's own synthesised audio (GPL-3.0-or-later with MeerCut)",
                                                  "items": items}, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
