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
find_package(TIFF REQUIRED)

add_library(aowis-server-map-core STATIC
    src/lib/maptiles.cpp
    src/lib/download_coverage.cpp
    src/lib/map_provider.cpp
    include/aowis/map/map_provider.h
    include/aowis/map/download_coverage.h
    src/lib/http_client_tilefetch.cpp
    src/lib/http_client_tilefetch.h
    src/lib/terrain_elevation.cpp
    src/lib/terrain_tile.cpp
    src/lib/terrain_provider.cpp
    src/lib/terrain_dataset.cpp
    src/lib/copernicus_terrain_provider.cpp
    src/lib/copernicus_terrain_provider.h
    src/lib/terrain_data.cpp
    include/aowis/map/maptiles.h
    include/aowis/map/terrain_elevation.h
    include/aowis/map/terrain_tile.h
    include/aowis/map/terrain_provider.h
    include/aowis/map/terrain_dataset.h
    include/aowis/map/terrain_data.h
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
    PRIVATE
        TIFF::TIFF
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

include(CTest)
if(BUILD_TESTING)
    add_executable(aowis-server-map-provider-limits-test
        tests/test_map_provider_limits.cpp
    )
    target_link_libraries(aowis-server-map-provider-limits-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-provider-limits
             COMMAND aowis-server-map-provider-limits-test)
    add_executable(aowis-server-map-cache-reuse-test
        tests/test_map_cache_reuse.cpp
    )
    target_link_libraries(aowis-server-map-cache-reuse-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-cache-reuse
             COMMAND aowis-server-map-cache-reuse-test)
    add_executable(aowis-server-map-download-coverage-test
        tests/test_download_coverage.cpp
    )
    target_link_libraries(aowis-server-map-download-coverage-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-download-coverage
             COMMAND aowis-server-map-download-coverage-test)
    add_executable(aowis-server-map-terrain-tile-format-test
        tests/test_terrain_tile_format.cpp
    )
    target_link_libraries(aowis-server-map-terrain-tile-format-test
        PRIVATE
            AOWIS::Map
    )
    target_compile_features(aowis-server-map-terrain-tile-format-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-tile-format
        COMMAND aowis-server-map-terrain-tile-format-test
    )

    add_executable(aowis-server-map-terrain-tile-delivery-test
        tests/test_terrain_tile_delivery.cpp
    )
    target_link_libraries(aowis-server-map-terrain-tile-delivery-test
        PRIVATE
            AOWIS::Map
    )
    target_compile_features(aowis-server-map-terrain-tile-delivery-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-tile-delivery
        COMMAND aowis-server-map-terrain-tile-delivery-test
    )

    add_executable(aowis-server-map-terrain-provider-test
        tests/test_terrain_provider.cpp
    )
    target_link_libraries(aowis-server-map-terrain-provider-test
        PRIVATE
            AOWIS::Map
            TIFF::TIFF
    )
    target_compile_features(aowis-server-map-terrain-provider-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-provider
        COMMAND aowis-server-map-terrain-provider-test
    )

    add_executable(aowis-server-map-terrain-elevation-sampling-test
        tests/test_terrain_elevation_sampling.cpp
    )
    target_link_libraries(aowis-server-map-terrain-elevation-sampling-test
        PRIVATE
            AOWIS::Map
    )
    target_compile_features(aowis-server-map-terrain-elevation-sampling-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-elevation-sampling
        COMMAND aowis-server-map-terrain-elevation-sampling-test
    )

    add_executable(aowis-server-map-terrain-vertical-datum-test
        tests/test_terrain_vertical_datum.cpp
    )
    target_link_libraries(aowis-server-map-terrain-vertical-datum-test
        PRIVATE
            AOWIS::Map
    )
    target_compile_features(aowis-server-map-terrain-vertical-datum-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-vertical-datum
        COMMAND aowis-server-map-terrain-vertical-datum-test
    )

    add_executable(aowis-server-map-terrain-dataset-catalog-test
        tests/test_terrain_dataset_catalog.cpp
    )
    target_link_libraries(aowis-server-map-terrain-dataset-catalog-test
        PRIVATE
            AOWIS::Map
    )
    target_compile_features(aowis-server-map-terrain-dataset-catalog-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-dataset-catalog
        COMMAND aowis-server-map-terrain-dataset-catalog-test
    )

    add_executable(aowis-server-map-terrain-cache-deletion-test
        tests/test_terrain_cache_deletion.cpp
    )
    target_link_libraries(aowis-server-map-terrain-cache-deletion-test
        PRIVATE
            AOWIS::Map
    )
    target_compile_features(aowis-server-map-terrain-cache-deletion-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-cache-deletion
        COMMAND aowis-server-map-terrain-cache-deletion-test
    )
endif()

if(AOWIS_SERVER_MAP_BUILD_SERVER)
    find_package(Qt6 REQUIRED COMPONENTS HttpServer Concurrent)

    qt_add_executable(aowis-server-map-server
        src/server/main.cpp
        src/server/configuration.cpp
        src/server/configuration.h
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
            Qt6::Concurrent
    )

    target_compile_features(aowis-server-map-server
        PRIVATE
            cxx_std_20
    )

    if(BUILD_TESTING)
        add_executable(aowis-server-map-provider-state-test
            tests/test_provider_state.cpp
            src/server/configuration.cpp
            src/server/configuration.h
        )
        target_link_libraries(aowis-server-map-provider-state-test
            PRIVATE AOWIS::Map Qt6::HttpServer)
        target_compile_features(aowis-server-map-provider-state-test
            PRIVATE cxx_std_20)
        add_test(NAME aowis-server-map-provider-state
                 COMMAND aowis-server-map-provider-state-test)
    endif()
endif()
