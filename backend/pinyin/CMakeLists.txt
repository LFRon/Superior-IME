# The pinyin capability: a platform-neutral state machine over a `RimePort`,
# with librime as today's implementation.
#
# librime is loaded at runtime via dlopen, so this module has no link-time
# dependency on it. That keeps the addon loadable on a machine without librime
# (it reports the reason) and keeps these sources unit testable.
add_library(superior_pinyin STATIC
    src/Layout.cpp
    src/LibrimePort.cpp
    src/RimeEngine.cpp)

add_library(Superior::Pinyin ALIAS superior_pinyin)

target_include_directories(superior_pinyin
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/superior-pinyin>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/rime")

target_link_libraries(superior_pinyin
    PRIVATE Superior::Diagnostics
    PRIVATE ${CMAKE_DL_LIBS})

target_compile_features(superior_pinyin PUBLIC cxx_std_20)
superior_set_warnings(superior_pinyin)
