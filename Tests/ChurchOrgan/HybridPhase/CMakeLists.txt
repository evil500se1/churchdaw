cmake_minimum_required(VERSION 3.20)

project(ChurchOrganHybridPhaseIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

set(CHURCH_ORGAN_HYBRID_SOURCES
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
    "${CHURCH_ORGAN_SOURCE_DIR}/WorshipLayerEngine.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/AutoMixContext.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/WorshipHybridEngine.cpp")

add_library(ChurchOrganHybrid STATIC
    ${CHURCH_ORGAN_HYBRID_SOURCES})

target_include_directories(ChurchOrganHybrid
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganHybridTests
    ChurchOrganHybridTests.cpp)
target_link_libraries(ChurchOrganHybridTests
    PRIVATE ChurchOrganHybrid)

add_executable(ChurchOrganHybridBenchmark
    ChurchOrganHybridBenchmark.cpp)
target_link_libraries(ChurchOrganHybridBenchmark
    PRIVATE ChurchOrganHybrid)

# Mede a cauda da nota com os padroes de producao (relato de "nota que fica
# soando"). Fica fora do CTest por ser diagnostico, nao criterio de aceite.
add_executable(WorshipDecayProbe
    WorshipDecayProbe.cpp)
target_link_libraries(WorshipDecayProbe
    PRIVATE ChurchOrganHybrid)

foreach(target
        ChurchOrganHybrid
        ChurchOrganHybridTests
        ChurchOrganHybridBenchmark
        WorshipDecayProbe)
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
    target_link_options(ChurchOrganHybridTests
        PRIVATE /STACK:33554432)
    target_link_options(ChurchOrganHybridBenchmark
        PRIVATE /STACK:33554432)
endif()

enable_testing()

add_test(
    NAME ChurchOrganHybridCorrectness
    COMMAND ChurchOrganHybridTests)

add_test(
    NAME ChurchOrganHybridBenchmark
    COMMAND ChurchOrganHybridBenchmark)

set_tests_properties(
    ChurchOrganHybridCorrectness
    ChurchOrganHybridBenchmark
    PROPERTIES TIMEOUT 300)
