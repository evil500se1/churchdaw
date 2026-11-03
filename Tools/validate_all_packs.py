import os
import sys
import subprocess

TARGET_PADS = [
    "Drone Ocean View",
    "up2_pad_angelic",
    "up2_pad_cinematic",
    "up2_pad_happy",
    "up2_pad_majestic",
    "up2_pad_odyssey",
    "up2_pad_organic",
    "up2_pad_sheltered",
    "up2_pad_space"
]

TARGET_PIANOS = [
    "EP7 Tines Amped",
    "EP8 Nefertiti",
    "Electric Grand 1",
    "Grand Imperial XL",
    "Grand Lady D L",
    "Italian Grand XL",
    "Mellow Upright XL",
    "Queen Upright L",
    "Rain Piano L",
    "Romantic Upright L",
    "Royal Grand 3D XL",
    "Saloon Upright L",
    "Silver Grand XL",
    "Velvet Grand XL",
    "Wurlitzer 1"
]

def main():
    root = r"C:\Users\Administrator\MinhaDAW"
    val_script = os.path.join(root, "Tools", "ValidateTimbrePack.py")

    all_passed = True
    total = len(TARGET_PADS) + len(TARGET_PIANOS)
    count = 0

    print("==================================================")
    print(f"VALIDATING ALL {total} TIMBRE PACKS (INDIVIDUAL ASSERTION)")
    print("==================================================")

    print("\n--- PADS (9 Target Packs) ---")
    for pad in TARGET_PADS:
        pack_path = os.path.join(root, "Pads", pad)
        res = subprocess.run([sys.executable, val_script, "--pack", pack_path], capture_output=True, text=True)
        is_pass = (res.returncode == 0) and ("RESULT: 100% PASS" in res.stdout)
        if is_pass:
            count += 1
            print(f"[PASS 100%] Pads\\{pad} (exit code 0)")
        else:
            all_passed = False
            print(f"[FAIL] Pads\\{pad} (exit code {res.returncode})")
            print(res.stdout)
            print(res.stderr)

    print("\n--- PIANOS (15 Target Packs) ---")
    for piano in TARGET_PIANOS:
        pack_path = os.path.join(root, "Pianos", piano)
        res = subprocess.run([sys.executable, val_script, "--pack", pack_path], capture_output=True, text=True)
        is_pass = (res.returncode == 0) and ("RESULT: 100% PASS" in res.stdout)
        if is_pass:
            count += 1
            print(f"[PASS 100%] Pianos\\{piano} (exit code 0)")
        else:
            all_passed = False
            print(f"[FAIL] Pianos\\{piano} (exit code {res.returncode})")
            print(res.stdout)
            print(res.stderr)

    print("\n==================================================")
    print(f"RESULT: {count}/{total} TIMBRE PACKS PASSED VALIDATION")
    print("==================================================")

    if not all_passed:
        sys.exit(1)
    print("ALL 24 TIMBRE PACKS PASSED VALIDATION SUCCESSFULLY!")

if __name__ == '__main__':
    main()
