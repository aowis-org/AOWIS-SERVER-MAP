cmake_minimum_required(VERSION 3.21)

project(
    AOWIS_SERVER_MAP
    VERSION 0.1.0
    DESCRIPTION "Caching map tile library and REST server"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)

option(
    AOWIS_SERVER_MAP_BUILD_SERVER
    "Build AOWIS map REST server executable"
    ON
)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_subdirectory(src/lib)

if(AOWIS_SERVER_MAP_BUILD_SERVER)
    find_package(Qt6 REQUIRED COMPONENTS HttpServer)
    add_subdirectory(src/server)
endif()
