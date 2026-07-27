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

add_library(aowis-server-map-core STATIC
    src/lib/maptiles.cpp
    src/lib/http_client_tilefetch.cpp
    src/lib/http_client_tilefetch.h
    include/aowis/map/maptiles.h
)

target_include_directories(aowis-server-map-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lib"
)

target_link_libraries(aowis-server-map-core
    PUBLIC
        Qt6::Core
        Qt6::Network
)

target_compile_features(aowis-server-map-core
    PUBLIC
        cxx_std_20
)

add_library(aowis-server-map INTERFACE)
add_library(AOWIS::ServerMap ALIAS aowis-server-map)
add_library(AOWIS::Map ALIAS aowis-server-map)

target_link_libraries(aowis-server-map
    INTERFACE
        aowis-server-map-core
)

if(AOWIS_SERVER_MAP_BUILD_SERVER)
    find_package(Qt6 REQUIRED COMPONENTS HttpServer)

    qt_add_executable(aowis-server-map-server
        src/server/main.cpp
        src/server/server.cpp
        src/server/server.h
    )

    set_target_properties(aowis-server-map-server
        PROPERTIES
            OUTPUT_NAME aowis-server-map
    )

    target_link_libraries(aowis-server-map-server
        PRIVATE
            AOWIS::Map
            Qt6::Core
            Qt6::Network
            Qt6::HttpServer
    )

    target_compile_features(aowis-server-map-server
        PRIVATE
            cxx_std_20
    )
endif()
