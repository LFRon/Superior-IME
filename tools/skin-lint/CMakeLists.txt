add_executable(skin_lint main.cpp)
target_link_libraries(skin_lint PRIVATE
    Superior::Skin
    Superior::Theme
    Superior::Diagnostics)
target_compile_features(skin_lint PRIVATE cxx_std_20)
superior_set_warnings(skin_lint)
