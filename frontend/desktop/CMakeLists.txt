find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# Icons are rendered from SVG when the module is available, and drawn by hand
# otherwise: a distribution that ships Qt without Qt6Svg must still build.
find_package(Qt6 QUIET COMPONENTS Svg)
if(Qt6Svg_FOUND)
    set(SUPERIOR_HAVE_QT_SVG ON)
endif()

add_executable(superior_ui
    src/CandidateLabel.cpp
    src/CandidateWindow.cpp
    src/DragHandle.cpp
    src/LayoutRules.cpp
    src/Logging.cpp
    src/ModelDownload.cpp
    src/Overlay.cpp
    src/OverlayLayout.cpp
    src/PlatformChoice.cpp
    src/PreeditLayout.cpp
    src/SceneClient.cpp
    src/SkinTheme.cpp
    src/SpeechRecognitionPanel.cpp
    src/StatusGlyph.cpp
    src/StatusWindow.cpp
    src/WheelStep.cpp
    src/main.cpp)

qt_add_resources(superior_ui "icons"
    PREFIX "/icons"
    BASE "assets"
    FILES
        assets/microphone.svg
        assets/tabler-microphone-off-outline.svg)

target_link_libraries(superior_ui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Superior::Api
    Superior::SpeechRecognition
    Superior::Protocol
    Superior::Skin
    Superior::Config
    Superior::Diagnostics)

if(SUPERIOR_HAVE_QT_SVG)
    target_link_libraries(superior_ui PRIVATE Qt6::Svg)
    target_compile_definitions(superior_ui PRIVATE SUPERIOR_HAVE_QT_SVG=1)
endif()

target_compile_features(superior_ui PRIVATE cxx_std_20)
superior_set_warnings(superior_ui)

install(TARGETS superior_ui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
