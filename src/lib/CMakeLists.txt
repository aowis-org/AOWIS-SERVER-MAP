set(AOWIS_SERVER_MAP_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include")

add_library(aowis-server-map INTERFACE)
add_library(AOWIS::ServerMap ALIAS aowis-server-map)
add_library(AOWIS::Map ALIAS aowis-server-map)

add_library(aowis-server-map-core STATIC
    maptiles.cpp
    http_client_tilefetch.h
    http_client_tilefetch.cpp

    "${AOWIS_SERVER_MAP_INCLUDE_DIR}/aowis/map/maptiles.h"
)

target_include_directories(aowis-server-map-core
    PUBLIC
        "${AOWIS_SERVER_MAP_INCLUDE_DIR}"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
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

target_link_libraries(aowis-server-map
    INTERFACE
        aowis-server-map-core
)
