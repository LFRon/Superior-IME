add_executable(superior_config_cli main.cpp)
target_link_libraries(superior_config_cli PRIVATE
    Superior::CloudPinyin
    Superior::Config
    Superior::Pinyin
    Superior::Skin
    Superior::Theme
    Superior::Diagnostics)
target_compile_features(superior_config_cli PRIVATE cxx_std_20)
superior_set_warnings(superior_config_cli)
