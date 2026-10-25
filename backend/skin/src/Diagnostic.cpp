// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/skin/Diagnostic.hpp"

#include <string>

namespace superior::skin {

std::string_view toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "error";
}

std::string format(const Diagnostic &diagnostic) {
    std::string out;
    out.reserve(diagnostic.pointer.size() + diagnostic.code.size() +
                diagnostic.message.size() + 24);

    out.append(toString(diagnostic.severity));
    if (!diagnostic.code.empty()) {
        out.push_back('[');
        out.append(diagnostic.code);
        out.push_back(']');
    }
    if (!diagnostic.pointer.empty()) {
        out.push_back(' ');
        out.append(diagnostic.pointer);
    }
    out.append(": ");
    out.append(diagnostic.message);
    return out;
}

} // namespace superior::skin
