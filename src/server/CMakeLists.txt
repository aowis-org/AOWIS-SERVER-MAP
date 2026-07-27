qt_add_executable(aowis-server-map-server
    main.cpp
    server.h
    server.cpp
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
