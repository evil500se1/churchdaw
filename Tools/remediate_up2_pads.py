import os
import sys
import struct
import json
import re
import subprocess
import soundfile as sf
from concurrent.futures import ThreadPoolExecutor

def midi_to_note_name(m: int) -> str:
    note_names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
    oct_val = (m // 12) - 2
    semi = m % 12
    return f"{note_names[semi]}{oct_val}"

def parse_nkc(nkc_path: str):
    data_nkc = open(nkc_path, 'rb').read()
    total_files = struct.unpack('<I', data_nkc[0x20:0x24])[0]
    pos = 0x28
    count = 0
    samples = []
    while pos < len(data_nkc) - 20 and count < total_files:
        offset, size = struct.unpack('<II', data_nkc[pos:pos+8])
        nlen = struct.unpack('<I', data_nkc[pos+20:pos+24])[0]
        if nlen == 0 or pos + 24 + nlen*2 > len(data_nkc):
            break
        fname = data_nkc[pos+24 : pos+24+nlen*2].decode('utf-16le')
        samples.append({'offset': offset, 'size': size, 'fname': fname})
        count += 1
        pos += 24 + nlen*2
    return samples

def convert_sample(args):
    idx, name, root, s, nkx_path, ncw_exe, out_dir, tmp_dir = args
    note_name = midi_to_note_name(root)
    wav_filename = f"{name}-{note_name}-V127-01.wav"
    wav_path = os.path.join(out_dir, wav_filename)
    
    with open(nkx_path, 'rb') as f_nkx:
        f_nkx.seek(s['offset'] + 31)
        payload = f_nkx.read(s['size'])
        
    tmp_ncw = os.path.join(tmp_dir, f"{name}_{root}.ncw")
    tmp_wav = os.path.join(tmp_dir, f"{name}_{root}.wav")
    
    with open(tmp_ncw, 'wb') as f_ncw:
        f_ncw.write(payload)
        
    res = subprocess.run([ncw_exe, tmp_ncw, tmp_wav], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"ERROR converting {s['fname']}: {res.stderr}", flush=True)
        return None
        
    audio_data, sr = sf.read(tmp_wav)
    sf.write(wav_path, audio_data, 44100, subtype='PCM_24')
    
    if os.path.exists(tmp_ncw): os.remove(tmp_ncw)
    if os.path.exists(tmp_wav): os.remove(tmp_wav)
    
    print(f"[{name}] [{idx+1}/28] Converted root {root} ({note_name}) -> {wav_filename}", flush=True)
    return (root, note_name, wav_filename)

def build_pad(name, nkc_samples, nkc_path, nkx_path, ncw_exe, project_root, tmp_dir, pool):
    out_dir = os.path.join(project_root, "Pads", name)
    os.makedirs(out_dir, exist_ok=True)
    
    # Check if already complete and passes validation
    val_script = os.path.join(project_root, 'Tools', 'ValidateTimbrePack.py')
    val_res = subprocess.run([sys.executable, val_script, '--pack', out_dir], capture_output=True, text=True)
    if val_res.returncode == 0:
        print(f"[{name}] Already 100% valid! Skipping re-conversion.", flush=True)
        return True

    # Clean directory if invalid
    for f in os.listdir(out_dir):
        try:
            os.remove(os.path.join(out_dir, f))
        except Exception as e:
            print(f"Warning deleting {f}: {e}", flush=True)

    items = []
    for s in nkc_samples:
        fn = s['fname']
        if name.lower() in fn.lower():
            nums = re.findall(r'\b\d{3}\b', fn)
            if nums:
                root = int(nums[0])
                items.append({'root': root, 'sample': s})
                
    items.sort(key=lambda x: x['root'])
    print(f"Found {len(items)} samples for {name}", flush=True)
    
    if len(items) != 28:
        print(f"ERROR: Expected 28 samples for {name}, found {len(items)}", flush=True)
        return False
        
    sorted_roots = [item['root'] for item in items]
    print(f"Roots for {name} ({len(sorted_roots)}): {sorted_roots}", flush=True)
    
    tasks = []
    for idx, item in enumerate(items):
        root = item['root']
        s = item['sample']
        tasks.append((idx, name, root, s, nkx_path, ncw_exe, out_dir, tmp_dir))
        
    results = list(pool.map(convert_sample, tasks))
    if any(r is None for r in results):
        print(f"ERROR: One or more sample conversions failed for {name}!", flush=True)
        return False
        
    # Calculate key ranges
    K = len(sorted_roots)
    json_samples = []
    sfz_regions = []
    
    for i, root in enumerate(sorted_roots):
        note_name = midi_to_note_name(root)
        wav_filename = f"{name}-{note_name}-V127-01.wav"
        
        if i == 0:
            k_low = 0
        else:
            k_low = (sorted_roots[i-1] + sorted_roots[i]) // 2 + 1
            
        if i == K - 1:
            k_high = 127
        else:
            k_high = (sorted_roots[i] + sorted_roots[i+1]) // 2
            
        json_samples.append({
            "file": wav_filename,
            "midi_root": root,
            "note_name": note_name,
            "key_low": k_low,
            "key_high": k_high,
            "vel_low": 0,
            "vel_high": 127
        })
        
        sfz_regions.append(f'<region> sample="{wav_filename}" pitch_keycenter={root} lokey={k_low} hikey={k_high} lovel=0 hivel=127')
        
    json_data = {
        "name": name,
        "raw_label": name,
        "category": "PADS",
        "default_attack": 0.35,
        "default_release": 2.5,
        "loop_enabled": True,
        "sample_count": K,
        "samples": json_samples
    }
    
    json_path = os.path.join(out_dir, f"{name}.json")
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(json_data, f, indent=2)
    print(f"Generated {json_path}", flush=True)
    
    sfz_lines = [
        "<group>",
        "ampeg_attack=0.350",
        "ampeg_release=2.50",
        "loop_mode=loop_continuous",
        ""
    ] + sfz_regions
    
    sfz_path = os.path.join(out_dir, f"{name}.sfz")
    with open(sfz_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(sfz_lines) + '\n')
    print(f"Generated {sfz_path}", flush=True)
    
    val_res = subprocess.run([sys.executable, val_script, '--pack', out_dir], capture_output=True, text=True)
    print(val_res.stdout, flush=True)
    return val_res.returncode == 0

def main():
    project_root = r"C:\Users\Administrator\MinhaDAW"
    samples_dir = r"C:\Users\Administrator\Downloads\Telegram Desktop\extracted_ultimate\Ultimate Pianos 2\Samples"
    nkc_path = os.path.join(samples_dir, "UP2 Pads.nkc")
    nkx_path = os.path.join(samples_dir, "UP2 Pads.nkx")
    ncw_exe = r"C:\Users\Administrator\.cargo\bin\ncw-convert.exe"
    tmp_dir = os.path.join(project_root, "Staging", "tmp_remediate")
    os.makedirs(tmp_dir, exist_ok=True)
    
    nkc_samples = parse_nkc(nkc_path)
    
    # All UP2 Pads to check/build
    target_pads = [
        "up2_pad_angelic",
        "up2_pad_cinematic",
        "up2_pad_happy",
        "up2_pad_majestic",
        "up2_pad_odyssey",
        "up2_pad_organic",
        "up2_pad_sheltered",
        "up2_pad_space"
    ]
    
    with ThreadPoolExecutor(max_workers=8) as pool:
        for pad_name in target_pads:
            ok = build_pad(pad_name, nkc_samples, nkc_path, nkx_path, ncw_exe, project_root, tmp_dir, pool)
            if not ok:
                print(f"FAIL: Remediation failed for {pad_name}", flush=True)
                sys.exit(1)
                
    print("\nSUCCESS: All UP2 pads built and 100% validated!", flush=True)

if __name__ == '__main__':
    main()
