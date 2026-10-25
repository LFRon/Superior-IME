find_package(nlohmann_json 3.11 REQUIRED)

add_library(superior_config STATIC
    src/Fcitx5Config.cpp
    src/IniFile.cpp
    src/Settings.cpp)

add_library(Superior::Config ALIAS superior_config)

target_include_directories(superior_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-config>)

target_link_libraries(superior_config
    PUBLIC Superior::Api
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_config PUBLIC cxx_std_20)
superior_set_warnings(superior_config)
