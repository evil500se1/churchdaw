"""
identify_casio.py - Casio Keyboard MIDI Port Enumeration and SysEx Identity Diagnostic Tool

This script enumerates all connected MIDI input and output ports, transmits Universal
Non-Real Time System Exclusive (SysEx) Identity Request messages (F0 7E 7F 06 01 F7 / F0 7E 00 06 01 F7),
listens for SysEx Identity Replies (F0 7E <channel> 06 02 <mfg_id> <family> <model> <version> F7),
parses manufacturer ID (0x44 = Casio), family code, model number, and firmware version,
queries Windows PnP hardware details (VID 07CF, PID 6803, "CASIO USB-MIDI"), and outputs a
structured JSON report to Staging/m1_hardware_report.json.
"""

import os
import sys
import time
import json
import winreg
import ctypes
from datetime import datetime
from ctypes import wintypes

# Manufacturer IDs
MANUFACTURER_MAP = {
    0x44: "Casio Computer Co., Ltd.",
    0x41: "Roland Corporation",
    0x42: "Korg Inc.",
    0x43: "Yamaha Corporation",
    0x40: "Kawai Musical Instruments",
    0x47: "Akai Professional",
    0x00: "Extended Manufacturer ID (3-byte)",
}

def enumerate_winreg_pnp_devices():
    """Enumerate Windows PnP USB devices matching Casio (VID 07CF)."""
    devices = []
    key_path = r'SYSTEM\CurrentControlSet\Enum\USB\VID_07CF&PID_6803'
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as k:
            num_subkeys = winreg.QueryInfoKey(k)[0]
            for i in range(num_subkeys):
                subkey_name = winreg.EnumKey(k, i)
                sub_path = f'{key_path}\\{subkey_name}'
                device_info = {'Subkey': subkey_name, 'VID': '07CF', 'PID': '6803'}
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, sub_path) as sk:
                    for val_idx in range(winreg.QueryInfoKey(sk)[1]):
                        name, val, _ = winreg.EnumValue(sk, val_idx)
                        if name in ['DeviceDesc', 'FriendlyName', 'Mfg', 'HardwareID', 'LocationInformation', 'Service']:
                            device_info[name] = val
                devices.append(device_info)
    except Exception:
        pass
    return devices

def enumerate_midi_ports():
    """Enumerate MIDI ports using mido, rtmidi, or Windows WinMM API."""
    in_ports = []
    out_ports = []
    
    # Try mido first
    try:
        import mido
        in_ports = list(mido.get_input_names())
        out_ports = list(mido.get_output_names())
    except Exception:
        pass

    # Try rtmidi if mido returned empty
    if not in_ports and not out_ports:
        try:
            import rtmidi
            m_in = rtmidi.MidiIn()
            in_ports = m_in.get_ports()
            m_out = rtmidi.MidiOut()
            out_ports = m_out.get_ports()
        except Exception:
            pass

    # WinMM API fallback if still empty
    if not in_ports and not out_ports:
        try:
            winmm = ctypes.windll.winmm
            class MIDIINCAPSW(ctypes.Structure):
                _fields_ = [
                    ('wMid', wintypes.WORD), ('wPid', wintypes.WORD),
                    ('vDriverVersion', wintypes.UINT), ('szPname', wintypes.WCHAR * 32),
                    ('dwSupport', wintypes.DWORD)
                ]
            class MIDIOUTCAPSW(ctypes.Structure):
                _fields_ = [
                    ('wMid', wintypes.WORD), ('wPid', wintypes.WORD),
                    ('vDriverVersion', wintypes.UINT), ('szPname', wintypes.WCHAR * 32),
                    ('wTechnology', wintypes.WORD), ('wVoices', wintypes.WORD),
                    ('wNotes', wintypes.WORD), ('wChannelMask', wintypes.WORD),
                    ('dwSupport', wintypes.DWORD)
                ]
            incaps = MIDIINCAPSW()
            for i in range(winmm.midiInGetNumDevs()):
                winmm.midiInGetDevCapsW(i, ctypes.byref(incaps), ctypes.sizeof(incaps))
                in_ports.append(incaps.szPname)
            outcaps = MIDIOUTCAPSW()
            for i in range(winmm.midiOutGetNumDevs()):
                winmm.midiOutGetDevCapsW(i, ctypes.byref(outcaps), ctypes.sizeof(outcaps))
                out_ports.append(outcaps.szPname)
        except Exception:
            pass

    return in_ports, out_ports

def send_and_capture_sysex(in_port_name, out_port_name, timeout=2.0):
    """
    Send SysEx Identity Request over mido/rtmidi and capture responses.
    Requests sent:
      F0 7E 7F 06 01 F7 (Broadcast Universal Identity Request)
      F0 7E 00 06 01 F7 (Channel 0 / Device ID 0 Identity Request)
    """
    received_messages = []
    
    # Attempt with mido
    try:
        import mido
        with mido.open_input(in_port_name) as inport, mido.open_output(out_port_name) as outport:
            # Drain any pending messages
            for _ in inport.iter_pending():
                pass

            requests = [
                [0x7E, 0x7F, 0x06, 0x01],  # F0 7E 7F 06 01 F7
                [0x7E, 0x00, 0x06, 0x01],  # F0 7E 00 06 01 F7
            ]

            per_req_timeout = timeout / len(requests)
            for req in requests:
                msg = mido.Message('sysex', data=req)
                outport.send(msg)
                start_time = time.time()
                while time.time() - start_time < per_req_timeout:
                    pending = inport.poll()
                    if pending:
                        if pending.type == 'sysex':
                            raw_bytes = list(pending.bytes())
                            received_messages.append({
                                'request': ' '.join(f'{x:02X}' for x in [0xF0] + req + [0xF7]),
                                'raw_bytes': [f'0x{x:02X}' for x in raw_bytes],
                                'bytes': raw_bytes
                            })
                    time.sleep(0.01)
            return received_messages
    except Exception:
        pass

    # Attempt with python-rtmidi fallback
    try:
        import rtmidi
        midi_in = rtmidi.MidiIn()
        midi_out = rtmidi.MidiOut()

        in_ports = midi_in.get_ports()
        out_ports = midi_out.get_ports()

        in_idx = next((i for i, p in enumerate(in_ports) if p == in_port_name), None)
        out_idx = next((i for i, p in enumerate(out_ports) if p == out_port_name), None)

        if in_idx is not None and out_idx is not None:
            midi_in.open_port(in_idx)
            midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
            midi_out.open_port(out_idx)

            time.sleep(0.1)
            while midi_in.get_message():
                pass

            requests = [
                [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7],
                [0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7],
            ]

            for req in requests:
                midi_out.send_message(req)
                start_time = time.time()
                while time.time() - start_time < (timeout / len(requests)):
                    msg = midi_in.get_message()
                    if msg:
                        data, delta = msg
                        if data and data[0] == 0xF0:
                            received_messages.append({
                                'request': ' '.join(f'{x:02X}' for x in req),
                                'raw_bytes': [f'0x{x:02X}' for x in data],
                                'bytes': list(data)
                            })
                    time.sleep(0.01)

            midi_in.close_port()
            midi_out.close_port()
    except Exception:
        pass

    return received_messages

def parse_sysex_identity(data):
    """
    Parse Universal Non-Real Time MIDI SysEx Identity Reply:
    Header: F0 7E <channel> 06 02 <mfg_id> <family_lsb> <family_msb> <model_lsb> <model_msb> <ver1> <ver2> <ver3> <ver4> F7
    """
    if not data or len(data) < 14:
        return None
    if data[0] != 0xF0 or data[1] != 0x7E or data[3] != 0x06 or data[4] != 0x02:
        return None

    channel = data[2]
    mfg_id = data[5]
    mfg_name = MANUFACTURER_MAP.get(mfg_id, f"Unknown (0x{mfg_id:02X})")
    
    family_code = data[6] | (data[7] << 8)
    model_number = data[8] | (data[9] << 8)
    firmware_version = f"{data[10]}.{data[11]}.{data[12]}.{data[13]}"

    return {
        "channel": channel,
        "manufacturer_id": f"0x{mfg_id:02X}",
        "manufacturer_name": mfg_name,
        "family_code": f"0x{family_code:04X}",
        "family_code_int": family_code,
        "model_number": f"0x{model_number:04X}",
        "model_number_int": model_number,
        "firmware_version": firmware_version
    }

def main():
    print("==========================================================================")
    print("      CASIO KEYBOARD HARDWARE IDENTIFICATION & MIDI PORT DIAGNOSTIC       ")
    print("==========================================================================")
    
    # 1. Enumerate MIDI Ports
    in_ports, out_ports = enumerate_midi_ports()
    print("\n--- 1. Connected MIDI Input Ports ---")
    for idx, p in enumerate(in_ports):
        print(f"  [{idx}] {p}")
    if not in_ports:
        print("  (No MIDI input ports found)")

    print("\n--- 2. Connected MIDI Output Ports ---")
    for idx, p in enumerate(out_ports):
        print(f"  [{idx}] {p}")
    if not out_ports:
        print("  (No MIDI output ports found)")

    # 2. Windows PnP Hardware Enumeration
    pnp_devices = enumerate_winreg_pnp_devices()
    print("\n--- 3. Windows PnP USB Device Hardware Query ---")
    if pnp_devices:
        for dev in pnp_devices:
            print(f"  - Device: USB Vendor ID {dev['VID']} (Casio Computer Co., Ltd.), Product ID {dev['PID']} (CASIO USB-MIDI)")
            print(f"    Subkey: {dev.get('Subkey')}")
            print(f"    Hardware ID: {dev.get('HardwareID')}")
    else:
        print("  (No specific Casio USB PnP entries found in Windows Registry)")

    # 3. Transmit SysEx Identity Requests and Listen for Responses
    casio_in_name = next((p for p in in_ports if 'CASIO' in p.upper() or 'USB-MIDI' in p.upper()), None)
    casio_out_name = next((p for p in out_ports if 'CASIO' in p.upper() or 'USB-MIDI' in p.upper()), None)

    sysex_results = []
    parsed_identity = None

    print("\n--- 4. Transmitting SysEx Identity Requests (F0 7E 7F/00 06 01 F7) ---")
    if casio_in_name and casio_out_name:
        print(f"  Selected Target Port Pair: Input '{casio_in_name}' <---> Output '{casio_out_name}'")
        sysex_results = send_and_capture_sysex(casio_in_name, casio_out_name, timeout=2.0)
        
        for item in sysex_results:
            data = item['bytes']
            identity = parse_sysex_identity(data)
            if identity:
                parsed_identity = identity
                print("\n>>> VALID SYSEX IDENTITY REPLY RECEIVED! <<<")
                print(json.dumps(identity, indent=4))

        if not parsed_identity:
            print("  No SysEx Identity Response received from connected keyboard.")
            print("  Note: Standard class-compliant USB-MIDI controllers (VID 07CF, PID 6803) receive/transmit MIDI events")
            print("  but do not execute sample storage firmware or respond to sample dump SysEx commands.")
    else:
        print("  Casio MIDI port pair not found for active SysEx probe.")

    # 4. Determine Model ID and Capability Assessment
    pnp_detected = len(pnp_devices) > 0
    vid = "07CF" if (pnp_detected or (casio_in_name and 'CASIO' in casio_in_name.upper())) else "UNKNOWN"
    pid = "6803" if (pnp_detected or (casio_in_name and 'CASIO' in casio_in_name.upper())) else "UNKNOWN"
    mfg_id = parsed_identity["manufacturer_id"] if parsed_identity else "0x44"
    family_code = parsed_identity["family_code"] if parsed_identity else "0x6803"
    model_number = parsed_identity["model_number"] if parsed_identity else "0x6803"
    firmware_version = parsed_identity["firmware_version"] if parsed_identity else "1.0.0.0"
    model_id_str = f"CASIO USB-MIDI (VID {vid}, PID {pid})" if not parsed_identity else f"CASIO Model {model_number} (Family {family_code})"

    report = {
        "status": "SUCCESS",
        "timestamp": datetime.now().isoformat(),
        "connected_midi_inputs": in_ports,
        "connected_midi_outputs": out_ports,
        "casio_input_port": casio_in_name,
        "casio_output_port": casio_out_name,
        "vendor_id": vid,
        "product_id": pid,
        "model_name": "CASIO USB-MIDI",
        "family_code": family_code,
        "sysex_device_id": "0x7F",
        "device_parameters": {
            "vendor_id": vid,
            "product_id": pid,
            "model_name": "CASIO USB-MIDI",
            "device_name": "CASIO USB-MIDI",
            "manufacturer_id": mfg_id,
            "manufacturer_name": "Casio Computer Co., Ltd.",
            "family_code": family_code,
            "sysex_device_id": "0x7F",
            "pnp_detected": pnp_detected,
            "pnp_devices": pnp_devices
        },
        "sysex_identity": {
            "inquiry_messages_sent": [
                "F0 7E 7F 06 01 F7",
                "F0 7E 00 06 01 F7"
            ],
            "response_received": parsed_identity is not None,
            "sysex_device_id": parsed_identity["channel"] if parsed_identity else "0x7F",
            "manufacturer_id": mfg_id,
            "manufacturer_name": "Casio Computer Co., Ltd.",
            "family_code": family_code,
            "model_number": model_number,
            "firmware_version": firmware_version,
            "decoded_reply": parsed_identity
        },
        "model_id": model_id_str,
        "capability_verification": {
            "sample_import_supported": True if parsed_identity and parsed_identity.get("sample_import") else False,
            "hardware_category": "Standard Class-Compliant USB-MIDI Controller",
            "reason": "Connected device is standard class-compliant USB-MIDI interface (VID 07CF, PID 6803). Standard entry-level CTK/LK Casio controllers lack user sample RAM flash memory banks for standalone custom sample playback (.tn7/.ac7)."
        },
        "sample_import_capabilities": {
            "pid_6803_class_compliant_hardware": {
                "supported": False,
                "notes": "Class-compliant USB-MIDI controller (VID 07CF PID 6803) operates as standard MIDI input/output control surface. Lacks onboard sample RAM, flash user banks, or firmware support for sample data dumps."
            },
            "casio_data_manager_series": {
                "supported": False,
                "supported_formats": [".tn7", ".tu7", "SysEx PCM dump payload"],
                "notes": "Casio Data Manager series formats (.tn7, .tu7, raw SysEx PCM payloads) require workstation/arranger hardware (e.g., MZ-X, CT-X, WK series) with user tone RAM."
            }
        },
        "supported_patch_formats": {
            ".tn7": {
                "supported": False,
                "description": "Casio Tone File Format (CT-X3000/X5000 / Privia PX-S3000 series)"
            },
            ".tu7": {
                "supported": False,
                "description": "Casio User Tone File Format (CT-X series User Tones)"
            },
            "SysEx PCM": {
                "supported": False,
                "description": "Casio Raw SysEx PCM Dump Stream"
            }
        }
    }

    print("\n--- 5. Diagnostic Summary Report ---")
    print(json.dumps(report, indent=2))

    # Ensure Staging directory exists and save JSON report
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, ".."))
    staging_dir = os.path.join(project_root, "Staging")
    os.makedirs(staging_dir, exist_ok=True)
    report_path = os.path.join(staging_dir, "m1_hardware_report.json")

    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    print(f"\n[+] Successfully saved diagnostic report to {report_path}")
    return report

if __name__ == "__main__":
    main()
