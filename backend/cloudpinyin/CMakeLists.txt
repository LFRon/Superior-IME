find_package(CURL 7.60 REQUIRED)

add_library(superior_cloudpinyin STATIC
    src/Cloud.cpp
    src/CloudState.cpp
    src/Fetcher.cpp)

add_library(Superior::CloudPinyin ALIAS superior_cloudpinyin)

target_include_directories(superior_cloudpinyin PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-cloudpinyin>)

target_link_libraries(superior_cloudpinyin
    PUBLIC CURL::libcurl
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_cloudpinyin PUBLIC cxx_std_20)
superior_set_warnings(superior_cloudpinyin)
