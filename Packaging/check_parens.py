from pathlib import Path

path = Path(__file__).resolve().parents[1] / "Source" / "AndroidAudioMidiCenter.h"
text = path.read_text(encoding="utf-8")
lines = text.splitlines()

# Fix known broken setButtonText lines missing closing paren for setButtonText
fixes = 0
out = []
for line in lines:
    # Pattern: setButtonText (juce::String::fromUTF8 (u8"...");
    # missing ) before ;
    if "setButtonText" in line and "fromUTF8" in line and line.rstrip().endswith('";'):
        # ends with ";  meaning string closed then ; without closing setButtonText
        # actually ends with u8"...";
        stripped = line.rstrip()
        if stripped.endswith('";') and stripped.count("(") == stripped.count(")") + 1:
            line = stripped[:-1] + ");" + ("\n" if line.endswith("\n") else "")
            # wait we're iterating without keepends
            line = stripped[:-1] + ");"
            fixes += 1
            print("fixed:", line[:100])
    out.append(line)

path.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
print("fixes", fixes)

# re-check balance per line for fromUTF8 lines
lines = path.read_text(encoding="utf-8").splitlines()
for i, line in enumerate(lines, 1):
    if "fromUTF8" not in line and "setButtonText" not in line:
        continue
    bal = 0
    in_str = False
    esc = False
    for c in line:
        if esc:
            esc = False
            continue
        if c == "\\":
            esc = True
            continue
        if c == '"':
            in_str = not in_str
            continue
        if in_str:
            continue
        if c == "(":
            bal += 1
        elif c == ")":
            bal -= 1
    if bal != 0:
        print(f"L{i} bal={bal}: {line.strip()[:160]}")
