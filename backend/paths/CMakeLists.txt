add_library(superior_paths STATIC
    src/Roots.cpp)

add_library(Superior::Paths ALIAS superior_paths)

target_include_directories(superior_paths PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-paths>)

target_compile_features(superior_paths PUBLIC cxx_std_20)

superior_set_warnings(superior_paths)
