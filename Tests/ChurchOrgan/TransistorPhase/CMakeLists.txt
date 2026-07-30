cmake_minimum_required(VERSION 3.20)

project(ChurchTransistorOrganIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

add_library(ChurchTransistorOrgan STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/TransistorDividerBank.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/TransistorOrganEngine.cpp")

target_include_directories(ChurchTransistorOrgan
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchTransistorOrganTests
    TransistorOrganTests.cpp)
target_link_libraries(ChurchTransistorOrganTests
    PRIVATE ChurchTransistorOrgan)

add_executable(ChurchTransistorOrganBenchmark
    TransistorOrganBenchmark.cpp)
target_link_libraries(ChurchTransistorOrganBenchmark
    PRIVATE ChurchTransistorOrgan)

foreach(target
        ChurchTransistorOrgan
        ChurchTransistorOrganTests
        ChurchTransistorOrganBenchmark)
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
    NAME ChurchTransistorOrganCorrectness
    COMMAND ChurchTransistorOrganTests)

add_test(
    NAME ChurchTransistorOrganBenchmark
    COMMAND ChurchTransistorOrganBenchmark
    CONFIGURATIONS Release)

set_tests_properties(
    ChurchTransistorOrganCorrectness
    ChurchTransistorOrganBenchmark
    PROPERTIES TIMEOUT 180)
