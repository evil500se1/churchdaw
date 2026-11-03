#!/bin/bash
# Build the existing app for the simulator. No upload or account mutation.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"

if ! xcodebuild -checkFirstLaunchStatus; then
    echo "Abra o Xcode e conclua a licenca e os componentes iniciais antes de compilar." >&2
    exit 1
fi
xcrun --sdk iphonesimulator --show-sdk-version
cmake -S "$ROOT" -B "$ROOT/build-ios-churchcam" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    -DCMAKE_XCODE_GENERATE_SCHEME=ON
cmake --build "$ROOT/build-ios-churchcam" --config Release --target MinhaDAW --parallel 3 -- \
    -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=""
APP="$(find "$ROOT/build-ios-churchcam" -type d -name ChurchDAW.app -print -quit)"
python3 "$ROOT/Tests/Studio/ValidateIOSBundle.py" "$APP"
echo "Simulator app: $APP"
