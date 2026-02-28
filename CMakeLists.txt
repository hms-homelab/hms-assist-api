cmake_minimum_required(VERSION 3.16)
project(hms_assist CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# ─── Version ──────────────────────────────────────────────────────────────────
# Single source of truth: VERSION file (MAJOR.MINOR.PATCH)
file(READ "${CMAKE_SOURCE_DIR}/VERSION" HMS_VERSION_RAW)
string(STRIP "${HMS_VERSION_RAW}" HMS_VERSION)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${HMS_VERSION}")
set(HMS_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(HMS_VERSION_MINOR "${CMAKE_MATCH_2}")
set(HMS_VERSION_PATCH "${CMAKE_MATCH_3}")

# Build number: total git commit count
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE HMS_BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HMS_BUILD_NUMBER)
    set(HMS_BUILD_NUMBER "0")
endif()

# Git short hash for traceability
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE HMS_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HMS_GIT_HASH)
    set(HMS_GIT_HASH "unknown")
endif()

message(STATUS "HMS-Assist version: ${HMS_VERSION} (build ${HMS_BUILD_NUMBER}, ${HMS_GIT_HASH})")

# Dependencies
find_package(Drogon CONFIG REQUIRED)
find_package(jsoncpp REQUIRED)
find_package(CURL REQUIRED)
find_package(yaml-cpp REQUIRED)

find_library(PQXX_LIB pqxx REQUIRED)
find_library(PQ_LIB   pq   REQUIRED)

# Collect all source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    HMS_VERSION_MAJOR=${HMS_VERSION_MAJOR}
    HMS_VERSION_MINOR=${HMS_VERSION_MINOR}
    HMS_VERSION_PATCH=${HMS_VERSION_PATCH}
    HMS_VERSION_STRING="${HMS_VERSION}"
    HMS_BUILD_NUMBER=${HMS_BUILD_NUMBER}
    HMS_GIT_HASH="${HMS_GIT_HASH}"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CURL_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME}
    Drogon::Drogon
    jsoncpp_lib
    ${PQXX_LIB}
    ${PQ_LIB}
    ${CURL_LIBRARIES}
    yaml-cpp
    pthread
    ssl
    crypto
)

install(TARGETS ${PROJECT_NAME} DESTINATION /usr/local/bin)

# ─── Unit Tests ───────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

# All sources except main.cpp (which defines main())
file(GLOB_RECURSE SOURCES_NO_MAIN "src/*.cpp")
list(REMOVE_ITEM SOURCES_NO_MAIN "${CMAKE_SOURCE_DIR}/src/main.cpp")

file(GLOB TEST_SOURCES "tests/unit/*.cpp")

add_executable(hms_assist_tests
    ${SOURCES_NO_MAIN}
    ${TEST_SOURCES}
)

target_compile_definitions(hms_assist_tests PRIVATE
    HMS_VERSION_MAJOR=${HMS_VERSION_MAJOR}
    HMS_VERSION_MINOR=${HMS_VERSION_MINOR}
    HMS_VERSION_PATCH=${HMS_VERSION_PATCH}
    HMS_VERSION_STRING="${HMS_VERSION}"
    HMS_BUILD_NUMBER=${HMS_BUILD_NUMBER}
    HMS_GIT_HASH="${HMS_GIT_HASH}"
)

target_include_directories(hms_assist_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CURL_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIR}
    ${GTEST_INCLUDE_DIRS}
)

target_link_libraries(hms_assist_tests
    GTest::gtest_main
    GTest::gmock
    Drogon::Drogon
    jsoncpp_lib
    ${PQXX_LIB}
    ${PQ_LIB}
    ${CURL_LIBRARIES}
    yaml-cpp
    pthread
    ssl
    crypto
)

include(GoogleTest)
gtest_discover_tests(hms_assist_tests)
