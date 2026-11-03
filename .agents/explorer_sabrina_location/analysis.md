# Technical Analysis Report: Sabrina Plugin Discovery & Host Directory Architecture

**Author**: `explorer_sabrina_location`  
**Date**: 2026-08-05  
**Target Project**: ChurchDAW (`D:\ChurchDAW_Source`)  
**Scope**: Discovery, binary analysis, metadata inspection, and host architecture mapping for the "Sabrina" plugin.

---

## 1. Executive Summary

A comprehensive search of `C:\Users\Fabiopc\Downloads` and its subdirectories was performed to locate compiled plugins and source code repositories for "Sabrina".

- **Found Artifacts**: 1 compiled binary DLL (`Sabrina.dll`) and 1 ZIP archive (`Sabrina(64).zip`).
- **Binary Architecture**: Windows **x86_64** (PE32+ 64-bit AMD64).
- **Plugin Format**: **VST 2.4 (VST2)** legacy C++ plugin (exports `VSTPluginMain` and `main`).
- **Framework Metadata**: Built with native Steinberg VST 2.4 C++ SDK (inheriting from `AudioEffectX` / class `7Sabrina`). Contains **no embedded JUCE metadata**.
- **Cross-Platform / Android Impact**: `Sabrina.dll` is compiled strictly for Windows x86_64. It cannot execute natively on Android (ARM64/ARMv7) or macOS. Implementation must use JUCE conditional compilation (`JUCE_WINDOWS`) with a transparent bypass / fallback processor for Android and macOS.

---

## 2. Location & Search Findings

### 2.1 Search Results in `C:\Users\Fabiopc\Downloads`

| Field | File 1: Compiled DLL | File 2: ZIP Archive |
|---|---|---|
| **Exact Absolute Path** | `C:\Users\Fabiopc\Downloads\Sabrina(64)\Sabrina.dll` | `C:\Users\Fabiopc\Downloads\Sabrina(64).zip` |
| **File Size** | 1,683,456 bytes (~1.61 MB) | 526,806 bytes (~514 KB) |
| **Last Modification Date** | 2021-07-01 11:16:46 | 2026-08-05 10:24:55 |
| **Archive Content** | N/A | Contains single file `Sabrina.dll` |
| **Source Code / Headers** | Not found in Downloads | Not found in Downloads |
| **VST3 / AU / Component / SO** | Not found in Downloads | Not found in Downloads |

### 2.2 Verification of Downloads Search Scope
- Scanned all subdirectories of `C:\Users\Fabiopc\Downloads` recursively.
- Checked for extensions: `.dll`, `.vst3`, `.component`, `.au`, `.so`, `.cpp`, `.h`, `.hpp`, `.zip`, `.tar.gz`.
- Result: **No source code, C++ repositories, headers, or Linux/macOS binary variants were found.** Only the 64-bit Windows VST2 DLL `Sabrina.dll` exists.

---

## 3. Deep Binary & PE Header Analysis (`Sabrina.dll`)

### 3.1 PE Header Details

```
=== PE HEADER & ARCHITECTURE ===
PE Header Offset:      0x80
Machine Code:          0x8664 (IMAGE_FILE_MACHINE_AMD64)
Architecture:          x86_64 (64-bit AMD64 / Intel 64)
Optional Header Magic: 0x020B (PE32+)
TimeDateStamp:         2021-07-01 13:16:45 UTC
Subsystem:             Windows GUI / C Runtime DLL
```

### 3.2 Export Table Inspection

Inspection of the DLL Export Directory revealed **2 exported entry points**:
1. `Export [0]: VSTPluginMain` (Standard entry point for VST 2.4 plugins)
2. `Export [1]: main` (Legacy backwards-compatibility entry point for VST 2.x plugins)

> **Note**: `GetPluginFactory` (VST3 entry point) is **absent**. This confirms `Sabrina.dll` is a **VST2** plugin format.

### 3.3 Framework & RTTI Symbol Analysis

String analysis of internal RTTI structures and C++ symbols revealed:
- `11AudioEffect` (GCC/MSVC RTTI mangled name for Steinberg VST SDK `AudioEffect` base class)
- `12AudioEffectX` (GCC/MSVC RTTI mangled name for Steinberg VST SDK `AudioEffectX` base class)
- `7Sabrina` (Mangled C++ class name for `class Sabrina : public AudioEffectX`)
- VST Event methods: `receiveVstMidiEvent`, `receiveVstTimeInfo`, `receiveVstEvents`, `sendVstMidiEvent`, `sendVstTimeInfo`
- JUCE markers count: `0` (No JUCE framework symbols, strings, or class structures present).

---

## 4. Multiplatform & Mobile (Android / macOS) Impact

1. **Windows Desktop (x86_64)**:
   - JUCE's `VSTPluginFormat` (or `AudioPluginFormatManager` with VST2 support enabled) can dynamically load `Sabrina.dll` on 64-bit Windows.
2. **Android (ARM64 / ARMv7)**:
   - Android runs Linux kernel on ARM hardware. Binary DLL files compiled for Windows x86_64 **cannot be loaded or executed natively** on Android.
   - Calling Windows DLL load functions (`LoadLibraryA`) on Android will fail.
   - **Requirement**: ChurchDAW's audio graph on Android must instantiate a **Bypass / Pass-through AudioProcessor** when running on non-Windows platforms or when `Sabrina.dll` is unavailable.
3. **macOS (x86_64 / Apple Silicon ARM64)**:
   - Windows `.dll` binaries cannot be loaded by macOS `dlopen()`.
   - Requires conditional compilation or fallback bypass processor on macOS unless a macOS `.vst3` or `.component` build is provided.

---

## 5. Recommended Target Directory Structure in `D:\ChurchDAW_Source`

To integrate `Sabrina` into ChurchDAW cleanly and maintain modular project hygiene, the following directory layout inside `D:\ChurchDAW_Source` is recommended:

```
D:\ChurchDAW_Source\
├── Plugins\
│   └── Sabrina\
│       ├── bin\
│       │   └── win_x64\
│       │       └── Sabrina.dll          <-- Copy C:\Users\Fabiopc\Downloads\Sabrina(64)\Sabrina.dll here
│       └── README.md                    <-- Documentation on plugin architecture & platform restrictions
└── Source\
    └── Plugins\
        └── Sabrina\
            ├── SabrinaHostProcessor.h   <-- JUCE AudioProcessor wrapper hosting Sabrina.dll on Windows
            ├── SabrinaHostProcessor.cpp <-- Fallback / Bypass logic for Android & macOS
            └── SabrinaToggleUI.h        <-- UI toggle button component for Desktop & Mobile
```

### 5.1 Rationale for Recommended Structure
1. **Separation of Binaries and Source Code**:
   - `Plugins/Sabrina/bin/win_x64/` holds platform-specific precompiled binaries.
   - `Source/Plugins/Sabrina/` holds C++ source files, headers, and UI components co-located under `Source/`.
2. **Alignment with JUCE / CMake Build System**:
   - CMake target `ChurchDAW` can easily add `Source/Plugins/Sabrina/*.cpp` to its source list.
   - CMake install/copy steps can automatically copy `Plugins/Sabrina/bin/win_x64/Sabrina.dll` to the build output folder (`$<TARGET_FILE_DIR:ChurchDAW>`) on Windows builds.
3. **Clean Conditional Compilation**:
   `SabrinaHostProcessor` can encapsulate platform checks:
   ```cpp
   #if JUCE_WINDOWS && !JUCE_ANDROID
       // Load VST2 plugin via JUCE VSTPluginFormat from Plugins/Sabrina/bin/win_x64/Sabrina.dll
   #else
       // Pass-through audio buffer directly (0% CPU, safe bypass for Android & macOS)
   #endif
   ```

---

## 6. Action Plan for Implementation Agents

1. **Directory Setup & File Copy**:
   - Create `D:\ChurchDAW_Source\Plugins\Sabrina\bin\win_x64\`
   - Copy `Sabrina.dll` from `C:\Users\Fabiopc\Downloads\Sabrina(64)\` to `D:\ChurchDAW_Source\Plugins\Sabrina\bin\win_x64\Sabrina.dll`.
2. **C++ Native Host Implementation (`R2`)**:
   - Enable VST2 support in JUCE CMake (`juce_enable_vst2_sdk` or JUCE VST2 hosting capability).
   - Implement `SabrinaHostProcessor` derived from `juce::AudioProcessor`.
3. **UI Bypass Toggle (`R3`)**:
   - Add a bypass toggle button in `MainComponent` / `MasterProcessor` UI.
   - When bypass is active, suspend `processBlock` execution for Sabrina to eliminate CPU usage.
