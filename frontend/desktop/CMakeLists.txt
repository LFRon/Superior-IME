find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

add_executable(superior_ui
    src/CandidateWindow.cpp
    src/DragHandle.cpp
    src/LayoutRules.cpp
    src/Logging.cpp
    src/Overlay.cpp
    src/PlatformChoice.cpp
    src/SceneClient.cpp
    src/SkinTheme.cpp
    src/StatusWindow.cpp
    src/main.cpp)

target_link_libraries(superior_ui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Superior::Api
    Superior::Protocol
    Superior::Skin
    Superior::Config
    Superior::Diagnostics)

target_compile_features(superior_ui PRIVATE cxx_std_20)
superior_set_warnings(superior_ui)

install(TARGETS superior_ui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
