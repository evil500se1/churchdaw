import os
import re
import sys

HEADER_PATH = r"D:\ChurchDAW_Source\Source\Plugins\Sabrina\SabrinaProcessor.h"
CPP_PATH = r"D:\ChurchDAW_Source\Source\Plugins\Sabrina\SabrinaProcessor.cpp"
SABRINA_FX_PATH = r"D:\ChurchDAW_Source\Source\SabrinaFx.h"
MAIN_CPP_PATH = r"D:\ChurchDAW_Source\Source\MainComponent.cpp"
CMAKE_PATH = r"D:\ChurchDAW_Source\CMakeLists.txt"

def verify():
    print("--- Verifying SabrinaProcessor M2 Work Product ---")
    failures = []

    # 1. Verify Header File
    if not os.path.exists(HEADER_PATH):
        failures.append(f"Header file missing: {HEADER_PATH}")
    else:
        with open(HEADER_PATH, "r", encoding="utf-8") as f:
            h_content = f.read()

        if "class SabrinaProcessor : public juce::AudioProcessor" not in h_content:
            failures.append("SabrinaProcessor does not inherit from juce::AudioProcessor")

        if "void setSabrinaBypassed" not in h_content or "bool isSabrinaBypassed" not in h_content:
            failures.append("Missing setSabrinaBypassed / isSabrinaBypassed atomic methods")

        if "std::atomic<bool> bypassed" not in h_content:
            failures.append("Missing std::atomic<bool> bypassed variable")

        if "#if JUCE_WINDOWS && !JUCE_ANDROID" not in h_content:
            failures.append("Missing platform preprocessor guards JUCE_WINDOWS && !JUCE_ANDROID")

        print("[OK] Header checks passed")

    # 2. Verify CPP File
    if not os.path.exists(CPP_PATH):
        failures.append(f"CPP file missing: {CPP_PATH}")
    else:
        with open(CPP_PATH, "r", encoding="utf-8") as f:
            cpp_content = f.read()

        if "AudioProcessor (BusesProperties()" not in cpp_content:
            failures.append("Constructor does not initialize AudioProcessor with stereo buses properties")

        # Zero-copy check
        bypass_match = re.search(r"if\s*\(\s*bypassed\.load\s*\(\s*std::memory_order_relaxed\s*\)\s*\)\s*return\s*;", cpp_content)
        if not bypass_match:
            failures.append("processBlock missing early return zero-copy bypass check")

        # Windows VST loading check
        if "sabrinaEngine.processIfEnabled" not in cpp_content:
            failures.append("processBlock missing sabrinaEngine VST process call")

        # Fallback saturation check
        if "std::tanh" not in cpp_content:
            failures.append("processBlock missing std::tanh native C++ fallback saturation algorithm")

        print("[OK] CPP implementation checks passed")

    # 3. Verify SabrinaFx.h
    if not os.path.exists(SABRINA_FX_PATH):
        failures.append(f"SabrinaFx header missing: {SABRINA_FX_PATH}")
    else:
        print("[OK] SabrinaFx header exists")

    # 4. Verify CMake integration
    if not os.path.exists(CMAKE_PATH):
        failures.append(f"CMakeLists.txt missing: {CMAKE_PATH}")
    else:
        with open(CMAKE_PATH, "r", encoding="utf-8") as f:
            cmake_content = f.read()
        if "Source/Plugins/Sabrina/SabrinaProcessor.cpp" not in cmake_content:
            failures.append("SabrinaProcessor.cpp not listed in CMakeLists.txt")
        else:
            print("[OK] CMakeLists.txt integration verified")

    if failures:
        print("\n[FAIL] VERIFICATION FAILED:")
        for fail in failures:
            print(f"  - {fail}")
        sys.exit(1)
    else:
        print("\n[PASS] ALL VERIFICATION CHECKS PASSED SUCCESSFULLY!")

if __name__ == "__main__":
    verify()
