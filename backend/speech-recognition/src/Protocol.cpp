// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Protocol.hpp"

#include <nlohmann/json.hpp>

namespace superior::speech_recognition {

namespace {

using Json = nlohmann::json;

const char *kindName(Command::Kind kind) {
    switch (kind) {
    case Command::Kind::Configure:
        return "configure";
    case Command::Kind::Start:
        return "start";
    case Command::Kind::Stop:
        return "stop";
    case Command::Kind::Cancel:
        return "cancel";
    case Command::Kind::Quit:
        return "quit";
    }
    return "quit";
}

Command::Kind commandKindFrom(std::string_view name) {
    if (name == "configure") {
        return Command::Kind::Configure;
    }
    if (name == "start") {
        return Command::Kind::Start;
    }
    if (name == "stop") {
        return Command::Kind::Stop;
    }
    if (name == "cancel") {
        return Command::Kind::Cancel;
    }
    return Command::Kind::Quit;
}

} // namespace

std::string encodeEvent(const Event &event) {
    Json json;
    switch (event.kind) {
    case Event::Kind::Ready:
        json = Json{{"ev", "ready"}};
        break;
    case Event::Kind::Phase:
        json = Json{{"ev", "phase"}, {"name", event.phase}};
        break;
    case Event::Kind::Level:
        json = Json{{"ev", "level"}, {"rms", event.level}};
        break;
    case Event::Kind::Partial:
        json = Json{{"ev", "partial"}, {"text", event.text}};
        break;
    case Event::Kind::Chunk:
        json = Json{{"ev", "chunk"}, {"text", event.text}};
        break;
    case Event::Kind::Final:
        json = Json{{"ev", "final"}, {"text", event.text}};
        break;
    case Event::Kind::Error:
        json = Json{{"ev", "error"}, {"code", event.code}, {"message", event.message}};
        break;
    }
    return json.dump();
}

std::string encodeCommand(const Command &command) {
    Json json{{"cmd", kindName(command.kind)}};
    if (command.kind == Command::Kind::Configure) {
        json["config"] = {{"engine", command.config.enginePath},
                          {"model", command.config.modelPath},
                          {"language", command.config.language},
                          {"threads", command.config.threads}};
    }
    return json.dump();
}

std::optional<Command> decodeCommand(std::string_view line) {
    if (line.empty()) {
        return std::nullopt;
    }
    Json json;
    try {
        json = Json::parse(line);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    if (!json.is_object()) {
        return std::nullopt;
    }

    Command command;
    command.kind = commandKindFrom(json.value("cmd", std::string{}));
    if (json.contains("config") && json.at("config").is_object()) {
        const auto &config = json.at("config");
        command.config.enginePath = config.value("engine", std::string{});
        command.config.modelPath = config.value("model", std::string{});
        command.config.language = config.value("language", std::string{"zh"});
        command.config.threads = config.value("threads", 4);
    }
    return command;
}

} // namespace superior::speech_recognition
