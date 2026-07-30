cmake_minimum_required(VERSION 3.22)

project(ChurchOrganUIPhaseIsolated VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")
set(CHURCH_DAW_JUCE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../JUCE")

# This harness is intentionally independent from the product CMake/Jucer files.
# It can be configured directly from Tests/ChurchOrgan/UIPhase.
add_subdirectory(
    "${CHURCH_DAW_JUCE_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/JUCE"
    EXCLUDE_FROM_ALL)

juce_add_console_app(ChurchOrganUIPhaseTests
    PRODUCT_NAME "ChurchOrganUIPhaseTests")

target_sources(ChurchOrganUIPhaseTests PRIVATE
    ChurchOrganUIPhaseTests.cpp
    "${CHURCH_ORGAN_ROOT}/ChurchOrganLayout.cpp"
    "${CHURCH_ORGAN_ROOT}/ChurchOrganEditor.cpp"
    "${CHURCH_ORGAN_ROOT}/UI/ChurchOrganControls.cpp")

target_include_directories(ChurchOrganUIPhaseTests PRIVATE
    "${CHURCH_ORGAN_ROOT}"
    "${CHURCH_ORGAN_ROOT}/UI")

target_compile_definitions(ChurchOrganUIPhaseTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(ChurchOrganUIPhaseTests
    PRIVATE
        juce::juce_gui_basics
        juce::juce_graphics
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(ChurchOrganUIPhaseTests PRIVATE
        /utf-8 /permissive- /EHsc)
    target_compile_definitions(ChurchOrganUIPhaseTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

enable_testing()
add_test(NAME ChurchOrganUIGeometry COMMAND ChurchOrganUIPhaseTests)
set_tests_properties(ChurchOrganUIGeometry PROPERTIES TIMEOUT 90)
