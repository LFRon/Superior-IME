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

add_executable(superior_overlay_layout_tests
    overlay_layout_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/OverlayLayout.cpp"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/LayoutRules.cpp")
target_include_directories(superior_overlay_layout_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_overlay_layout_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_overlay_layout_tests)

add_executable(superior_drag_handle_tests
    drag_handle_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/DragHandle.cpp"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/LayoutRules.cpp")
target_include_directories(superior_drag_handle_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_drag_handle_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_drag_handle_tests)

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

add_executable(superior_status_mapping_tests
    status_mapping_tests.cpp
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src/StatusGlyph.cpp")
target_link_libraries(superior_status_mapping_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_status_mapping_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/frontend/desktop/src")
target_compile_features(superior_status_mapping_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_status_mapping_tests)

add_executable(superior_status_bar_state_tests status_bar_state_tests.cpp)
target_link_libraries(superior_status_bar_state_tests PRIVATE Superior::Config)
target_include_directories(superior_status_bar_state_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_status_bar_state_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_status_bar_state_tests)

add_executable(superior_learning_tests learning_tests.cpp)
target_link_libraries(superior_learning_tests PRIVATE Superior::Config)
target_include_directories(superior_learning_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_learning_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_learning_tests)

add_executable(superior_pinyin_engine_tests pinyin_engine_tests.cpp)
target_link_libraries(superior_pinyin_engine_tests PRIVATE Superior::Pinyin)
target_include_directories(superior_pinyin_engine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_pinyin_engine_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_pinyin_engine_tests)

add_executable(superior_cloud_tests cloud_tests.cpp)
target_link_libraries(superior_cloud_tests PRIVATE Superior::CloudPinyin)
target_include_directories(superior_cloud_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_cloud_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_cloud_tests)

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

add_executable(superior_core_tests core_tests.cpp)
target_link_libraries(superior_core_tests PRIVATE Superior::Core)
target_include_directories(superior_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_core_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_core_tests)

add_executable(superior_modes_tests modes_tests.cpp)
target_link_libraries(superior_modes_tests PRIVATE Superior::Core)
target_include_directories(superior_modes_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_modes_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_modes_tests)

# The keysym -> KeyCode table is the seam between the Fcitx5 adapter and the
# decision layer: if a key maps to `Other` the policy silently stops applying.
add_executable(superior_key_translation_tests key_translation_tests.cpp)
target_link_libraries(superior_key_translation_tests PRIVATE Superior::Fcitx5Mapping)
target_include_directories(superior_key_translation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_key_translation_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_key_translation_tests)

add_executable(superior_key_policy_tests key_policy_tests.cpp)
target_link_libraries(superior_key_policy_tests PRIVATE Superior::Core)
target_include_directories(superior_key_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_key_policy_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_key_policy_tests)

# Every capability resolves its directories through Superior::Paths, so the
# fallback chain has to be pinned where nobody can quietly re-derive it.
# The single exit for text. Tested with a fake port so the hold / drop rules
# are pinned without a window system.
add_executable(superior_text_outbox_tests text_outbox_tests.cpp)
target_link_libraries(superior_text_outbox_tests PRIVATE
    Superior::Core
    Superior::PlatformStub)
target_include_directories(superior_text_outbox_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_text_outbox_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_text_outbox_tests)

add_executable(superior_paths_tests paths_tests.cpp)
target_link_libraries(superior_paths_tests PRIVATE Superior::Paths)
target_include_directories(superior_paths_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_paths_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_paths_tests)

add_executable(superior_scene_input_tests scene_input_tests.cpp)
target_link_libraries(superior_scene_input_tests PRIVATE Superior::Core)
target_include_directories(superior_scene_input_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_scene_input_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_scene_input_tests)

add_executable(superior_pinyin_layout_tests pinyin_layout_tests.cpp)
target_link_libraries(superior_pinyin_layout_tests PRIVATE Superior::Pinyin)
target_include_directories(superior_pinyin_layout_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_pinyin_layout_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_pinyin_layout_tests)

add_executable(superior_golden_pinyin_tests golden_pinyin_tests.cpp)
target_link_libraries(superior_golden_pinyin_tests PRIVATE Superior::Pinyin)
target_include_directories(superior_golden_pinyin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_golden_pinyin_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_golden_pinyin_tests)

add_executable(superior_audio_capture_tests audio_capture_tests.cpp)
target_link_libraries(superior_audio_capture_tests PRIVATE Superior::LinuxPlatform)
target_include_directories(superior_audio_capture_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_audio_capture_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_audio_capture_tests)

add_executable(superior_port_conformance_tests conformance/port_conformance_tests.cpp)
target_link_libraries(superior_port_conformance_tests PRIVATE
    Superior::Core
    Superior::PlatformStub)
target_include_directories(superior_port_conformance_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_port_conformance_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_port_conformance_tests)

add_executable(superior_speech_capability_tests speech_capability_tests.cpp)
target_link_libraries(superior_speech_capability_tests PRIVATE Superior::SpeechRecognition)
target_include_directories(superior_speech_capability_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_speech_capability_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_speech_capability_tests)

add_executable(superior_speech_step_tests speech_step_tests.cpp)
target_link_libraries(superior_speech_step_tests PRIVATE Superior::SpeechRecognition)
target_include_directories(superior_speech_step_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_speech_step_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_speech_step_tests)

# The speech panel's controller (P5.4): its capability, its clock and its layout
# are fakes, so the state machine is exercised with no microphone, no model, no
# child process and no Fcitx5. The target only exists where the adapter is built.
if(TARGET Superior::Fcitx5Speech)
    add_executable(superior_speech_controller_tests speech_controller_tests.cpp)
    target_link_libraries(superior_speech_controller_tests PRIVATE
        Superior::Fcitx5Speech
        Superior::PlatformStub)
    target_include_directories(superior_speech_controller_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(superior_speech_controller_tests PRIVATE cxx_std_20)
    superior_set_warnings(superior_speech_controller_tests)
    add_test(NAME speech-controller COMMAND superior_speech_controller_tests)
endif()

add_executable(superior_speech_state_tests speech_state_tests.cpp)
target_link_libraries(superior_speech_state_tests PRIVATE Superior::Core)
target_include_directories(superior_speech_state_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_speech_state_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_speech_state_tests)

add_executable(superior_protocol_tests protocol_tests.cpp)
target_link_libraries(superior_protocol_tests PRIVATE Superior::Protocol)
target_include_directories(superior_protocol_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_protocol_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_protocol_tests)

add_executable(superior_socket_tests socket_tests.cpp)
target_link_libraries(superior_socket_tests PRIVATE Superior::Protocol)
target_include_directories(superior_socket_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(superior_socket_tests PRIVATE cxx_std_20)
superior_set_warnings(superior_socket_tests)

add_test(NAME diagnostics COMMAND superior_diagnostics_tests)
add_test(NAME skin COMMAND superior_skin_tests)
add_test(NAME inline-text COMMAND superior_inline_text_tests)
add_test(NAME shift-tap COMMAND superior_shift_tap_tests)
add_test(NAME key-translation COMMAND superior_key_translation_tests)
add_test(NAME qt-platform COMMAND superior_qt_platform_tests)
add_test(NAME layout-rules COMMAND superior_layout_rules_tests)
add_test(NAME drag-handle COMMAND superior_drag_handle_tests)
add_test(NAME overlay-layout COMMAND superior_overlay_layout_tests)
add_test(NAME candidate-label COMMAND superior_candidate_label_tests)
add_test(NAME wheel-step COMMAND superior_wheel_step_tests)
add_test(NAME preedit-layout COMMAND superior_preedit_layout_tests)
add_test(NAME status-bar-state COMMAND superior_status_bar_state_tests)
add_test(NAME status-mapping COMMAND superior_status_mapping_tests)
add_test(NAME learning COMMAND superior_learning_tests)
add_test(NAME pinyin-engine COMMAND superior_pinyin_engine_tests)
add_test(NAME cloud COMMAND superior_cloud_tests)
add_test(NAME config COMMAND superior_config_tests)
add_test(NAME inputmethod COMMAND superior_inputmethod_tests)
add_test(NAME theme COMMAND superior_theme_tests)
add_test(NAME core COMMAND superior_core_tests)
add_test(NAME modes COMMAND superior_modes_tests)
add_test(NAME key-policy COMMAND superior_key_policy_tests)
add_test(NAME text-outbox COMMAND superior_text_outbox_tests)
add_test(NAME paths COMMAND superior_paths_tests)
add_test(NAME scene-input COMMAND superior_scene_input_tests)
add_test(NAME pinyin-layout COMMAND superior_pinyin_layout_tests)
add_test(NAME golden-pinyin
    COMMAND superior_golden_pinyin_tests "${PROJECT_SOURCE_DIR}/tests/golden/pinyin.txt")
add_test(NAME port-conformance COMMAND superior_port_conformance_tests)
add_test(NAME audio-capture COMMAND superior_audio_capture_tests)
add_test(NAME speech-capability COMMAND superior_speech_capability_tests)
add_test(NAME speech-state COMMAND superior_speech_state_tests)
add_test(NAME speech-step COMMAND superior_speech_step_tests)

if(TARGET superior_speech_recognition_process)
    add_test(NAME speech-process
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/speech_process_test.sh"
                $<TARGET_FILE:superior_speech_recognition_process>)
endif()
add_test(NAME protocol COMMAND superior_protocol_tests)
add_test(NAME socket COMMAND superior_socket_tests)

add_test(
    NAME boundaries
    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/check-boundaries.sh" "${PROJECT_SOURCE_DIR}")

# The schema archive is committed, so the pinned checksum and the bytes in the
# tree can drift apart. This keeps "vendored" from becoming a second truth.
add_test(
    NAME bundled-schema
    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/check-bundled-schema.sh")

if(TARGET skin_lint)
    add_test(NAME skin-lint-default
        COMMAND skin_lint "${PROJECT_SOURCE_DIR}/skins/default" --strict)
endif()
