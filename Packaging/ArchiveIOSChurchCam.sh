#!/bin/bash
# Archive the existing iOS app with an explicitly selected distribution profile.
# Does not export or upload, and never changes Keychain permissions.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
: "${CHURCHDAW_IOS_PROFILE:?Set the existing app distribution profile name}"
: "${CHURCHDAW_IOS_ARCHIVE:?Set the destination xcarchive path}"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
xcodebuild -checkFirstLaunchStatus
cmake -S "$ROOT" -B "$ROOT/build-ios-churchcam-device" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_XCODE_GENERATE_SCHEME=ON
xcodebuild archive -project "$ROOT/build-ios-churchcam-device/MinhaDAW.xcodeproj" \
    -scheme MinhaDAW -configuration Release -sdk iphoneos \
    -destination 'generic/platform=iOS' -archivePath "$CHURCHDAW_IOS_ARCHIVE" \
    -jobs 3 CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM=RH3476QHUV \
    'CODE_SIGN_IDENTITY=Apple Distribution' \
    "PROVISIONING_PROFILE_SPECIFIER=$CHURCHDAW_IOS_PROFILE"
APP="$CHURCHDAW_IOS_ARCHIVE/Products/Applications/ChurchDAW.app"
codesign --verify --deep --strict "$APP"
python3 "$ROOT/Tests/Studio/ValidateIOSBundle.py" "$APP"
