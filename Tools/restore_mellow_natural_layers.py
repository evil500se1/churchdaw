"""Restore Mellow Upright to natural root notes only + complete 9 velocity layers.

Removes offline pitch-shifted chromatic samples that cause metallic/aliasing sound.
Keeps full velocity layers on each of the 13 original roots so no wrong-root fallback.
"""
from __future__ import annotations

import json
import os
import re
import shutil
from pathlib import Path

BASE = Path(os.environ["APPDATA"]) / r"ChurchDAW\Libraries\church-live\Pianos\Mellow Upright"
KEEP = Path(os.environ["TEMP"]) / "mellow_natural_layers"

# Original recorded roots (Ableton C3 = 60)
ROOTS = [24, 31, 38, 45, 52, 59, 66, 73, 80, 87, 94, 101, 108]
VELS = [14, 28, 42, 56, 71, 85, 99, 113, 127]
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


def main() -> None:
    by: dict[tuple[int, int], Path] = {}
    for p in BASE.glob("*.wav"):
        m = re.match(r"Mellow Upright-(.+)-V(\d+)-(\d+)\.wav$", p.name)
        if not m:
            continue
        note, vel = m.group(1), int(m.group(2))
        by[(name_to_midi(note), vel)] = p

    if KEEP.exists():
        shutil.rmtree(KEEP)
    KEEP.mkdir(parents=True)

    kept = 0
    for root in ROOTS:
        for vel in VELS:
            src = by.get((root, vel))
            if src is None:
                cands = [(v, p) for (m, v), p in by.items() if m == root]
                if not cands:
                    print("MISSING root", root)
                    continue
                cands.sort(key=lambda x: abs(x[0] - vel))
                src = cands[0][1]
                print("fallback", root, vel, "from", cands[0][0])
            name = midi_to_name(root)
            dst = KEEP / f"Mellow Upright-{name}-V{vel:03d}-01.wav"
            shutil.copy2(src, dst)
            kept += 1

    print("kept", kept)

    notes_sorted = [midi_to_name(r) for r in ROOTS]
    midis = list(ROOTS)

    def key_range(i: int):
        m = midis[i]
        lo = 0 if i == 0 else (midis[i - 1] + m) // 2 + 1
        hi = 127 if i == len(midis) - 1 else (m + midis[i + 1]) // 2
        return lo, hi

    samples = []
    sfz = [
        "<group>",
        "ampeg_attack=0.001",
        "ampeg_release=1.20",
        "loop_mode=no_loop",
        "",
    ]
    for i, note in enumerate(notes_sorted):
        lo, hi = key_range(i)
        midi = midis[i]
        prev = -1
        for vel in VELS:
            fname = f"Mellow Upright-{note}-V{vel:03d}-01.wav"
            vel_lo = prev + 1
            samples.append(
                {
                    "file": fname,
                    "midi_root": midi,
                    "note_name": note,
                    "key_low": lo,
                    "key_high": hi,
                    "vel_low": vel_lo,
                    "vel_high": vel,
                }
            )
            sfz.append(
                f"<region> sample={fname} pitch_keycenter={midi} "
                f"lokey={lo} hikey={hi} lovel={vel_lo} hivel={vel}"
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
    (KEEP / "Mellow Upright.json").write_text(
        json.dumps(meta, indent=2), encoding="utf-8"
    )
    (KEEP / "Mellow Upright.sfz").write_text("\n".join(sfz) + "\n", encoding="utf-8")

    shutil.rmtree(BASE)
    shutil.copytree(KEEP, BASE)
    print("installed", len(list(BASE.glob("*.wav"))))

    idx = Path(os.environ["APPDATA"]) / "ChurchDAW" / "timbre-index-v2.bin"
    if idx.exists():
        idx.unlink()
        print("cleared index")


if __name__ == "__main__":
    main()
