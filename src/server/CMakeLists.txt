
qt_add_executable(aowis-server-map
    main.cpp
    server.h
    server.cpp
)

target_link_libraries(aowis-server-map
    PRIVATE
        aowis-server-map-core
        Qt6::Core
        Qt6::Network
        Qt6::HttpServer
)
