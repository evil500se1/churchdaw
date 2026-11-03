from pathlib import Path
import re

p = Path(r"C:\Users\Administrator\MinhaDAW\Source\AndroidAudioMidiCenter.h")
t = p.read_text(encoding="utf-8")

t = t.replace(
    """        if (auto* type = host.deviceManager.getCurrentDeviceTypeObject())
            type->playTestSound();
""",
    """        host.deviceManager.playTestSound();
""",
)


def to_u8_literal(text: str) -> str:
    out = []
    for ch in text:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif o < 128:
            out.append(ch)
        else:
            out.append("\\u%04x" % o)
    return 'juce::String::fromUTF8 (u8"' + "".join(out) + '")'


def decode_c_escapes(s: str) -> str:
    # Interpret \xNN as bytes then UTF-8
    raw = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x" and i + 3 < len(s):
                # greedy hex like C++ (bad) — only take 2 digits
                raw.append(int(s[i + 2 : i + 4], 16))
                i += 4
                continue
            if n == "n":
                raw.append(ord("\n"))
                i += 2
                continue
            if n == "t":
                raw.append(ord("\t"))
                i += 2
                continue
            if n == "\\":
                raw.append(ord("\\"))
                i += 2
                continue
            if n == '"':
                raw.append(ord('"'))
                i += 2
                continue
            if n == "e":
                # \xe2 etc already handled by x
                raw.append(ord(n))
                i += 2
                continue
        raw.append(ord(s[i]) if ord(s[i]) < 256 else 63)
        i += 1
    return bytes(raw).decode("utf-8", errors="replace")


def fix_string_literal(m: re.Match) -> str:
    s = m.group(1)
    if "\\x" not in s:
        return m.group(0)
    try:
        text = decode_c_escapes(s)
        return to_u8_literal(text)
    except Exception as e:
        print("fail", s[:60], e)
        return m.group(0)


# Only replace double-quoted string literals containing \x
t2 = re.sub(r'(?<!u8)"((?:\\.|[^"\\])*)"', fix_string_literal, t)

p.write_text(t2, encoding="utf-8")
print("remaining \\x count:", t2.count("\\x"))
for i, line in enumerate(t2.splitlines(), 1):
    if "\\x" in line and "u8" not in line:
        print(i, line[:140])
print("done")
