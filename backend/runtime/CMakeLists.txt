add_library(superior_runtime STATIC
    src/SerialPump.cpp
    src/SessionRuntime.cpp)

add_library(Superior::Runtime ALIAS superior_runtime)

target_include_directories(superior_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-runtime>)

target_link_libraries(superior_runtime
    PUBLIC Superior::Api
    PRIVATE Superior::Core
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_runtime PUBLIC cxx_std_20)

superior_set_warnings(superior_runtime)
