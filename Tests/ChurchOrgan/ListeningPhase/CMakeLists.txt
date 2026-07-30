cmake_minimum_required(VERSION 3.20)

project(ChurchOrganListeningPhase LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

add_library(ChurchOrganListeningEngines STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/HarmonicFoldback.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/HarmonicPercussion.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/KeyClickGenerator.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/MultiContactKeying.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganAmpCab.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganParameters.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/RotaryCabinet.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/ScannerVibrato.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelBank.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelCharacter.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelOrganEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TransistorDividerBank.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TransistorOrganEngine.cpp")

target_include_directories(ChurchOrganListeningEngines
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganListeningPack
    ListeningPackRenderer.cpp)

target_link_libraries(ChurchOrganListeningPack
    PRIVATE ChurchOrganListeningEngines)

foreach(target
        ChurchOrganListeningEngines
        ChurchOrganListeningPack)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
endforeach()

enable_testing()

add_test(
    NAME ChurchOrganListeningPackQA
    COMMAND ChurchOrganListeningPack
        "${CMAKE_CURRENT_BINARY_DIR}/listening-pack")

set_tests_properties(
    ChurchOrganListeningPackQA
    PROPERTIES
        TIMEOUT 600
        LABELS "listening;offline")
