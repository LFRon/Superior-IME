find_package(nlohmann_json 3.11 REQUIRED)

add_library(superior_protocol STATIC
    src/SceneServer.cpp
    src/Wire.cpp)

add_library(Superior::Protocol ALIAS superior_protocol)

target_include_directories(superior_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-protocol>)

target_link_libraries(superior_protocol
    PUBLIC Superior::Api
    # Public: the socket path is derived from the session's runtime root, and a
    # port that moves that root must move this too.
    PRIVATE Superior::Paths
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_protocol PUBLIC cxx_std_20)

superior_set_warnings(superior_protocol)
