cmake_minimum_required(VERSION 3.16)
project(AOWIS-SERVER-MAP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(AOWIS_SERVER_MAP_BUILD_SERVER "Build AOWIS map REST server executable" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network HttpServer)

add_subdirectory(src/lib)

if(AOWIS_SERVER_MAP_BUILD_SERVER)
    add_subdirectory(src/server)
endif()
