cmake_minimum_required(VERSION 3.22)

project(
    ChurchOrganIntegrationPhaseIsolated
    VERSION 1.0.0
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../../JUCE"
    "${CMAKE_CURRENT_BINARY_DIR}/JUCE")

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

set(CHURCH_ORGAN_INTEGRATION_SOURCES
    "${CHURCH_ORGAN_SOURCE_DIR}/HarmonicFoldback.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelBank.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelCharacter.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/MultiContactKeying.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/KeyClickGenerator.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/HarmonicPercussion.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/ScannerVibrato.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganAmpCab.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/RotaryCabinet.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganParameters.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelOrganEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TransistorDividerBank.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TransistorOrganEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/PipeOrganEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/WorshipLayerEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/AutoMixContext.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/WorshipHybridEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/ChurchOrganLayerProcessor.cpp")

add_library(ChurchOrganIntegration STATIC
    ${CHURCH_ORGAN_INTEGRATION_SOURCES})

target_include_directories(ChurchOrganIntegration
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

target_compile_definitions(ChurchOrganIntegration
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(ChurchOrganIntegration
    PUBLIC
        juce::juce_audio_basics
        juce::juce_data_structures
        juce::juce_recommended_config_flags)

juce_add_console_app(ChurchOrganIntegrationTests
    PRODUCT_NAME "ChurchOrganIntegrationTests")
target_sources(ChurchOrganIntegrationTests PRIVATE
    ChurchOrganIntegrationTests.cpp)
target_link_libraries(ChurchOrganIntegrationTests
    PRIVATE ChurchOrganIntegration)

juce_add_console_app(ChurchOrganIntegrationBenchmark
    PRODUCT_NAME "ChurchOrganIntegrationBenchmark")
target_sources(ChurchOrganIntegrationBenchmark PRIVATE
    ChurchOrganIntegrationBenchmark.cpp)
target_link_libraries(ChurchOrganIntegrationBenchmark
    PRIVATE ChurchOrganIntegration)

foreach(target
        ChurchOrganIntegration
        ChurchOrganIntegrationTests
        ChurchOrganIntegrationBenchmark)
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

if(MSVC)
    target_link_options(ChurchOrganIntegrationTests
        PRIVATE /STACK:33554432)
    target_link_options(ChurchOrganIntegrationBenchmark
        PRIVATE /STACK:33554432)
endif()

enable_testing()

add_test(
    NAME ChurchOrganIntegrationCorrectness
    COMMAND ChurchOrganIntegrationTests)

add_test(
    NAME ChurchOrganIntegrationBenchmark
    COMMAND ChurchOrganIntegrationBenchmark)

set_tests_properties(
    ChurchOrganIntegrationCorrectness
    ChurchOrganIntegrationBenchmark
    PROPERTIES TIMEOUT 420)
