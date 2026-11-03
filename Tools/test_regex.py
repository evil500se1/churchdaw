import os
import re

def test_sfz(sfz_path):
    with open(sfz_path, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
    for line in lines:
        line = line.strip()
        if 'sample=' in line:
            m = re.search(r'sample=(?:"([^"]+)"|([^\r\n]+?)(?=\s+[a-zA-Z_0-9]+=|[\r\n]|$))', line)
            if m:
                sample_file = (m.group(1) or m.group(2)).strip()
                print("Line:", line)
                print("Extracted sample:", repr(sample_file))
            break

test_sfz(r"C:\Users\Administrator\MinhaDAW\Pianos\Rain Piano L\Rain Piano L.sfz")
