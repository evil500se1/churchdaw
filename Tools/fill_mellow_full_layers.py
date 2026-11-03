"""Expand Mellow Upright to full chromatic + complete velocity layers.

Source: installed sparse roots (13 notes x 9 vels).
Output: every MIDI note 21..108 x 9 velocity layers, pitch-shifted offline
so ChurchDAW barely needs realtime stretch (kills ghost/wrong notes).
"""
from __future__ import annotations

import json
import os
import re
import shutil
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy import signal

BASE = Path(os.environ["APPDATA"]) / r"ChurchDAW\Libraries\church-live\Pianos\Mellow Upright"
OUT = Path(os.environ["TEMP"]) / "mellow_full_layers"

VELS = [14, 28, 42, 56, 71, 85, 99, 113, 127]
MIDI_LO, MIDI_HI = 21, 108
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def midi_to_name(m: int) -> str:
    return f"{NOTE_NAMES[m % 12]}{(m // 12) - 2}"


def name_to_midi(n: str) -> int:
    semis = {"A": 9, "B": 11, "C": 0, "D": 2, "E": 4, "F": 5, "G": 7}
    m = re.match(r"^([A-G])([#b]?)(-?\d+)$", n)
    if not m:
        raise ValueError(n)
    s = semis[m.group(1)]
    if m.group(2) == "#":
        s += 1
    elif m.group(2) == "b":
        s -= 1
    return (int(m.group(3)) + 2) * 12 + s


def read_wav(path: Path):
    with open(path, "rb") as f:
        assert f.read(4) == b"RIFF"
        f.read(4)
        assert f.read(4) == b"WAVE"
        sr = ch = bits = None
        data = None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            cid, size = struct.unpack("<4sI", hdr)
            chunk = f.read(size + (size % 2))
            if cid == b"fmt ":
                _af, ch, sr, _br, _ba, bits = struct.unpack("<HHIIHH", chunk[:16])
            elif cid == b"data":
                data = chunk[:size]
                break
    bps = bits // 8
    if bits == 24:
        raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, ch, 3)
        v = (
            raw[:, :, 0].astype(np.int32)
            | (raw[:, :, 1].astype(np.int32) << 8)
            | (raw[:, :, 2].astype(np.int32) << 16)
        )
        v[v >= 0x800000] -= 0x1000000
        audio = v.astype(np.float64) / 8388608.0
    elif bits == 16:
        audio = (
            np.frombuffer(data, dtype="<i2").astype(np.float64).reshape(-1, ch) / 32768.0
        )
    else:
        raise RuntimeError(f"unsupported bits={bits}")
    return sr, audio


def write_wav_24(path: Path, sr: int, audio: np.ndarray) -> None:
    audio = np.clip(audio, -1.0, 1.0)
    n, ch = audio.shape
    pcm = np.round(audio * 8388607.0).astype(np.int32)
    b0 = (pcm & 0xFF).astype(np.uint8)
    b1 = ((pcm >> 8) & 0xFF).astype(np.uint8)
    b2 = ((pcm >> 16) & 0xFF).astype(np.uint8)
    data = np.stack([b0, b1, b2], axis=-1).reshape(-1).tobytes()
    block_align = ch * 3
    byte_rate = sr * block_align
    fmt = struct.pack("<HHIIHH", 1, ch, sr, byte_rate, block_align, 24)
    data_padded = data + (b"\x00" if len(data) % 2 else b"")
    riff_size = 4 + (8 + len(fmt)) + (8 + len(data_padded))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", riff_size) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data_padded)


def pitch_shift_resample(audio: np.ndarray, semis: float) -> np.ndarray:
    if abs(semis) < 1e-9:
        return audio
    ratio = 2.0 ** (semis / 12.0)
    n_new = max(16, int(round(audio.shape[0] / ratio)))
    out = np.zeros((n_new, audio.shape[1]), dtype=np.float64)
    for c in range(audio.shape[1]):
        out[:, c] = signal.resample(audio[:, c], n_new)
    return out


def main() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    src: dict[tuple[int, int], Path] = {}
    for p in BASE.glob("*.wav"):
        m = re.match(r"Mellow Upright-(.+)-V(\d+)-(\d+)\.wav$", p.name)
        if not m:
            continue
        note, vel = m.group(1), int(m.group(2))
        src[(name_to_midi(note), vel)] = p

    roots = sorted({m for (m, _v) in src})
    print("source roots", roots)
    print("source pairs", len(src))
    if not roots:
        raise SystemExit("no source samples in " + str(BASE))

    copied = generated = 0
    for vel in VELS:
        avail = sorted([m for m in roots if (m, vel) in src])
        if not avail:
            print("NO roots for vel", vel)
            continue
        cache: dict[int, tuple[int, np.ndarray]] = {}

        def get_audio(midi: int):
            if midi not in cache:
                cache[midi] = read_wav(src[(midi, vel)])
            return cache[midi]

        for target in range(MIDI_LO, MIDI_HI + 1):
            nearest = min(avail, key=lambda r: (abs(r - target), abs(r - 60)))
            name = midi_to_name(target)
            out_path = OUT / f"Mellow Upright-{name}-V{vel:03d}-01.wav"
            if target == nearest:
                shutil.copy2(src[(nearest, vel)], out_path)
                copied += 1
            else:
                sr, audio = get_audio(nearest)
                shifted = pitch_shift_resample(audio, target - nearest)
                write_wav_24(out_path, sr, shifted)
                generated += 1
        print(f"vel {vel:03d}: done")

    print("copied", copied, "generated", generated, "total", copied + generated)

    by_note: dict[str, list] = defaultdict(list)
    for p in sorted(OUT.glob("*.wav")):
        m = re.match(r"Mellow Upright-(.+)-V(\d+)-", p.name)
        note, vel = m.group(1), int(m.group(2))
        by_note[note].append((vel, p.name, name_to_midi(note)))

    notes_sorted = sorted(by_note.keys(), key=name_to_midi)
    midis = [name_to_midi(n) for n in notes_sorted]

    def key_range(i: int):
        m = midis[i]
        lo = 0 if i == 0 else (midis[i - 1] + m) // 2 + 1
        hi = 127 if i == len(midis) - 1 else (m + midis[i + 1]) // 2
        return lo, hi

    samples = []
    sfz_lines = [
        "<group>",
        "ampeg_attack=0.001",
        "ampeg_release=1.20",
        "loop_mode=no_loop",
        "",
    ]
    for i, note in enumerate(notes_sorted):
        lo, hi = key_range(i)
        midi = midis[i]
        layers = sorted(by_note[note], key=lambda x: x[0])
        prev = -1
        for vel, fname, _ in layers:
            vel_lo = prev + 1
            vel_hi = vel
            samples.append(
                {
                    "file": fname,
                    "midi_root": midi,
                    "note_name": note,
                    "key_low": lo,
                    "key_high": hi,
                    "vel_low": vel_lo,
                    "vel_high": vel_hi,
                }
            )
            sfz_lines.append(
                f"<region> sample={fname} pitch_keycenter={midi} "
                f"lokey={lo} hikey={hi} lovel={vel_lo} hivel={vel_hi}"
            )
            prev = vel

    meta = {
        "name": "Mellow Upright",
        "raw_label": "Mellow Upright",
        "category": "PIANOS",
        "default_attack": 0.001,
        "default_release": 1.2,
        "loop_enabled": False,
        "sample_count": len(samples),
        "samples": samples,
    }
    (OUT / "Mellow Upright.json").write_text(
        json.dumps(meta, indent=2), encoding="utf-8"
    )
    (OUT / "Mellow Upright.sfz").write_text(
        "\n".join(sfz_lines) + "\n", encoding="utf-8"
    )

    total = sum(p.stat().st_size for p in OUT.glob("*.wav"))
    print("notes", len(notes_sorted), "samples", len(samples))
    print("size_mb", round(total / 1024 / 1024, 1))
    print("out", OUT)


if __name__ == "__main__":
    main()
