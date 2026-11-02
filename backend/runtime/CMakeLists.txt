# The platform-neutral runtime: sessions, key routing, modes, effects.
#
# It depends on the contract and the pure policies, never on a capability
# implementation or a platform header. The host's composition root injects a
# `ComposerFactory` and a `SpeechPort`.
add_library(superior_runtime STATIC
    src/Runtime.cpp)

add_library(Superior::Runtime ALIAS superior_runtime)

target_include_directories(superior_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-runtime>)

target_link_libraries(superior_runtime
    PUBLIC Superior::Api
    PUBLIC Superior::Core
    PUBLIC Superior::Config
    PRIVATE Superior::Diagnostics)

target_compile_features(superior_runtime PUBLIC cxx_std_20)
superior_set_warnings(superior_runtime)
