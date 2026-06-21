
add_library(aowis-server-map-core STATIC
    maptiles.h
    maptiles.cpp
    http_client_tilefetch.h
    http_client_tilefetch.cpp
)

target_include_directories(aowis-server-map-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aowis-server-map-core
    PUBLIC
        Qt6::Core
        Qt6::Network
)

