# librime is loaded at runtime via dlopen, so this module has no link-time
# dependency on it. That keeps the addon loadable on a machine without librime
# (it reports the reason) and keeps these sources unit testable.
add_library(superior_rime STATIC
    src/LibrimePort.cpp
    src/RimeEngine.cpp
    src/RimePaths.cpp)

add_library(Superior::Rime ALIAS superior_rime)

target_include_directories(superior_rime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/superior-rime>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/rime")

target_link_libraries(superior_rime
    PRIVATE Superior::Diagnostics
    PRIVATE ${CMAKE_DL_LIBS})

target_compile_features(superior_rime PUBLIC cxx_std_20)
superior_set_warnings(superior_rime)
