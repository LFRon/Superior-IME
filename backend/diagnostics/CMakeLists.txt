add_library(superior_diagnostics STATIC
    src/Log.cpp)

add_library(Superior::Diagnostics ALIAS superior_diagnostics)

target_include_directories(superior_diagnostics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-diagnostics>)

target_compile_features(superior_diagnostics PUBLIC cxx_std_20)

target_link_libraries(superior_diagnostics
    PRIVATE Superior::Paths)

superior_set_warnings(superior_diagnostics)
