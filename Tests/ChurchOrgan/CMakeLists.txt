cmake_minimum_required(VERSION 3.20)

project(ChurchOrganCoreIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../Source/ChurchOrgan")

add_library(ChurchOrganCore STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/ChurchOrganCore.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/HarmonicFoldback.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TonewheelBank.cpp")

target_include_directories(ChurchOrganCore
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganCoreTests
    ChurchOrganCoreTests.cpp)
target_link_libraries(ChurchOrganCoreTests
    PRIVATE ChurchOrganCore)

add_executable(ChurchOrganCoreBenchmark
    ChurchOrganBenchmark.cpp)
target_link_libraries(ChurchOrganCoreBenchmark
    PRIVATE ChurchOrganCore)

foreach(target
        ChurchOrganCore
        ChurchOrganCoreTests
        ChurchOrganCoreBenchmark)
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
    NAME ChurchOrganCoreCorrectness
    COMMAND ChurchOrganCoreTests)

add_test(
    NAME ChurchOrganCoreBenchmark
    COMMAND ChurchOrganCoreBenchmark)

set_tests_properties(
    ChurchOrganCoreCorrectness
    ChurchOrganCoreBenchmark
    PROPERTIES TIMEOUT 180)
