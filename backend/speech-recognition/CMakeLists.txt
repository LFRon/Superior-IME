# The speech-recognition capability: microphone capture plus one external
# recogniser process. No Qt, no Fcitx5, no model runtime linked in.
find_package(nlohmann_json 3.11 REQUIRED)

add_library(superior_speech_recognition STATIC
    src/Capture.cpp
    src/Engine.cpp
    src/Layout.cpp
    src/Protocol.cpp
    src/Service.cpp)

add_library(Superior::SpeechRecognition ALIAS superior_speech_recognition)

target_include_directories(superior_speech_recognition PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/superior-speech-recognition>)

target_link_libraries(superior_speech_recognition
    PRIVATE Superior::Diagnostics
    PRIVATE nlohmann_json::nlohmann_json)

target_compile_features(superior_speech_recognition PUBLIC cxx_std_20)
superior_set_warnings(superior_speech_recognition)

if(UNIX)
    add_executable(superior_speech_recognition_process main.cpp)
    set_target_properties(superior_speech_recognition_process PROPERTIES
        OUTPUT_NAME superior_speech_recognition)
    target_link_libraries(superior_speech_recognition_process
        PRIVATE Superior::SpeechRecognition)
    target_compile_features(superior_speech_recognition_process PRIVATE cxx_std_20)
    superior_set_warnings(superior_speech_recognition_process)

    install(TARGETS superior_speech_recognition_process
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
