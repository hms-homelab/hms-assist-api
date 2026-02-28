cmake_minimum_required(VERSION 3.16)
project(hms_assist CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# Find required libraries
find_package(Drogon CONFIG REQUIRED)
find_package(jsoncpp REQUIRED)
find_library(PAHO_MQTTPP3_LIB paho-mqttpp3)
find_library(PAHO_MQTT3AS_LIB paho-mqtt3as)
find_library(PQXX_LIB pqxx)
find_library(PQ_LIB pq)
find_package(CURL REQUIRED)

# Collect all source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CURL_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    Drogon::Drogon
    jsoncpp_lib
    ${PAHO_MQTTPP3_LIB}
    ${PAHO_MQTT3AS_LIB}
    ${PQXX_LIB}
    ${PQ_LIB}
    ${CURL_LIBRARIES}
    pthread
    ssl
    crypto
)

# Install target
install(TARGETS ${PROJECT_NAME} DESTINATION /usr/local/bin)
