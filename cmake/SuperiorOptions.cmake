option(SUPERIOR_BUILD_TESTS "Build unit tests" ON)
option(SUPERIOR_BUILD_TOOLS "Build developer tools (skin-lint, ...)" ON)
option(SUPERIOR_ENABLE_QT "Build the Qt desktop UI" OFF)
option(SUPERIOR_ENABLE_FCITX5 "Build the Fcitx5 observer addon" OFF)
option(SUPERIOR_ENABLE_SSF_IMPORT "Build the optional Sogou .ssf importer (P2)" OFF)
option(SUPERIOR_WERROR "Treat compiler warnings as errors" OFF)

if(SUPERIOR_ENABLE_SSF_IMPORT)
    message(FATAL_ERROR
        "SUPERIOR_ENABLE_SSF_IMPORT is reserved for P2 and is intentionally not implemented.")
endif()
