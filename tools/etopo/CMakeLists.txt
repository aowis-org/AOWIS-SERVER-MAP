cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AOWIS_ETOPO_TOOLS LANGUAGES CXX)
    include(CTest)
    find_package(TIFF REQUIRED)
endif()
add_executable(aowis-map-etopo etopo.cpp)
set_target_properties(aowis-map-etopo PROPERTIES AUTOMOC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(aowis-map-etopo PRIVATE TIFF::TIFF)
target_compile_features(aowis-map-etopo PRIVATE cxx_std_20)
target_include_directories(aowis-map-etopo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
if(BUILD_TESTING)
    add_test(NAME aowis-server-map-etopo-native
        COMMAND aowis-map-etopo self-test)
    add_test(NAME aowis-server-map-etopo-provisioning
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_provisioning.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_global_terrain_data.sh")
    set_tests_properties(aowis-server-map-etopo-native aowis-server-map-etopo-provisioning
        PROPERTIES LABELS aowis)
endif()
