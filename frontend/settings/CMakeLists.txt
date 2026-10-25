find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(superior_settings
    src/SettingsWindow.cpp
    src/main.cpp)

target_link_libraries(superior_settings PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Superior::Api
    Superior::Skin
    Superior::Config
    Superior::Theme
    Superior::Diagnostics)

target_compile_features(superior_settings PRIVATE cxx_std_20)
superior_set_warnings(superior_settings)

install(TARGETS superior_settings RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/superior-settings.desktop.in"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    RENAME superior-settings.desktop)
