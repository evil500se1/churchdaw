#!/usr/bin/env python3
"""Fix corrupted juce::String::fromUTF8 nests in AndroidAudioMidiCenter.h"""
from __future__ import annotations

import re
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "Source" / "AndroidAudioMidiCenter.h"
s = path.read_text(encoding="utf-8")
original = s

# 1) Collapse any remaining nested fromUTF8(fromUTF8(...))
prev = None
while prev != s:
    prev = s
    s = re.sub(
        r"juce::String::fromUTF8\s*\(\s*juce::String::fromUTF8\s*\(",
        "juce::String::fromUTF8 (",
        s,
    )

# 2) Remove leftover extra ')' after fromUTF8(u8"...")
s = re.sub(
    r'(juce::String::fromUTF8\s*\(\s*u8"[^"]*"\s*\))\)',
    r"\1",
    s,
)

# 3) Fix broken inHint multi-line by line-scan replace
marker_start = "inHint.setText (juce::String::fromUTF8 ("
idx = s.find(marker_start)
if idx >= 0:
    # Find end of statement
    end = s.find("juce::dontSendNotification);", idx)
    if end > idx:
        end = end + len("juce::dontSendNotification);")
        replacement = (
            'inHint.setText (juce::String::fromUTF8 (u8"Entrada desativada\\n'
            'O ChurchDAW funciona s\\u00f3 com instrumentos virtuais (sem microfone)."),\n'
            "                        juce::dontSendNotification);"
        )
        # only replace if still broken (contains nested fromUTF8 or plain ASCII line break concat)
        chunk = s[idx:end]
        if "juce::String::fromUTF8" in chunk[len(marker_start) :] or "Entrada desativada\\n" in chunk:
            s = s[:idx] + replacement + s[end:]
            print("fixed inHint")

# 4) Fix outHint dual adjacent fromUTF8 without operator
old_out = (
    'outHint.setText (\n'
    '                juce::String::fromUTF8 (u8"Nenhuma interface USB em uso. O \\u00e1udio vai para a ")\n'
    '                    juce::String::fromUTF8 (u8"sa\\u00edda padr\\u00e3o do Android."),'
)
# try flexible match
m = re.search(
    r"outHint\.setText\s*\(\s*"
    r'juce::String::fromUTF8\s*\(\s*u8"Nenhuma interface USB em uso\. O \\u00e1udio vai para a "\s*\)\s*'
    r'juce::String::fromUTF8\s*\(\s*u8"sa\\u00edda padr\\u00e3o do Android\."\s*\)\s*,',
    s,
)
if m:
    s = (
        s[: m.start()]
        + 'outHint.setText (\n'
        + '                juce::String::fromUTF8 (u8"Nenhuma interface USB em uso. '
        + 'O \\u00e1udio vai para a sa\\u00edda padr\\u00e3o do Android."),'
        + s[m.end() :]
    )
    print("fixed outHint")
else:
    # line-based fallback
    lines = s.splitlines(keepends=True)
    out = []
    i = 0
    while i < len(lines):
        if (
            "outHint.setText" in lines[i]
            and i + 2 < len(lines)
            and "Nenhuma interface USB" in lines[i + 1]
            and "padr" in lines[i + 2]
        ):
            out.append(
                '            outHint.setText (\n'
                '                juce::String::fromUTF8 (u8"Nenhuma interface USB em uso. '
                'O \\u00e1udio vai para a sa\\u00edda padr\\u00e3o do Android."),\n'
            )
            # skip until dontSendNotification line consumed after
            i += 1
            while i < len(lines) and "dontSendNotification" not in lines[i]:
                i += 1
            if i < len(lines):
                out.append(lines[i])  # keep dontSendNotification line
            i += 1
            print("fixed outHint (fallback)")
            continue
        out.append(lines[i])
        i += 1
    s = "".join(out)

path.write_text(s, encoding="utf-8", newline="\n")
print(f"wrote {path}")
print(f"changed: {s != original}")
print(f"nested left: {s.count('fromUTF8 (juce::String::fromUTF8')}")
print(
    "double-close after u8 left:",
    len(re.findall(r'fromUTF8\s*\(\s*u8"[^"]*"\s*\)\)', s)),
)

for i, line in enumerate(s.splitlines(), 1):
    if "fromUTF8" not in line:
        continue
    # leftover double close
    if re.search(r'u8"[^"]*"\s*\)\)', line):
        print(f"  DBL L{i}: {line.strip()[:140]}")
    # two fromUTF8 on one line or next weird
    if line.count("fromUTF8") > 1:
        print(f"  MULTI L{i}: {line.strip()[:140]}")
