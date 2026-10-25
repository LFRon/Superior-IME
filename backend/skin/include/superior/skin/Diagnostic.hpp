// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace superior::skin {

enum class Severity {
    Info,
    Warning,
    Error,
};

struct Diagnostic {
    Severity severity = Severity::Error;

    /// RFC 6901 JSON Pointer of the offending node. Empty means "the package
    /// as a whole".
    std::string pointer;

    /// Stable, documentation-friendly error code. Never localized.
    std::string code;

    /// Human-readable detail. English by convention; the UI may localize by
    /// code instead.
    std::string message;
};

namespace codes {

// Package / file level.
inline constexpr std::string_view FileMissing = "file.missing";
inline constexpr std::string_view FileRead = "file.read";
inline constexpr std::string_view JsonParse = "json.parse";
inline constexpr std::string_view NotADirectory = "package.not-directory";

// Schema and required fields.
inline constexpr std::string_view SchemaMissing = "schema.missing";
inline constexpr std::string_view SchemaUnsupported = "schema.unsupported";
inline constexpr std::string_view FieldMissing = "field.missing";
inline constexpr std::string_view FieldType = "field.type";
inline constexpr std::string_view FieldValue = "field.value";
inline constexpr std::string_view FieldUnknown = "field.unknown";

// Tokens.
inline constexpr std::string_view TokenUnknown = "token.unknown";
inline constexpr std::string_view TokenCycle = "token.cycle";
inline constexpr std::string_view TokenType = "token.type";
inline constexpr std::string_view TokenInvalid = "token.invalid";

// Values.
inline constexpr std::string_view ColorInvalid = "color.invalid";
inline constexpr std::string_view NumberRange = "number.range";
inline constexpr std::string_view EnumInvalid = "enum.invalid";

// Assets.
inline constexpr std::string_view AssetPathInvalid = "asset.path.invalid";
inline constexpr std::string_view AssetMissing = "asset.missing";
inline constexpr std::string_view AssetExtension = "asset.extension";

// Cross references.
inline constexpr std::string_view VariantStyleUnknown = "variant.style.unknown";
inline constexpr std::string_view StyleMissing = "panel.style.missing";

// Motion.
inline constexpr std::string_view MotionTypeInvalid = "motion.type.invalid";

} // namespace codes

[[nodiscard]] std::string_view toString(Severity severity) noexcept;

/// "error[color.invalid] /tokens/color.text: #GGGGGG is not a colour"
[[nodiscard]] std::string format(const Diagnostic &diagnostic);

} // namespace superior::skin
