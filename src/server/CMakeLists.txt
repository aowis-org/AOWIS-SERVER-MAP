qt_add_executable(aowis-server-map
    main.cpp
    server.h
    server.cpp
)

target_link_libraries(aowis-server-map
    PRIVATE
        AOWIS::ServerMap
        Qt6::Core
        Qt6::Network
        Qt6::HttpServer
)

target_compile_features(aowis-server-map
    PRIVATE
        cxx_std_20
)
