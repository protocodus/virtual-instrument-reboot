#!/usr/bin/env python3
"""Fits the handheld's Line corners to Herbert Weixelbaum's recordings.

Protocol. Weixelbaum's "game boy sound comparison"
(https://herbertweixelbaum.com/comparison.htm) shows each machine's
waveform as an audio editor drew it: "all game boys were played at their
highest volume. they were connected directly to the audiointerface", playing
LSDJ's "pulse instrument, envelope: A8, wave: pulse width 50% (square), with
a pitch of C3 (which is great C, in musical terms)", so a 65.41 Hz square.
Each picture is 674 x 186 pixels holding four periods (rising edges every
168.7 pixels), the trace in (0, 0, 100) on white, the centre line at row 93
and the grid at rows 47 and 139.

This tool reads the trace column by column, leaves out the columns within two
pixels of an edge (the editor's vertical strokes) and fits the steady-state
response of a 50 % square through one or two first-order high-passes, summed
over 1500 odd harmonics, with the level free. The time scale is the only
assumption; the original's fit landing on Pan Docs' 28 Hz checks it.

The pictures are Weixelbaum's: they are fetched into the directory given on
the command line and never committed.

  python3 Tools/fit_handheld_line.py <download-dir>

Needs NumPy and Pillow.
"""

import math
import pathlib
import sys
import urllib.request

import numpy as np
from PIL import Image

BASE = "https://herbertweixelbaum.com/2pics/"
PICTURES = {
    "original": "classic_waveform.gif",
    "play it loud": "play_it_loud_waveform.gif",
    "pocket": "pocket_waveform.gif",
    "color": "color_waveform.gif",
}
PERIOD = 168.7                      # pixels per period
MS_PER_PIXEL = 1000.0 / 65.41 / PERIOD
HARMONICS = np.arange(1, 3001, 2)
OMEGA = 2 * math.pi * HARMONICS / PERIOD


def observations(path):
    im = Image.open(path).convert("RGB")
    width, height = im.size
    columns = [[y for y in range(height) if im.getpixel((x, y)) == (0, 0, 100)] for x in range(width)]
    edges = [x for x, ys in enumerate(columns) if ys and max(ys) - min(ys) > 40]
    xs, vs = [], []
    for x, ys in enumerate(columns):
        if any(abs(x - e) <= 2 for e in edges):
            continue
        if not ys:
            v = 0.0                 # under the centre line
        elif max(ys) - min(ys) > 4:
            continue
        else:
            v = (93.0 - (min(ys) + max(ys)) / 2.0) / 46.0
        xs.append(x)
        vs.append(v)
    return np.array(xs, float), np.array(vs, float)


def tau(corner_hz):
    return 1000.0 / (2 * math.pi * corner_hz * MS_PER_PIXEL)


class Fit:
    def __init__(self, xs, vs):
        self.vs = vs
        self.sin = np.sin(np.outer(xs, OMEGA))
        self.cos = np.cos(np.outer(xs, OMEGA))

    def error(self, corners):
        h = np.ones_like(OMEGA, dtype=complex)
        for corner in corners:
            t = tau(corner)
            h *= 1j * OMEGA * t / (1 + 1j * OMEGA * t)
        coef = 4 / (math.pi * HARMONICS) * h
        model = self.sin @ coef.real + self.cos @ coef.imag
        level = float(model @ self.vs / (model @ model))
        r = self.vs - level * model
        return float(np.sqrt(np.mean(r * r)))


def at_71(corners):
    return 20 * math.log10(math.prod(71.0 / math.hypot(71.0, f) for f in corners))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = pathlib.Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    for name, picture in PICTURES.items():
        path = folder / picture
        if not path.exists():
            urllib.request.urlretrieve(BASE + picture, path)
        fit = Fit(*observations(path))
        e1, f1 = min((fit.error([f]), f) for f in np.arange(5.0, 800.0, 1.0))
        pairs = {(a, b): fit.error([a, b]) for a in np.arange(40.0, 500.0, 4.0)
                 for b in np.arange(5.0, 120.0, 1.0) if b < a}
        (fa, fb), e2 = min(pairs.items(), key=lambda kv: kv[1])
        near = [k for k, v in pairs.items() if v < 1.2 * e2]
        print(f"{name}: one pole {f1:.0f} Hz (error {e1:.4f}, {at_71([f1]):.1f} dB at 71 Hz); "
              f"two poles {fa:.0f} + {fb:.0f} Hz (error {e2:.4f}, {at_71([fa, fb]):.1f} dB at 71 Hz; "
              f"within 20 %: {min(k[0] for k in near):.0f}-{max(k[0] for k in near):.0f} and "
              f"{min(k[1] for k in near):.0f}-{max(k[1] for k in near):.0f} Hz)")


if __name__ == "__main__":
    main()
