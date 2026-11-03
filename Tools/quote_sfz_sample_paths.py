import os
import re

def process_sfz(sfz_path):
    with open(sfz_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    modified = False

    def replace_sample(match):
        nonlocal modified
        full_match = match.group(0)
        sample_val = match.group(1)
        if sample_val.startswith('"') and sample_val.endswith('"'):
            return full_match
        if sample_val.startswith("'") and sample_val.endswith("'"):
            unquoted = sample_val[1:-1]
            modified = True
            return f'sample="{unquoted}"'
        modified = True
        return f'sample="{sample_val}"'

    # Match sample= capturing group 1: quoted or unquoted filename up to space or end of line
    new_content = re.sub(r'sample=("(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|[^\s>]+)', replace_sample, content)

    if modified:
        with open(sfz_path, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"Updated {sfz_path}")

def main():
    root_dirs = [r"C:\Users\Administrator\MinhaDAW\Pads", r"C:\Users\Administrator\MinhaDAW\Pianos"]
    for rd in root_dirs:
        for root, dirs, files in os.walk(rd):
            for file in files:
                if file.endswith('.sfz'):
                    sfz_path = os.path.join(root, file)
                    process_sfz(sfz_path)

if __name__ == '__main__':
    main()
