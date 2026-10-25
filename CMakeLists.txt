cmake_minimum_required(VERSION 3.25)

project(SuperiorIME
    VERSION 0.1.0
    DESCRIPTION "Cross-platform input method UI, Linux/Fcitx5 first"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

# Some distro CMake packages leave DATADIR empty; never let an empty value turn
# an install destination into an absolute /fcitx5 path.
if(NOT CMAKE_INSTALL_DATADIR)
    set(CMAKE_INSTALL_DATADIR "${CMAKE_INSTALL_DATAROOTDIR}")
endif()

include(SuperiorOptions)
include(SuperiorWarnings)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

enable_testing()

add_subdirectory(backend/diagnostics)
add_subdirectory(backend/api)
add_subdirectory(backend/skin)
add_subdirectory(backend/config)
add_subdirectory(backend/cloudpinyin)
add_subdirectory(backend/rime)
add_subdirectory(backend/theme)
add_subdirectory(backend/core)
add_subdirectory(backend/protocol)
add_subdirectory(backend/platforms/fcitx5)

if(SUPERIOR_BUILD_TOOLS)
    add_subdirectory(tools/skin-lint)
    add_subdirectory(tools/scene-sim)
    add_subdirectory(tools/superior-config)
endif()

if(SUPERIOR_ENABLE_QT)
    add_subdirectory(frontend/desktop)
    add_subdirectory(frontend/settings)
endif()

if(SUPERIOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

message(STATUS "Superior-IME ${PROJECT_VERSION}")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests         : ${SUPERIOR_BUILD_TESTS}")
message(STATUS "  tools         : ${SUPERIOR_BUILD_TOOLS}")
message(STATUS "  Qt frontend   : ${SUPERIOR_ENABLE_QT}")
message(STATUS "  Fcitx5 agent  : ${SUPERIOR_ENABLE_FCITX5}")

# Skins are data, installed next to the binaries so a system install works
# without any per-user staging step.
if(SUPERIOR_ENABLE_QT)
    install(DIRECTORY skins/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/superior-ime/skins"
        FILES_MATCHING PATTERN "skin.json"
        PATTERN "*.svg"
        PATTERN "*.png")
endif()
