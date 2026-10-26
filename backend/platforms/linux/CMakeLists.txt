# Linux desktop platform: the parts of `api::Ports` that only exist here.
#
# Nothing in a capability may include this module. A capability takes the port by
# reference; the composition root of the process (a `main.cpp`) decides which
# implementation to hand it. That is what keeps `speech-recognition/` free of
# `pw-record` and of `XDG_RUNTIME_DIR`.
if(UNIX AND NOT APPLE)
    add_library(superior_linux_platform STATIC
        src/AudioCapture.cpp)

    add_library(Superior::LinuxPlatform ALIAS superior_linux_platform)

    target_include_directories(superior_linux_platform PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

    target_link_libraries(superior_linux_platform
        PUBLIC Superior::Api)

    find_package(Threads REQUIRED)
    target_link_libraries(superior_linux_platform PRIVATE Threads::Threads)

    target_compile_features(superior_linux_platform PUBLIC cxx_std_20)
    superior_set_warnings(superior_linux_platform)
endif()
