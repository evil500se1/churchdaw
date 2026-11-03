import os
import re

def fix_sfz_file(sfz_path):
    with open(sfz_path, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()

    modified = False
    new_lines = []

    for line in lines:
        if 'sample=' in line:
            # Find sample= up to next attribute or end of line
            # Attributes in sfz regions: pitch_keycenter, lokey, hikey, lovel, hivel, tune, volume, etc.
            m = re.search(r'sample=(.*?)(?=\s+[a-zA-Z_0-9]+=|[\r\n]|$)', line)
            if m:
                raw_val = m.group(1).strip()
                # Clean any quotes from raw_val
                clean_val = raw_val.replace('"', '').replace("'", '').strip()
                correct_sample_str = f'sample="{clean_val}"'
                
                # Replace sample=... in line with sample="clean_val"
                old_sample_part = f'sample={raw_val}'
                if old_sample_part in line and old_sample_part != correct_sample_str:
                    line = line.replace(old_sample_part, correct_sample_str)
                    modified = True
        new_lines.append(line)

    if modified:
        with open(sfz_path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)
        print(f"Fixed quotes in: {sfz_path}")

def main():
    root_dirs = [r"C:\Users\Administrator\MinhaDAW\Pads", r"C:\Users\Administrator\MinhaDAW\Pianos"]
    for rd in root_dirs:
        for root, dirs, files in os.walk(rd):
            for file in files:
                if file.endswith('.sfz'):
                    fix_sfz_file(os.path.join(root, file))

if __name__ == '__main__':
    main()
