add_library(superior_fcitx5_mapping STATIC
    src/InlineText.cpp
    src/PreeditSource.cpp
    src/ShiftTap.cpp
    src/StatusMapping.cpp)

add_library(Superior::Fcitx5Mapping ALIAS superior_fcitx5_mapping)

target_include_directories(superior_fcitx5_mapping PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(superior_fcitx5_mapping
    PUBLIC Superior::Core
    PRIVATE Superior::Diagnostics
    PRIVATE Superior::Theme)

target_compile_features(superior_fcitx5_mapping PUBLIC cxx_std_20)
superior_set_warnings(superior_fcitx5_mapping)

set(SUPERIOR_FCITX5_ADDON_DIR "${CMAKE_INSTALL_LIBDIR}/fcitx5" CACHE PATH
    "Directory scanned by Fcitx5 for addon libraries")
set(SUPERIOR_FCITX5_DATA_DIR "${CMAKE_INSTALL_DATADIR}/fcitx5/addon" CACHE PATH
    "Directory scanned by Fcitx5 for addon metadata")
set(SUPERIOR_FCITX5_INPUTMETHOD_DIR "${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod" CACHE PATH
    "Directory scanned by Fcitx5 for input method entries")
set(SUPERIOR_ICON_DIR "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" CACHE PATH
    "Icon theme directory for the Superior input method icon")

if(SUPERIOR_ENABLE_FCITX5)
    find_package(Fcitx5Core 5.1 REQUIRED)
    set(FCITX5_CORE_VERSION "${Fcitx5Core_VERSION}")

    add_library(superior_fcitx5_agent MODULE
        src/SuperiorAgent.cpp)

    target_include_directories(superior_fcitx5_agent PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_link_libraries(superior_fcitx5_agent PRIVATE
        Fcitx5::Core
        Superior::Fcitx5Mapping
        Superior::Protocol
        Superior::Config
        Superior::CloudPinyin
        Superior::Diagnostics)

    set_target_properties(superior_fcitx5_agent PROPERTIES
        OUTPUT_NAME superioragent)

    target_compile_features(superior_fcitx5_agent PRIVATE cxx_std_20)
    superior_set_warnings(superior_fcitx5_agent)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/data/superioragent.conf.in"
        "${CMAKE_CURRENT_BINARY_DIR}/superioragent.conf"
        @ONLY)

    # librime engine. A separate addon library, so a machine without librime
    # still loads the agent and Fcitx5 reports what is missing instead of failing
    # the whole addon tree.
    add_library(superior_fcitx5_pinyin MODULE
        src/RimeInputMethod.cpp)

    target_include_directories(superior_fcitx5_pinyin PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

    target_link_libraries(superior_fcitx5_pinyin PRIVATE
        Fcitx5::Core
        Superior::Core
        Superior::Pinyin
        Superior::Diagnostics)

    set_target_properties(superior_fcitx5_pinyin PROPERTIES
        OUTPUT_NAME superiorpinyin)

    target_compile_features(superior_fcitx5_pinyin PRIVATE cxx_std_20)
    superior_set_warnings(superior_fcitx5_pinyin)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/data/superiorpinyin.conf.in"
        "${CMAKE_CURRENT_BINARY_DIR}/superiorpinyin.conf"
        @ONLY)

    # The input method entry is a plain file: Fcitx5 derives the unique name
    # from the file name, so it must be installed as "super-ime.conf".
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/data/super-ime.conf.in"
        "${CMAKE_CURRENT_BINARY_DIR}/super-ime.conf"
        COPYONLY)

    install(TARGETS superior_fcitx5_agent superior_fcitx5_pinyin
        LIBRARY DESTINATION "${SUPERIOR_FCITX5_ADDON_DIR}")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/superioragent.conf"
        "${CMAKE_CURRENT_BINARY_DIR}/superiorpinyin.conf"
        DESTINATION "${SUPERIOR_FCITX5_DATA_DIR}")

    # Exactly one entry: its Addon is superiorrime, so there is no second
    # libime-backed entry to choose between.
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/super-ime.conf"
        DESTINATION "${SUPERIOR_FCITX5_INPUTMETHOD_DIR}")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/org.superior.ime.svg"
        DESTINATION "${SUPERIOR_ICON_DIR}")
endif()
