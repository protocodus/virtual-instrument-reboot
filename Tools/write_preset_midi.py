#!/usr/bin/env python3
"""Write the committed README demo phrases, Docs/audio/midi/<NN>-<slug>.mid.

Eight of the sixteen factory presets get a demo (README G3), each showing a
different side of the boards: the handheld's square lead and its arpeggio,
the console's gliding twin pulses, the cartridge saw and the disk unit's
modulated wave, the PSG's periodic-noise bass, the micro's envelope buzz and
its one-bit beeper. NN is the preset's program number. Every phrase is
original. The .mid files are the committed sources the demo renderer reads;
this script only records how they were written. Re-run it only to change a
phrase on purpose, and commit the changed .mid files on their own:

    python3 Tools/write_preset_midi.py           # write Docs/audio/midi
    python3 Tools/write_preset_midi.py --check   # the committed files match

Every file is format 0 on MIDI channel 1 at 480 PPQ, with a tempo and a track
name. Its last event falls 5.5-9.5 s in, so with the renderer's 2.5 s tail
every demo is 8-12 s long (G3c). A legato line in a Mono preset overlaps its
keys by LEGATO beats, so the next key glides from the last instead of
striking again.
"""
import argparse
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MIDI_DIR = ROOT / "Docs/audio/midi"
BANK = ROOT / "Source/DSP/RebootPresets.h"

PPQ = 480
LEGATO = 0.08
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def n(name):
    """'C4' is MIDI 60; sharps as 'F#3', flats as 'Bb2'."""
    pitch = NAMES[name[0]]
    rest = name[1:]
    if rest[0] == "#":
        pitch, rest = pitch + 1, rest[1:]
    elif rest[0] == "b":
        pitch, rest = pitch - 1, rest[1:]
    return 12 * (int(rest) + 1) + pitch


class Phrase:
    def __init__(self, bpm):
        self.bpm = bpm
        self.events = []   # (beat, order, bytes): note-offs sort before note-ons

    def note(self, beat, length, pitch, velocity=100):
        key = n(pitch) if isinstance(pitch, str) else pitch
        self.events.append((beat, 1, bytes((0x90, key, velocity))))
        self.events.append((beat + length, 0, bytes((0x80, key, 0))))

    def chord(self, beat, length, pitches, velocity=96):
        for p in pitches:
            self.note(beat, length, p, velocity)

    def line(self, beat, notes, velocity=100, gate=0.9):
        """(pitch, beats) pairs one after another; pitch None is a rest."""
        for pitch, length in notes:
            if pitch is not None:
                self.note(beat, length * gate, pitch, velocity)
            beat += length
        return beat

    def legato(self, beat, notes, velocity=100):
        """Each key held LEGATO beats into the next, for a Mono glide."""
        for i, (pitch, length) in enumerate(notes):
            tie = LEGATO if i + 1 < len(notes) and notes[i + 1][0] is not None else -0.1
            if pitch is not None:
                self.note(beat, length + tie, pitch, velocity)
            beat += length
        return beat

    def bend(self, beat, amount):
        value = max(0, min(16383, round(8192 + amount * 8191)))
        self.events.append((beat, 2, bytes((0xE0, value & 0x7F, value >> 7))))

    def seconds(self):
        return max(beat for beat, _, _ in self.events) * 60 / self.bpm

    def smf(self, name):
        def vlq(value):
            out = [value & 0x7F]
            value >>= 7
            while value:
                out.append(0x80 | (value & 0x7F))
                value >>= 7
            return bytes(reversed(out))

        tempo = round(60_000_000 / self.bpm)
        track = bytearray()
        track += b"\x00\xff\x03" + vlq(len(name)) + name.encode("ascii")
        track += b"\x00\xff\x51\x03" + tempo.to_bytes(3, "big")
        previous = 0
        for beat, _, message in sorted(self.events, key=lambda e: (round(e[0] * PPQ), e[1], e[2])):
            tick = round(beat * PPQ)
            track += vlq(tick - previous) + message
            previous = tick
        track += b"\x00\xff\x2f\x00"
        return (b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
                + b"MTrk" + struct.pack(">I", len(track)) + track)


def square_lead():
    # A bright tune over a walking bass, both on the handheld's pulses.
    p = Phrase(132)
    p.line(0, [("E5", 0.5), ("G5", 0.5), ("A5", 1), ("G5", 0.5), ("E5", 0.5), ("D5", 1),
               ("C5", 0.5), ("D5", 0.5), ("E5", 0.5), ("G5", 0.5), ("E5", 2),
               ("A5", 0.5), ("C6", 0.5), ("B5", 0.5), ("A5", 0.5), ("G5", 1), ("E5", 1),
               ("D5", 0.5), ("E5", 0.5), ("C5", 3)], velocity=104)
    p.line(0, [("C3", 1), ("G3", 1), ("A2", 1), ("E3", 1), ("F2", 1), ("C3", 1), ("G2", 1), ("D3", 1),
               ("C3", 1), ("G3", 1), ("A2", 1), ("E3", 1), ("F2", 1), ("G2", 1), ("C3", 2)], velocity=90)
    return p


def pocket_arp():
    # Held chords stepped through by the arpeggio, a chord a bar.
    p = Phrase(120)
    for bar, chord in enumerate((("A3", "C4", "E4"), ("F3", "A3", "C4", "F4"), ("C4", "E4", "G4"),
                                 ("G3", "B3", "D4", "G4"))):
        p.chord(bar * 4, 3.9, chord, velocity=100)
    return p


def pulse_glide():
    # A legato line the twin pulses glide through, vibrato on the long notes.
    p = Phrase(100)
    p.legato(0, [("A4", 1), ("C5", 0.5), ("D5", 0.5), ("E5", 2), ("G5", 0.5), ("E5", 0.5),
                 ("D5", 1), ("C5", 1.5), ("D5", 0.5), ("A4", 3)], velocity=100)
    p.bend(10.5, 0.0)
    return p


def saw_lead():
    # A bluesy line on the cartridge's saw, bent into its last note.
    p = Phrase(96)
    end = p.legato(0, [("D4", 0.75), ("F4", 0.25), ("G4", 0.5), ("Ab4", 0.5), ("A4", 1), (None, 0.5),
                       ("C5", 0.5), ("A4", 0.5), ("G4", 0.5), ("F4", 0.5), ("D4", 1.5)], velocity=104)
    p.note(end, 2.5, "D5", 110)
    for i in range(9):
        p.bend(end + 0.5 + i * 0.1, -0.5 + 0.5 * i / 8)
    p.bend(end + 2.4, 0.0)
    return p


def disk_organ():
    # Four voices on four disk units: a chorale.
    p = Phrase(84)
    chords = [(("C4", "E4", "G4"), "C5"), (("A3", "C4", "E4"), "E5"), (("F3", "A3", "C4"), "F5"),
              (("G3", "B3", "D4"), "D5"), (("C4", "E4", "G4"), "C5")]
    lengths = [2, 2, 2, 2, 3]
    beat = 0
    for (chord, top), length in zip(chords, lengths):
        p.chord(beat, length * 0.95, chord, velocity=96)
        p.note(beat, length * 0.95, top, 104)
        beat += length
    return p


def periodic_bass():
    # The PSG's tones below their lowest note, as its periodic noise: two
    # octaves under these keys.
    p = Phrase(100)
    p.line(0, [("C4", 0.5), ("C4", 0.5), ("G4", 0.5), ("C4", 0.5), ("Bb3", 0.5), ("Bb3", 0.5), ("F4", 0.5),
               ("Bb3", 0.5), ("Ab3", 0.5), ("Ab3", 0.5), ("Eb4", 0.5), ("Ab3", 0.5), ("G3", 0.5), ("D4", 0.5),
               ("G3", 0.5), ("B3", 0.5), ("C4", 2)], velocity=110, gate=0.8)
    return p


def buzz_bass():
    # The micro's envelope run as an oscillator, a saw an octave down.
    p = Phrase(90)
    p.line(0, [("E3", 0.75), ("E3", 0.25), ("E4", 0.5), ("E3", 0.5), ("G3", 0.75), ("A3", 0.25),
               ("B3", 0.5), ("D4", 0.5), ("E3", 0.75), ("E3", 0.25), ("E4", 0.5), ("D4", 0.5),
               ("B3", 0.5), ("A3", 0.5), ("G3", 0.5), ("E3", 1.5)], velocity=110, gate=0.85)
    return p


def one_bit_melody():
    # The beeper's own kind of tune: short notes, one at a time.
    p = Phrase(116)
    p.line(0, [("C5", 0.5), ("E5", 0.5), ("G5", 0.5), ("C6", 0.5), ("B5", 0.5), ("G5", 0.5), ("A5", 1),
               ("F5", 0.5), ("A5", 0.5), ("C6", 0.5), ("A5", 0.5), ("G5", 1), ("E5", 1),
               ("D5", 0.5), ("E5", 0.5), ("F5", 0.5), ("D5", 0.5), ("C5", 2)], velocity=110, gate=0.75)
    return p


# Slug and phrase of each demo's preset; the number is its place in the bank.
DEMOS = [
    ("square-lead", square_lead), ("pocket-arp", pocket_arp), ("pulse-glide", pulse_glide),
    ("saw-lead", saw_lead), ("disk-organ", disk_organ), ("periodic-bass", periodic_bass),
    ("buzz-bass", buzz_bass), ("one-bit-melody", one_bit_melody),
]


def files():
    bank = [re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
            for name in re.findall(r'\{ "([^"]+)",', BANK.read_text(encoding="utf-8"))]
    result = {}
    for slug, build in DEMOS:
        if slug not in bank:
            sys.exit(f"{slug} is not a preset of the bank {bank}")
        phrase = build()
        end = phrase.seconds()
        if not 5.5 <= end <= 9.5:
            sys.exit(f"{slug}: {end:.1f} s is outside the 5.5-9.5 s phrase length")
        result[f"{bank.index(slug) + 1:02d}-{slug}.mid"] = phrase.smf(slug)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail unless the committed files match")
    args = parser.parse_args()
    expected = files()
    found = {path.name: path.read_bytes() for path in MIDI_DIR.glob("*.mid")}
    if args.check:
        stale = sorted(set(found) ^ set(expected) | {name for name in expected if found.get(name) != expected[name]})
        if stale:
            sys.exit(f"Docs/audio/midi differs from {Path(__file__).name} in: {', '.join(stale)}")
        print(f"{len(expected)} demo phrases match {Path(__file__).name}")
        return
    MIDI_DIR.mkdir(parents=True, exist_ok=True)
    for name in set(found) - set(expected):
        (MIDI_DIR / name).unlink()
    for name, data in expected.items():
        (MIDI_DIR / name).write_bytes(data)
        print(name)


if __name__ == "__main__":
    main()
