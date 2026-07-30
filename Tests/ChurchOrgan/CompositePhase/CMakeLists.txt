cmake_minimum_required(VERSION 3.20)

project(ChurchOrganCompositeIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

set(CHURCH_ORGAN_COMPOSITE_SOURCES
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
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelOrganEngine.cpp")

add_library(ChurchOrganComposite STATIC
    ${CHURCH_ORGAN_COMPOSITE_SOURCES})

target_include_directories(ChurchOrganComposite
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganCompositeTests
    TonewheelOrganEngineTests.cpp)
target_link_libraries(ChurchOrganCompositeTests
    PRIVATE ChurchOrganComposite)

add_executable(ChurchOrganCompositeBenchmark
    TonewheelOrganEngineBenchmark.cpp)
target_link_libraries(ChurchOrganCompositeBenchmark
    PRIVATE ChurchOrganComposite)

foreach(target
        ChurchOrganComposite
        ChurchOrganCompositeTests
        ChurchOrganCompositeBenchmark)
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
    target_link_options(ChurchOrganCompositeTests PRIVATE /STACK:16777216)
    target_link_options(ChurchOrganCompositeBenchmark PRIVATE /STACK:8388608)
endif()

enable_testing()
add_test(NAME ChurchOrganCompositeCorrectness
         COMMAND ChurchOrganCompositeTests)
add_test(NAME ChurchOrganCompositeBenchmark
         COMMAND ChurchOrganCompositeBenchmark)

set_tests_properties(
    ChurchOrganCompositeCorrectness
    ChurchOrganCompositeBenchmark
    PROPERTIES TIMEOUT 240)
