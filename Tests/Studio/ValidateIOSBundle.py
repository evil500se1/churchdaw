#!/usr/bin/env python3
"""Validate the built iPhone/iPad package before archiving or distribution."""
import pathlib
import plistlib
import re
import sys


def validate(app: pathlib.Path) -> None:
    with (app / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    cmake = (pathlib.Path(__file__).resolve().parents[2] / "CMakeLists.txt").read_text()
    version = re.search(r"project\(MinhaDAW\s+VERSION\s+([\d.]+)", cmake).group(1)
    build = re.search(r'BUILD_VERSION\s+"([\d.]+)"', cmake).group(1)
    expected = {
        "CFBundleIdentifier": "com.churchdaw.app",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": build,
        "UIDeviceFamily": [1, 2],
        "UIRequiresFullScreen": True,
        "UIFileSharingEnabled": True,
    }
    for key, value in expected.items():
        if info.get(key) != value:
            raise ValueError(f"{key}: expected {value!r}, found {info.get(key)!r}")
    for key in ("NSCameraUsageDescription", "NSMicrophoneUsageDescription",
                "NSPhotoLibraryAddUsageDescription"):
        if not info.get(key, "").strip():
            raise ValueError(f"Missing permission purpose: {key}")
    orientations = {"UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"}
    for key in ("UISupportedInterfaceOrientations", "UISupportedInterfaceOrientations~ipad"):
        if set(info.get(key, [])) != orientations:
            raise ValueError(f"Unexpected orientations: {key}")
    if "audio" not in info.get("UIBackgroundModes", []):
        raise ValueError("Background instrument audio is missing")
    executable = app / info.get("CFBundleExecutable", "ChurchDAW")
    if not executable.is_file():
        raise ValueError(f"Missing executable: {executable}")
    print(f"iPhone/iPad bundle PASS: {version} ({build}), existing app identity, ChurchCam permissions")


if __name__ == "__main__":
    try:
        validate(pathlib.Path(sys.argv[1]))
    except (IndexError, OSError, ValueError) as error:
        sys.exit(f"iOS bundle validation failed: {error}")
