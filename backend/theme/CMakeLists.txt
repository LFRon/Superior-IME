add_library(superior_theme STATIC
    src/ClassicUiTheme.cpp)

add_library(Superior::Theme ALIAS superior_theme)

target_include_directories(superior_theme PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-theme>)

target_link_libraries(superior_theme
    PUBLIC Superior::Skin
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_theme PUBLIC cxx_std_20)
superior_set_warnings(superior_theme)
