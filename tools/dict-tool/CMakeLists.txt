add_executable(superior_dict main.cpp)
target_link_libraries(superior_dict PRIVATE
    Superior::Fcitx5Mapping
    Superior::Diagnostics)
target_compile_features(superior_dict PRIVATE cxx_std_20)
superior_set_warnings(superior_dict)
