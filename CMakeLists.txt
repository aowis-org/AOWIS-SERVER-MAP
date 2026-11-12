cmake_minimum_required(VERSION 3.21)

project(
    AOWIS_SERVER_MAP
    VERSION 0.1.0
    DESCRIPTION "Caching map tile library and REST server"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)

# The standalone MAP server owns its CONFIG dependency. When MAP is embedded
# in another AOWIS project (e.g. GUI), reuse that project's existing target.
if(NOT TARGET aowis::config)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-CONFIG/CMakeLists.txt")
        message(FATAL_ERROR
            "AOWIS-CONFIG submodule is missing. Run ./git_submodule_init.sh "
            "or git submodule update --init -- external/AOWIS-CONFIG")
    endif()
    add_subdirectory(external/AOWIS-CONFIG EXCLUDE_FROM_ALL)
    # CONFIG is a plain C++ library. Inherit no Qt AUTOMOC from MAP.
    set_target_properties(aowis-config PROPERTIES AUTOMOC OFF)
endif()

option(
    AOWIS_SERVER_MAP_BUILD_SERVER
    "Build AOWIS map REST server executable"
    ON
)

find_package(Qt6 REQUIRED COMPONENTS Core Network Concurrent)
find_package(TIFF REQUIRED)
find_package(PNG REQUIRED)
find_path(AOWIS_TURBOJPEG_INCLUDE_DIR NAMES turbojpeg.h REQUIRED)
find_library(AOWIS_TURBOJPEG_LIBRARY NAMES turbojpeg REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AOWIS_ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_library(aowis-server-map-core STATIC
    src/lib/maptiles.cpp
    src/lib/download_coverage.cpp
    src/lib/map_provider.cpp
    src/lib/settings_repository.cpp
    include/aowis/map/settings_repository.h
    include/aowis/map/map_provider.h
    include/aowis/map/download_coverage.h
    src/lib/http_client_tilefetch.cpp
    src/lib/http_client_tilefetch.h
    src/lib/terrain_elevation.cpp
    src/lib/terrain_tile.cpp
    src/lib/terrain_transfer.cpp
    src/lib/raster_transfer.cpp
    include/aowis/map/raster_transfer.h
    include/aowis/map/terrain_transfer.h
    src/lib/terrain_provider.cpp
    src/lib/terrain_dataset.cpp
    src/lib/copernicus_terrain_provider.cpp
    src/lib/copernicus_terrain_provider.h
    src/lib/terrain_data.cpp
    include/aowis/map/maptiles.h
    include/aowis/map/map_cascade_config.h
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
        "${AOWIS_TURBOJPEG_INCLUDE_DIR}"
)

target_link_libraries(aowis-server-map-core
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Concurrent
        aowis::config
    PRIVATE
        TIFF::TIFF
        PkgConfig::AOWIS_ZSTD
        PNG::PNG
        "${AOWIS_TURBOJPEG_LIBRARY}"
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
    # Optional benchmark is available in test builds, not release packages.
    add_executable(aowis-server-map-transfer-benchmark
        tools/benchmark_cascade_transfer.cpp)
    target_link_libraries(aowis-server-map-transfer-benchmark
        PRIVATE AOWIS::Map PNG::PNG)
    target_compile_features(aowis-server-map-transfer-benchmark PRIVATE cxx_std_20)
    add_executable(aowis-server-map-legacy-standalone-migration-test
        tests/test_map_legacy_standalone_migration.cpp)
    target_link_libraries(aowis-server-map-legacy-standalone-migration-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-legacy-standalone-migration
        COMMAND aowis-server-map-legacy-standalone-migration-test)
    set_tests_properties(aowis-server-map-legacy-standalone-migration PROPERTIES LABELS "aowis")

    add_executable(aowis-server-map-settings-repository-test
        tests/test_map_settings_repository.cpp)
    target_link_libraries(aowis-server-map-settings-repository-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-settings-repository
             COMMAND aowis-server-map-settings-repository-test)

    add_executable(aowis-server-map-provider-limits-test
        tests/test_map_provider_limits.cpp
    )
    target_link_libraries(aowis-server-map-provider-limits-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-provider-limits
             COMMAND aowis-server-map-provider-limits-test)
    add_executable(aowis-server-map-raster-transfer-test
        tests/test_raster_transfer.cpp)
    target_link_libraries(aowis-server-map-raster-transfer-test
        PRIVATE AOWIS::Map PNG::PNG)
    add_test(NAME aowis-server-map-raster-transfer
             COMMAND aowis-server-map-raster-transfer-test)
    add_executable(aowis-server-map-raster-cascade-test
        tests/test_map_raster_cascade.cpp
    )
    target_link_libraries(aowis-server-map-raster-cascade-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-raster-cascade
             COMMAND aowis-server-map-raster-cascade-test)

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

    add_executable(aowis-server-map-terrain-cascade-test
        tests/test_map_terrain_cascade.cpp
    )
    target_link_libraries(aowis-server-map-terrain-cascade-test PRIVATE AOWIS::Map)
    add_test(NAME aowis-server-map-terrain-cascade
             COMMAND aowis-server-map-terrain-cascade-test)

    add_executable(aowis-server-map-terrain-transfer-test
        tests/test_terrain_transfer.cpp
    )
    target_link_libraries(aowis-server-map-terrain-transfer-test
        PRIVATE AOWIS::Map PkgConfig::AOWIS_ZSTD)
    add_test(NAME aowis-server-map-terrain-transfer
             COMMAND aowis-server-map-terrain-transfer-test)

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

    add_executable(aowis-server-map-copernicus-resume-http-test
        tests/test_copernicus_resume_http.cpp
    )
    target_link_libraries(aowis-server-map-copernicus-resume-http-test
        PRIVATE AOWIS::Map Qt6::Network Qt6::Concurrent
    )
    add_test(NAME aowis-server-map-copernicus-resume-http
             COMMAND aowis-server-map-copernicus-resume-http-test)

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

    add_executable(aowis-server-map-terrain-area-average-test
        tests/test_terrain_area_average.cpp
    )
    target_link_libraries(aowis-server-map-terrain-area-average-test
        PRIVATE
            AOWIS::Map
            TIFF::TIFF
    )
    target_compile_features(aowis-server-map-terrain-area-average-test
        PRIVATE
            cxx_std_20
    )
    add_test(
        NAME aowis-server-map-terrain-area-average
        COMMAND aowis-server-map-terrain-area-average-test
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
        src/server/terrain_admission.h
        src/server/terrain_byte_admission.h
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
        add_executable(aowis-server-map-cascade-configuration-test
            tests/test_map_cascade_configuration.cpp
            src/server/configuration.cpp
            src/server/configuration.h
        )
        target_link_libraries(aowis-server-map-cascade-configuration-test
            PRIVATE AOWIS::Map Qt6::HttpServer)
        add_test(NAME aowis-server-map-cascade-configuration
                 COMMAND aowis-server-map-cascade-configuration-test)

        add_executable(aowis-server-map-managed-settings-test
            tests/test_map_server_managed_settings.cpp
            src/server/server.cpp
            src/server/server.h
            src/server/configuration.cpp
            src/server/configuration.h)
        target_link_libraries(aowis-server-map-managed-settings-test
            PRIVATE AOWIS::Map Qt6::HttpServer)
        add_test(NAME aowis-server-map-managed-settings
            COMMAND aowis-server-map-managed-settings-test)

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
        add_executable(aowis-server-map-cascade-chain-test
            tests/test_map_cascade_chain.cpp
            src/server/server.cpp
            src/server/server.h
            src/server/configuration.cpp
            src/server/configuration.h)
        target_include_directories(aowis-server-map-cascade-chain-test
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/server")
        target_link_libraries(aowis-server-map-cascade-chain-test
            PRIVATE AOWIS::Map Qt6::HttpServer Qt6::Concurrent PNG::PNG)
        add_test(NAME aowis-server-map-cascade-chain
            COMMAND aowis-server-map-cascade-chain-test)
        set_tests_properties(aowis-server-map-cascade-chain
            PROPERTIES TIMEOUT 60)
        add_executable(aowis-server-map-raster-jpeg-http-test
            tests/test_raster_jpeg_http.cpp
            src/server/server.cpp
            src/server/server.h
            src/server/configuration.cpp
            src/server/configuration.h)
        target_include_directories(aowis-server-map-raster-jpeg-http-test
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/server")
        target_link_libraries(aowis-server-map-raster-jpeg-http-test
            PRIVATE AOWIS::Map Qt6::HttpServer Qt6::Concurrent PNG::PNG)
        add_test(NAME aowis-server-map-raster-jpeg-http
            COMMAND aowis-server-map-raster-jpeg-http-test)
        add_executable(aowis-server-map-terrain-zstd-http-test
            tests/test_terrain_zstd_http.cpp
            src/server/server.cpp
            src/server/server.h
            src/server/configuration.cpp
            src/server/configuration.h
        )
        target_include_directories(aowis-server-map-terrain-zstd-http-test
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/server")
        target_link_libraries(aowis-server-map-terrain-zstd-http-test
            PRIVATE AOWIS::Map Qt6::HttpServer Qt6::Concurrent)
        add_test(NAME aowis-server-map-terrain-zstd-http
            COMMAND aowis-server-map-terrain-zstd-http-test)

        add_executable(aowis-server-map-terrain-singleflight-http-test
            tests/test_terrain_singleflight_http.cpp
            src/server/server.cpp
            src/server/server.h
            src/server/configuration.cpp
            src/server/configuration.h
        )
        target_include_directories(aowis-server-map-terrain-singleflight-http-test
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/server")
        target_link_libraries(aowis-server-map-terrain-singleflight-http-test
            PRIVATE AOWIS::Map Qt6::HttpServer Qt6::Concurrent)
        target_compile_features(aowis-server-map-terrain-singleflight-http-test
            PRIVATE cxx_std_20)
        add_test(NAME aowis-server-map-terrain-singleflight-http
            COMMAND aowis-server-map-terrain-singleflight-http-test)
        # Extended lifecycle stress can repeat all cancellation/shutdown cases
        # up to 50 times via AOWIS_TERRAIN_LIFECYCLE_REPETITIONS.
        set_tests_properties(aowis-server-map-terrain-singleflight-http
            PROPERTIES TIMEOUT 60)
    endif()
endif()

# Aggregate native test target for compile_linux_tests.sh. The labels allow the
# runner to distinguish this project's tests from any tests in dependencies.
if(BUILD_TESTING)
    add_test(NAME aowis-server-map-transfer-benchmark-smoke
        COMMAND aowis-server-map-transfer-benchmark --self-test)
    set_tests_properties(aowis-server-map-transfer-benchmark-smoke
        PROPERTIES LABELS "aowis")
    set(AOWIS_SERVER_MAP_TEST_TARGETS
        aowis-server-map-transfer-benchmark
        aowis-server-map-settings-repository-test
        aowis-server-map-legacy-standalone-migration-test
        aowis-server-map-provider-limits-test
        aowis-server-map-cache-reuse-test
        aowis-server-map-raster-cascade-test
        aowis-server-map-raster-transfer-test
        aowis-server-map-download-coverage-test
        aowis-server-map-terrain-tile-format-test
        aowis-server-map-terrain-tile-delivery-test
        aowis-server-map-terrain-cascade-test
        aowis-server-map-terrain-transfer-test
        aowis-server-map-terrain-provider-test
        aowis-server-map-copernicus-resume-http-test
        aowis-server-map-terrain-elevation-sampling-test
        aowis-server-map-terrain-vertical-datum-test
        aowis-server-map-terrain-dataset-catalog-test
        aowis-server-map-terrain-cache-deletion-test
        aowis-server-map-terrain-area-average-test
    )
    if(AOWIS_SERVER_MAP_BUILD_SERVER)
        # Compiling only the unit tests misses API errors in server.cpp.
        # The native test target must compile the production executable too.
        list(APPEND AOWIS_SERVER_MAP_TEST_TARGETS
            aowis-server-map-cascade-configuration-test
            aowis-server-map-managed-settings-test
            aowis-server-map-provider-state-test
            aowis-server-map-terrain-singleflight-http-test
            aowis-server-map-terrain-zstd-http-test
            aowis-server-map-cascade-chain-test
            aowis-server-map-raster-jpeg-http-test
            aowis-server-map-server)
    endif()
    add_executable(aowis-server-map-terrain-admission-test
        tests/test_terrain_admission.cpp)
    add_test(NAME aowis-server-map-terrain-admission
        COMMAND aowis-server-map-terrain-admission-test)
    list(APPEND AOWIS_SERVER_MAP_TEST_TARGETS aowis-server-map-terrain-admission-test)
    add_executable(aowis-server-map-terrain-byte-admission-test
        tests/test_terrain_byte_admission.cpp)
    add_test(NAME aowis-server-map-terrain-byte-admission
        COMMAND aowis-server-map-terrain-byte-admission-test)
    list(APPEND AOWIS_SERVER_MAP_TEST_TARGETS aowis-server-map-terrain-byte-admission-test)
    # Every test executable must be built by aowis-server-map-tests, or
    # compile_linux_tests.sh reports its test as "Not Run". Fail at configure
    # time instead of when a new test is forgotten in the list above.
    get_property(AOWIS_SERVER_MAP_DIRECTORY_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target_name IN LISTS AOWIS_SERVER_MAP_DIRECTORY_TARGETS)
        if(target_name MATCHES "^aowis-server-map-.*-test$" AND
           NOT target_name IN_LIST AOWIS_SERVER_MAP_TEST_TARGETS)
            message(FATAL_ERROR
                "Test executable ${target_name} is missing from AOWIS_SERVER_MAP_TEST_TARGETS")
        endif()
    endforeach()
    add_custom_target(aowis-server-map-tests
        DEPENDS ${AOWIS_SERVER_MAP_TEST_TARGETS})
    get_property(AOWIS_SERVER_MAP_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS AOWIS_SERVER_MAP_REGISTERED_TESTS)
        if(test_name MATCHES "^aowis-server-map-")
            set_property(TEST "${test_name}" APPEND PROPERTY LABELS aowis)
        endif()
    endforeach()
endif()
