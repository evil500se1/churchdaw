cmake_minimum_required(VERSION 3.20)

project(ChurchOrganStateIsolated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHURCH_ORGAN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../Source/ChurchOrgan")

add_library(ChurchOrganState STATIC
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganParameters.cpp"
    "${CHURCH_ORGAN_SOURCE_DIR}/OrganPresetState.cpp")

target_include_directories(ChurchOrganState
    PUBLIC "${CHURCH_ORGAN_SOURCE_DIR}")

add_executable(ChurchOrganStateTests
    OrganStateTests.cpp)

target_link_libraries(ChurchOrganStateTests
    PRIVATE ChurchOrganState)

foreach(target ChurchOrganState ChurchOrganStateTests)
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
add_test(NAME ChurchOrganStateContract
         COMMAND ChurchOrganStateTests)
