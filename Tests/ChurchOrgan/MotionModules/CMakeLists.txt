cmake_minimum_required(VERSION 3.20)

project(ChurchOrganMotionIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

add_library(ChurchOrganMotion STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/ScannerVibrato.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganAmpCab.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/RotaryCabinet.cpp")

target_include_directories(ChurchOrganMotion
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganMotionTests
    ChurchOrganMotionTests.cpp)
target_link_libraries(ChurchOrganMotionTests
    PRIVATE ChurchOrganMotion)

add_executable(ChurchOrganMotionBenchmark
    ChurchOrganMotionBenchmark.cpp)
target_link_libraries(ChurchOrganMotionBenchmark
    PRIVATE ChurchOrganMotion)

foreach(target
        ChurchOrganMotion
        ChurchOrganMotionTests
        ChurchOrganMotionBenchmark)
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
    NAME ChurchOrganMotionCorrectness
    COMMAND ChurchOrganMotionTests)

add_test(
    NAME ChurchOrganMotionBenchmark
    COMMAND ChurchOrganMotionBenchmark)

set_tests_properties(
    ChurchOrganMotionCorrectness
    ChurchOrganMotionBenchmark
    PROPERTIES TIMEOUT 180)
