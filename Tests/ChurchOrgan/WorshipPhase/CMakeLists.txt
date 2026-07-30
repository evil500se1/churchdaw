cmake_minimum_required(VERSION 3.20)

project(ChurchOrganWorshipPhaseIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

add_library(ChurchOrganWorship STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/WorshipLayerEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/AutoMixContext.cpp")

target_include_directories(ChurchOrganWorship
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganWorshipTests
    ChurchOrganWorshipTests.cpp)
target_link_libraries(ChurchOrganWorshipTests
    PRIVATE ChurchOrganWorship)

add_executable(ChurchOrganWorshipBenchmark
    ChurchOrganWorshipBenchmark.cpp)
target_link_libraries(ChurchOrganWorshipBenchmark
    PRIVATE ChurchOrganWorship)

foreach(target
        ChurchOrganWorship
        ChurchOrganWorshipTests
        ChurchOrganWorshipBenchmark)
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
    NAME ChurchOrganWorshipCorrectness
    COMMAND ChurchOrganWorshipTests)

add_test(
    NAME ChurchOrganWorshipBenchmark
    COMMAND ChurchOrganWorshipBenchmark)

set_tests_properties(
    ChurchOrganWorshipCorrectness
    ChurchOrganWorshipBenchmark
    PROPERTIES TIMEOUT 180)
