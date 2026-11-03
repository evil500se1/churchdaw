import os
import sys
import struct
import json
import re
import subprocess
import soundfile as sf
from concurrent.futures import ThreadPoolExecutor

def midi_to_note_name(m: int) -> str:
    # Ableton pitch convention: C3 = 60
    # C-2 = 0, C-1 = 12, C0 = 24, C1 = 36, C2 = 48, C3 = 60, etc.
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
    idx, root, s, nkx_path, ncw_exe, out_dir, tmp_dir = args
    note_name = midi_to_note_name(root)
    wav_filename = f"up2_pad_majestic-{note_name}-V127-01.wav"
    wav_path = os.path.join(out_dir, wav_filename)
    
    with open(nkx_path, 'rb') as f_nkx:
        f_nkx.seek(s['offset'] + 31)
        payload = f_nkx.read(s['size'])
        
    tmp_ncw = os.path.join(tmp_dir, f"majestic_{root}.ncw")
    tmp_wav = os.path.join(tmp_dir, f"majestic_{root}.wav")
    
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
    
    print(f"[{idx+1}/28] Converted root {root} ({note_name}) -> {wav_filename}", flush=True)
    return (root, note_name, wav_filename)

def main():
    project_root = r"C:\Users\Administrator\MinhaDAW"
    samples_dir = r"C:\Users\Administrator\Downloads\Telegram Desktop\extracted_ultimate\Ultimate Pianos 2\Samples"
    nkc_path = os.path.join(samples_dir, "UP2 Pads.nkc")
    nkx_path = os.path.join(samples_dir, "UP2 Pads.nkx")
    ncw_exe = r"C:\Users\Administrator\.cargo\bin\ncw-convert.exe"
    
    out_dir = os.path.join(project_root, "Pads", "up2_pad_majestic")
    tmp_dir = os.path.join(project_root, "Staging", "tmp_majestic")
    
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(tmp_dir, exist_ok=True)
    
    # Clean out existing directory
    for f in os.listdir(out_dir):
        try:
            os.remove(os.path.join(out_dir, f))
        except Exception as e:
            print(f"Warning deleting {f}: {e}", flush=True)

    nkc_samples = parse_nkc(nkc_path)
    majestic_items = []
    
    for s in nkc_samples:
        fn = s['fname']
        if 'up2_pad_majestic' in fn.lower():
            nums = re.findall(r'\b\d{3}\b', fn)
            if nums:
                root = int(nums[0])
                majestic_items.append({'root': root, 'sample': s})
                
    majestic_items.sort(key=lambda x: x['root'])
    print(f"Found {len(majestic_items)} samples for up2_pad_majestic", flush=True)
    
    if len(majestic_items) != 28:
        print(f"ERROR: Expected 28 samples, found {len(majestic_items)}", flush=True)
        sys.exit(1)
        
    sorted_roots = [item['root'] for item in majestic_items]
    print(f"Roots ({len(sorted_roots)}): {sorted_roots}", flush=True)
    
    tasks = []
    for idx, item in enumerate(majestic_items):
        root = item['root']
        s = item['sample']
        tasks.append((idx, root, s, nkx_path, ncw_exe, out_dir, tmp_dir))
        
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(convert_sample, tasks))
        
    if any(r is None for r in results):
        print("ERROR: One or more sample conversions failed!", flush=True)
        sys.exit(1)
        
    # Calculate key ranges
    K = len(sorted_roots)
    json_samples = []
    sfz_regions = []
    
    for i, root in enumerate(sorted_roots):
        note_name = midi_to_note_name(root)
        wav_filename = f"up2_pad_majestic-{note_name}-V127-01.wav"
        
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
        
        sfz_regions.append(f"<region> sample={wav_filename} pitch_keycenter={root} lokey={k_low} hikey={k_high} lovel=0 hivel=127")
        
    json_data = {
        "name": "up2_pad_majestic",
        "raw_label": "up2_pad_majestic",
        "category": "PADS",
        "default_attack": 0.35,
        "default_release": 2.5,
        "loop_enabled": True,
        "sample_count": K,
        "samples": json_samples
    }
    
    json_path = os.path.join(out_dir, "up2_pad_majestic.json")
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
    
    sfz_path = os.path.join(out_dir, "up2_pad_majestic.sfz")
    with open(sfz_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(sfz_lines) + '\n')
    print(f"Generated {sfz_path}", flush=True)
    
    print("\n--- Running ValidateTimbrePack.py on Pads/up2_pad_majestic ---", flush=True)
    val_script = os.path.join(project_root, 'Tools', 'ValidateTimbrePack.py')
    val_res = subprocess.run([sys.executable, val_script, '--pack', out_dir], capture_output=True, text=True)
    print(val_res.stdout, flush=True)
    if val_res.returncode != 0:
        print("FAIL: Validation failed!", flush=True)
        sys.exit(1)
    else:
        print("SUCCESS: up2_pad_majestic 100% validated!", flush=True)

if __name__ == '__main__':
    main()
