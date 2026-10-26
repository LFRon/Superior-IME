# Host-neutral port implementations (`架构设计.md` §11 P5.5).
#
# This target links `Superior::Api` and nothing else — not Fcitx5, not Qt, not
# `Superior::Paths`. Keeping it that way is what proves the contract layer is
# free of platform vocabulary: the day an `api/` header needs a platform type,
# the build of a module that has none of them stops working.
add_library(superior_platform_stub STATIC
    src/ApiProbe.cpp
    src/StubPorts.cpp)

add_library(Superior::PlatformStub ALIAS superior_platform_stub)

target_include_directories(superior_platform_stub PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(superior_platform_stub PUBLIC Superior::Api)

target_compile_features(superior_platform_stub PUBLIC cxx_std_20)
superior_set_warnings(superior_platform_stub)
