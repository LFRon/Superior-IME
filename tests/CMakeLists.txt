add_executable(superior_diagnostics_tests diagnostics_tests.cpp)
target_link_libraries(superior_diagnostics_tests PRIVATE Superior::Diagnostics)
target_include_directories(superior_diagnostics_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_diagnostics_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_diagnostics_tests)

add_executable(superior_skin_tests skin_tests.cpp)
target_link_libraries(superior_skin_tests PRIVATE Superior::Skin)
target_compile_definitions(superior_skin_tests PRIVATE
    SUPERIOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_features(superior_skin_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_skin_tests)

add_executable(superior_inline_text_tests inline_text_tests.cpp)
target_link_libraries(superior_inline_text_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_inline_text_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_inline_text_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_inline_text_tests)

add_executable(superior_candidate_actions_tests candidate_actions_tests.cpp)
target_link_libraries(superior_candidate_actions_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_candidate_actions_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_candidate_actions_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_candidate_actions_tests)

add_executable(superior_shift_tap_tests shift_tap_tests.cpp)
target_link_libraries(superior_shift_tap_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_shift_tap_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_shift_tap_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_shift_tap_tests)

# The platform choice is pure logic living in the UI layer: exercise it without
# linking Qt, so it stays a decision function instead of growing side effects.
add_executable(superior_qt_platform_tests
    qt_platform_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/PlatformChoice.cpp")
target_include_directories(superior_qt_platform_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_qt_platform_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_qt_platform_tests)

add_executable(superior_layout_rules_tests
    layout_rules_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/LayoutRules.cpp")
target_include_directories(superior_layout_rules_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_layout_rules_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_layout_rules_tests)

add_executable(superior_candidate_label_tests
    candidate_label_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/CandidateLabel.cpp")
target_include_directories(superior_candidate_label_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_candidate_label_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_candidate_label_tests)

add_executable(superior_preedit_layout_tests
    preedit_layout_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/PreeditLayout.cpp")
target_link_libraries(superior_preedit_layout_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_preedit_layout_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_preedit_layout_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_preedit_layout_tests)

add_executable(superior_wheel_step_tests
    wheel_step_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/WheelStep.cpp")
target_include_directories(superior_wheel_step_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_wheel_step_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_wheel_step_tests)

add_executable(superior_config_tests config_tests.cpp)
target_link_libraries(superior_config_tests PRIVATE Superior::Config)
target_include_directories(superior_config_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_config_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_config_tests)

add_executable(superior_inputmethod_tests inputmethod_tests.cpp)
target_compile_definitions(superior_inputmethod_tests PRIVATE
    SUPERIOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(superior_inputmethod_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_inputmethod_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_inputmethod_tests)

add_executable(superior_theme_tests theme_tests.cpp)
target_link_libraries(superior_theme_tests PRIVATE Superior::Theme)
target_compile_definitions(superior_theme_tests PRIVATE
    SUPERIOR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(superior_theme_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_theme_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_theme_tests)

add_executable(superior_dictionary_tests dictionary_tests.cpp)
target_link_libraries(superior_dictionary_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_dictionary_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_dictionary_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_dictionary_tests)

add_executable(superior_core_tests core_tests.cpp)
target_link_libraries(superior_core_tests PRIVATE Superior::Core)
target_include_directories(superior_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_core_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_core_tests)

add_executable(superior_protocol_tests protocol_tests.cpp)
target_link_libraries(superior_protocol_tests PRIVATE Superior::Protocol)
target_include_directories(superior_protocol_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_protocol_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_protocol_tests)

add_executable(superior_fcitx5_mapper_tests fcitx5_mapper_tests.cpp)
target_link_libraries(superior_fcitx5_mapper_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_fcitx5_mapper_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_fcitx5_mapper_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_fcitx5_mapper_tests)

add_executable(superior_socket_tests socket_tests.cpp)
target_link_libraries(superior_socket_tests PRIVATE Superior::Protocol)
target_include_directories(superior_socket_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_socket_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_socket_tests)

add_executable(superior_runtime_tests runtime_tests.cpp)
target_link_libraries(superior_runtime_tests PRIVATE Superior::Runtime)
target_include_directories(superior_runtime_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_runtime_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_runtime_tests)

add_test(NAME diagnostics COMMAND superior_diagnostics_tests)
add_test(NAME skin COMMAND superior_skin_tests)
add_test(NAME inline-text COMMAND superior_inline_text_tests)
add_test(NAME candidate-actions COMMAND superior_candidate_actions_tests)
add_test(NAME shift-tap COMMAND superior_shift_tap_tests)
add_test(NAME qt-platform COMMAND superior_qt_platform_tests)
add_test(NAME layout-rules COMMAND superior_layout_rules_tests)
add_test(NAME candidate-label COMMAND superior_candidate_label_tests)
add_test(NAME wheel-step COMMAND superior_wheel_step_tests)
add_test(NAME preedit-layout COMMAND superior_preedit_layout_tests)
add_test(NAME config COMMAND superior_config_tests)
add_test(NAME inputmethod COMMAND superior_inputmethod_tests)
add_test(NAME theme COMMAND superior_theme_tests)
add_test(NAME dictionary COMMAND superior_dictionary_tests)
add_test(NAME core COMMAND superior_core_tests)
add_test(NAME runtime COMMAND superior_runtime_tests)
add_test(NAME protocol COMMAND superior_protocol_tests)
add_test(NAME socket COMMAND superior_socket_tests)
add_test(NAME fcitx5-mapper COMMAND superior_fcitx5_mapper_tests)

add_test(
    NAME boundaries
    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/check-boundaries.sh" "${PROJECT_SOURCE_DIR}")

if(TARGET skin_lint)
    add_test(NAME skin-lint-default
        COMMAND skin_lint "${PROJECT_SOURCE_DIR}/skins/default" --strict)
endif()
