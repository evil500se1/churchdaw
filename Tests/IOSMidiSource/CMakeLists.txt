cmake_minimum_required(VERSION 3.22)
project(ChurchDAWIOSMidiSource LANGUAGES C CXX OBJC OBJCXX)

add_executable(ChurchDAWIOSMidiSource MACOSX_BUNDLE main.mm)
target_link_libraries(ChurchDAWIOSMidiSource PRIVATE
    "-framework UIKit"
    "-framework Foundation"
    "-framework CoreMIDI"
    "-framework AVFoundation")

set_target_properties(ChurchDAWIOSMidiSource PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.churchdaw.miditest"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "RH3476QHUV"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0")
