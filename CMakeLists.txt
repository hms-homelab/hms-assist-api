cmake_minimum_required(VERSION 3.16)
project(hms_assist CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

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
