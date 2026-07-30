cmake_minimum_required(VERSION 3.20)

project(ChurchPipeOrganIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

add_library(ChurchPipeOrgan STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/PipeOrganEngine.cpp")

target_include_directories(ChurchPipeOrgan
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchPipeOrganTests
    PipeOrganTests.cpp)
target_link_libraries(ChurchPipeOrganTests
    PRIVATE ChurchPipeOrgan)

add_executable(ChurchPipeOrganBenchmark
    PipeOrganBenchmark.cpp)
target_link_libraries(ChurchPipeOrganBenchmark
    PRIVATE ChurchPipeOrgan)

foreach(target
        ChurchPipeOrgan
        ChurchPipeOrganTests
        ChurchPipeOrganBenchmark)
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
    # The tests intentionally instantiate up to three fixed-storage engines
    # together. Production owns one engine in its heap-backed processor.
    target_link_options(
        ChurchPipeOrganTests PRIVATE /STACK:16777216)
    target_link_options(
        ChurchPipeOrganBenchmark PRIVATE /STACK:16777216)
endif()

enable_testing()

add_test(
    NAME ChurchPipeOrganCorrectness
    COMMAND ChurchPipeOrganTests)

add_test(
    NAME ChurchPipeOrganBenchmark
    COMMAND ChurchPipeOrganBenchmark
    CONFIGURATIONS Release)

set_tests_properties(
    ChurchPipeOrganCorrectness
    ChurchPipeOrganBenchmark
    PROPERTIES TIMEOUT 240)
