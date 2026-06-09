cmake_minimum_required(VERSION 3.16)
project(aowis-epanet-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network HttpServer)

qt_add_executable(aowis-epanet-server
    src/main.cpp
    src/maptiles.h src/maptiles.cpp
    src/rest_client.h src/rest_client.cpp
    src/server.h src/server.cpp
)

target_link_libraries(aowis-epanet-server
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::HttpServer
)
