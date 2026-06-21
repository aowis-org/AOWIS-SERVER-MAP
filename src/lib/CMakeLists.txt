
add_library(aowis-server-map-core STATIC
    maptiles.h
    maptiles.cpp
    rest_client.h
    rest_client.cpp
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

