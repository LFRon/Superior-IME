add_library(superior_core STATIC
    src/CandidateModel.cpp
    src/ImeSession.cpp
    src/MotionTimeline.cpp)

add_library(Superior::Core ALIAS superior_core)

target_include_directories(superior_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-core>)

target_link_libraries(superior_core
    PUBLIC Superior::Api
    PUBLIC Superior::Skin
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_core PUBLIC cxx_std_20)

superior_set_warnings(superior_core)
