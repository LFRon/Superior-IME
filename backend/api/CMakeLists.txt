add_library(superior_api INTERFACE)
add_library(Superior::Api ALIAS superior_api)

target_include_directories(superior_api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(superior_api INTERFACE cxx_std_20)
