find_package(nlohmann_json 3.11 REQUIRED)

add_library(superior_skin STATIC
    src/Canonical.cpp
    src/Diagnostic.cpp
    src/Skin.cpp)

add_library(Superior::Skin ALIAS superior_skin)

target_include_directories(superior_skin PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-skin>)

target_link_libraries(superior_skin
    PUBLIC Superior::Api
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_skin PUBLIC cxx_std_20)

superior_set_warnings(superior_skin)
