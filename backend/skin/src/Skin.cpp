// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/skin/Skin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "superior/diagnostics/Log.hpp"

#include "Canonical.hpp"

namespace superior::skin {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kSkinFileName = "skin.json";

constexpr std::array<std::string_view, 7> kAssetExtensions{
    ".png", ".svg", ".webp", ".gif", ".jpg", ".jpeg", ".avif"};

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::optional<std::uint8_t> hexByte(char high, char low) {
    const int hi = hexDigit(high);
    const int lo = hexDigit(low);
    if (hi < 0 || lo < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(hi * 16 + lo);
}

std::optional<Color> parseColor(std::string_view text) {
    if (text.size() < 4 || text.front() != '#') {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(1);
    if (digits.size() == 3) {
        const int r = hexDigit(digits[0]);
        const int g = hexDigit(digits[1]);
        const int b = hexDigit(digits[2]);
        if (r < 0 || g < 0 || b < 0) {
            return std::nullopt;
        }
        return Color{static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17),
                     static_cast<std::uint8_t>(b * 17), 255};
    }
    if (digits.size() != 6 && digits.size() != 8) {
        return std::nullopt;
    }
    Color color;
    const auto r = hexByte(digits[0], digits[1]);
    const auto g = hexByte(digits[2], digits[3]);
    const auto b = hexByte(digits[4], digits[5]);
    if (!r || !g || !b) {
        return std::nullopt;
    }
    color.r = *r;
    color.g = *g;
    color.b = *b;
    if (digits.size() == 8) {
        const auto a = hexByte(digits[6], digits[7]);
        if (!a) {
            return std::nullopt;
        }
        color.a = *a;
    }
    return color;
}

bool isSafeRelativePath(std::string_view raw) {
    if (raw.empty() || raw.front() == '/') {
        return false;
    }
    if (raw.find('\\') != std::string_view::npos) {
        return false;
    }
    for (char c : raw) {
        if (c == '\0') {
            return false;
        }
    }
    const std::filesystem::path path{std::string(raw)};
    if (path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto &part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

bool isSupportedAsset(const std::filesystem::path &path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(kAssetExtensions.begin(), kAssetExtensions.end(), extension) !=
           kAssetExtensions.end();
}

std::string escapePointerToken(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (char c : token) {
        if (c == '~') {
            out.append("~0");
        } else if (c == '/') {
            out.append("~1");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string joinPointer(std::string_view base, std::string_view key) {
    std::string out(base);
    out.push_back('/');
    out.append(escapePointerToken(key));
    return out;
}

std::string indexPointer(std::string_view base, std::size_t index) {
    std::string out(base);
    out.push_back('/');
    out.append(std::to_string(index));
    return out;
}

bool isValidId(std::string_view id) {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (!alnum(id.front())) {
        return false;
    }
    for (char c : id) {
        if (!alnum(c) && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

bool isValidVersion(std::string_view version) {
    int parts = 0;
    std::size_t start = 0;
    while (start < version.size()) {
        const std::size_t dot = version.find('.', start);
        const std::string_view part =
            dot == std::string_view::npos ? version.substr(start) : version.substr(start, dot - start);
        if (part.empty() ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return parts == 3;
}

class Parser {
public:
    Parser(std::filesystem::path root, const Json &document, LoadOptions options)
        : root_(std::move(root)), document_(document), options_(options) {}

    LoadResult run() {
        LoadResult result;

        if (!document_.is_object()) {
            emit(Severity::Error, "", codes::FieldType, "skin.json must contain a JSON object");
            result.diagnostics = std::move(diagnostics_);
            return result;
        }

        checkUnknownKeys(document_,
                         {"schema", "id", "name", "version", "author", "license", "description",
                          "preview", "modes", "defaultMode", "tokens", "panel", "status", "voice",
                          "motion"},
                         "");

        SkinSpec spec;
        spec.schema = std::string(kSchemaV1);

        const auto schema = stringField(document_, "schema", "");
        if (!schema) {
            emit(Severity::Error, "/schema", codes::SchemaMissing,
                 "missing required 'schema' (expected '" + spec.schema + "')");
        } else if (*schema != kSchemaV1) {
            emit(Severity::Error, "/schema", codes::SchemaUnsupported,
                 "unsupported schema '" + *schema + "', expected '" + spec.schema + "'");
        }

        if (auto value = stringField(document_, "id", "")) {
            spec.id = *value;
            if (!isValidId(spec.id)) {
                emit(Severity::Error, "/id", codes::FieldValue,
                     "id must start with a letter or digit and contain only [A-Za-z0-9._-]");
            }
        } else {
            emit(Severity::Error, "/id", codes::FieldMissing, "missing required 'id'");
        }

        if (auto value = stringField(document_, "name", "")) {
            spec.name = *value;
        } else {
            emit(Severity::Error, "/name", codes::FieldMissing, "missing required 'name'");
        }

        if (auto value = stringField(document_, "version", "")) {
            spec.version = *value;
            if (!isValidVersion(spec.version)) {
                emit(Severity::Error, "/version", codes::FieldValue,
                     "version must be three dot-separated integers, for example 1.0.0");
            }
        } else {
            emit(Severity::Error, "/version", codes::FieldMissing, "missing required 'version'");
        }

        spec.author = stringField(document_, "author", "").value_or("");
        spec.license = stringField(document_, "license", "").value_or("");
        spec.description = stringField(document_, "description", "").value_or("");

        if (auto preview = stringField(document_, "preview", "")) {
            spec.preview = *preview;
            noteAsset(spec.preview, "/preview");
        }

        parseModes(spec);
        parseTokens();
        parsePanel(spec);
        parseStatus(spec);
        parseVoice(spec);
        parseMotion(spec);

        spec.assets = collectAssets();
        if (!hasErrors()) {
            spec.hash = detail::computeSpecHash(spec);
            result.spec = std::move(spec);
        }
        result.diagnostics = std::move(diagnostics_);
        return result;
    }

private:
    // ---------------------------------------------------------------- emitting

    void emit(Severity severity, std::string pointer, std::string_view code, std::string message) {
        if (options_.strict && severity == Severity::Warning) {
            severity = Severity::Error;
        }
        diagnostics_.push_back(Diagnostic{severity, std::move(pointer), std::string(code),
                                          std::move(message)});
    }

    void error(std::string pointer, std::string_view code, std::string message) {
        emit(Severity::Error, std::move(pointer), code, std::move(message));
    }

    void warning(std::string pointer, std::string_view code, std::string message) {
        emit(Severity::Warning, std::move(pointer), code, std::move(message));
    }

    [[nodiscard]] bool hasErrors() const {
        return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                           [](const Diagnostic &d) { return d.severity == Severity::Error; });
    }

    // ------------------------------------------------------------------ tokens

    void parseTokens() {
        const auto it = document_.find("tokens");
        if (it == document_.end()) {
            return;
        }
        if (!it->is_object()) {
            error("/tokens", codes::FieldType, "'tokens' must be an object");
            return;
        }
        tokens_ = &*it;
    }

    std::optional<Json> resolveTokenImpl(std::string_view name, const std::string &pointer,
                                         std::vector<std::string> &stack) {
        if (tokens_ == nullptr) {
            error(pointer, codes::TokenUnknown,
                  "token '$" + std::string(name) + "' used but 'tokens' is missing");
            return std::nullopt;
        }
        const std::string key(name);
        const auto it = tokens_->find(key);
        if (it == tokens_->end()) {
            error(pointer, codes::TokenUnknown, "unknown token '$" + key + "'");
            return std::nullopt;
        }
        if (std::find(stack.begin(), stack.end(), key) != stack.end()) {
            error(pointer, codes::TokenCycle, "token cycle involving '$" + key + "'");
            return std::nullopt;
        }
        stack.push_back(key);
        std::optional<Json> result;
        if (it->is_string() && it->get_ref<const std::string &>().starts_with('$')) {
            const std::string &text = it->get_ref<const std::string &>();
            result = resolveTokenImpl(std::string_view(text).substr(1), pointer, stack);
        } else {
            result = *it;
        }
        stack.pop_back();
        return result;
    }

    std::optional<Json> resolveNode(const Json &node, const std::string &pointer) {
        if (!node.is_string()) {
            return node;
        }
        const std::string &text = node.get_ref<const std::string &>();
        if (!text.starts_with('$')) {
            return node;
        }
        std::vector<std::string> stack;
        return resolveTokenImpl(std::string_view(text).substr(1), pointer, stack);
    }

    // ------------------------------------------------------------- primitives

    static const Json *member(const Json &object, std::string_view key) {
        const auto it = object.find(std::string(key));
        return it == object.end() ? nullptr : &*it;
    }

    void checkUnknownKeys(const Json &object, std::initializer_list<std::string_view> known,
                          const std::string &base) {
        if (!object.is_object()) {
            return;
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            const std::string_view key = it.key();
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                warning(joinPointer(base, key), codes::FieldUnknown,
                        "unknown key '" + std::string(key) + "' ignored");
            }
        }
    }

    void checkUnknownKeys(const Json &object, const std::vector<std::string_view> &known,
                          const std::string &base) {
        if (!object.is_object()) {
            return;
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            const std::string_view key = it.key();
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                warning(joinPointer(base, key), codes::FieldUnknown,
                        "unknown key '" + std::string(key) + "' ignored");
            }
        }
    }

    std::optional<double> numberField(const Json &object, std::string_view key,
                                      const std::string &base, double minValue, double maxValue) {
        const Json *node = member(object, key);
        if (node == nullptr) {
            return std::nullopt;
        }
        const std::string pointer = joinPointer(base, key);
        const auto resolved = resolveNode(*node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        if (!resolved->is_number()) {
            error(pointer, codes::FieldType, "expected a number");
            return std::nullopt;
        }
        const double value = resolved->get<double>();
        if (!(value >= minValue && value <= maxValue)) {
            error(pointer, codes::NumberRange,
                  "expected a number in [" + std::to_string(minValue) + ", " +
                      std::to_string(maxValue) + "]");
            return std::nullopt;
        }
        return value;
    }

    std::optional<int> intField(const Json &object, std::string_view key, const std::string &base,
                                int minValue, int maxValue) {
        const auto value = numberField(object, key, base, static_cast<double>(minValue),
                                       static_cast<double>(maxValue));
        if (!value) {
            return std::nullopt;
        }
        if (std::floor(*value) != *value) {
            error(joinPointer(base, key), codes::FieldType, "expected an integer");
            return std::nullopt;
        }
        return static_cast<int>(*value);
    }

    std::optional<bool> boolField(const Json &object, std::string_view key,
                                  const std::string &base) {
        const Json *node = member(object, key);
        if (node == nullptr) {
            return std::nullopt;
        }
        if (!node->is_boolean()) {
            error(joinPointer(base, key), codes::FieldType, "expected true or false");
            return std::nullopt;
        }
        return node->get<bool>();
    }

    std::optional<std::string> stringField(const Json &object, std::string_view key,
                                           const std::string &base) {
        const Json *node = member(object, key);
        if (node == nullptr) {
            return std::nullopt;
        }
        const std::string pointer = joinPointer(base, key);
        const auto resolved = resolveNode(*node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        if (!resolved->is_string()) {
            error(pointer, codes::FieldType, "expected a string");
            return std::nullopt;
        }
        return resolved->get<std::string>();
    }

    std::optional<Color> colorValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        if (!resolved->is_string()) {
            error(pointer, codes::FieldType, "expected a colour string");
            return std::nullopt;
        }
        const std::string &text = resolved->get_ref<const std::string &>();
        const auto color = parseColor(text);
        if (!color) {
            error(pointer, codes::ColorInvalid,
                  "'" + text + "' is not #RGB, #RRGGBB or #RRGGBBAA");
        }
        return color;
    }

    std::optional<Color> colorField(const Json &object, std::string_view key,
                                    const std::string &base) {
        const Json *node = member(object, key);
        if (node == nullptr) {
            return std::nullopt;
        }
        return colorValue(*node, joinPointer(base, key));
    }

    std::optional<Font> fontValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected a font object");
            return std::nullopt;
        }
        checkUnknownKeys(*resolved, {"family", "size", "weight", "italic"}, pointer);
        Font font;
        if (auto family = stringField(*resolved, "family", pointer)) {
            font.family = *family;
        }
        if (auto size = numberField(*resolved, "size", pointer, 1.0, 512.0)) {
            font.size = *size;
        }
        if (auto weight = intField(*resolved, "weight", pointer, 1, 1000)) {
            font.weight = *weight;
        }
        if (auto italic = boolField(*resolved, "italic", pointer)) {
            font.italic = *italic;
        }
        return font;
    }

    std::optional<Insets> insetsValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        Insets insets;
        if (resolved->is_array()) {
            std::vector<double> values;
            values.reserve(resolved->size());
            for (std::size_t i = 0; i < resolved->size(); ++i) {
                const auto value = (*resolved)[i];
                if (!value.is_number()) {
                    error(indexPointer(pointer, i), codes::FieldType, "expected a number");
                    return std::nullopt;
                }
                values.push_back(value.get<double>());
            }
            if (values.size() == 1) {
                insets = Insets{values[0], values[0], values[0], values[0]};
            } else if (values.size() == 2) {
                insets = Insets{values[0], values[1], values[0], values[1]};
            } else if (values.size() == 4) {
                insets = Insets{values[0], values[1], values[2], values[3]};
            } else {
                error(pointer, codes::FieldValue,
                      "expected 1, 2 or 4 numbers (all | vertical,horizontal | top,right,bottom,left)");
                return std::nullopt;
            }
        } else if (resolved->is_object()) {
            checkUnknownKeys(*resolved, {"top", "right", "bottom", "left"}, pointer);
            insets.top = numberField(*resolved, "top", pointer, 0.0, 4096.0).value_or(0.0);
            insets.right = numberField(*resolved, "right", pointer, 0.0, 4096.0).value_or(0.0);
            insets.bottom = numberField(*resolved, "bottom", pointer, 0.0, 4096.0).value_or(0.0);
            insets.left = numberField(*resolved, "left", pointer, 0.0, 4096.0).value_or(0.0);
        } else {
            error(pointer, codes::FieldType, "expected an array or an object");
            return std::nullopt;
        }
        for (double value : {insets.top, insets.right, insets.bottom, insets.left}) {
            if (value < 0.0 || value > 4096.0) {
                error(pointer, codes::NumberRange, "insets must be within [0, 4096]");
                return std::nullopt;
            }
        }
        return insets;
    }

    std::optional<Slice> sliceValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        Slice slice;
        if (resolved->is_array() && resolved->size() == 4) {
            const auto read = [&](std::size_t index) -> std::optional<int> {
                const auto &value = (*resolved)[index];
                if (!value.is_number_integer()) {
                    error(indexPointer(pointer, index), codes::FieldType, "expected an integer");
                    return std::nullopt;
                }
                const int parsed = value.get<int>();
                if (parsed < 0 || parsed > 8192) {
                    error(indexPointer(pointer, index), codes::NumberRange,
                          "slice must be within [0, 8192]");
                    return std::nullopt;
                }
                return parsed;
            };
            const auto left = read(0);
            const auto top = read(1);
            const auto right = read(2);
            const auto bottom = read(3);
            if (!left || !top || !right || !bottom) {
                return std::nullopt;
            }
            slice = Slice{*left, *top, *right, *bottom};
        } else if (resolved->is_object()) {
            checkUnknownKeys(*resolved, {"left", "top", "right", "bottom"}, pointer);
            slice.left = intField(*resolved, "left", pointer, 0, 8192).value_or(0);
            slice.top = intField(*resolved, "top", pointer, 0, 8192).value_or(0);
            slice.right = intField(*resolved, "right", pointer, 0, 8192).value_or(0);
            slice.bottom = intField(*resolved, "bottom", pointer, 0, 8192).value_or(0);
        } else {
            error(pointer, codes::FieldType, "expected [left,top,right,bottom] or an object");
            return std::nullopt;
        }
        return slice;
    }

    std::optional<ImageBrush> imageBrushValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        ImageBrush brush;
        if (resolved->is_string()) {
            brush.image = resolved->get<std::string>();
            noteAsset(brush.image, pointer);
            return brush;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected an asset path or an image object");
            return std::nullopt;
        }
        checkUnknownKeys(*resolved, {"image", "slice", "fit"}, pointer);
        const auto image = stringField(*resolved, "image", pointer);
        if (!image) {
            error(pointer, codes::FieldMissing, "image object requires 'image'");
            return std::nullopt;
        }
        brush.image = *image;
        noteAsset(brush.image, joinPointer(pointer, "image"));

        if (const Json *slice = member(*resolved, "slice")) {
            if (auto parsed = sliceValue(*slice, joinPointer(pointer, "slice"))) {
                brush.slice = *parsed;
            }
        }
        if (const auto fit = stringField(*resolved, "fit", pointer)) {
            if (*fit == "stretch") {
                brush.fit = ImageFit::Stretch;
            } else if (*fit == "tile") {
                brush.fit = ImageFit::Tile;
            } else if (*fit == "contain") {
                brush.fit = ImageFit::Contain;
            } else if (*fit == "cover") {
                brush.fit = ImageFit::Cover;
            } else {
                error(joinPointer(pointer, "fit"), codes::EnumInvalid,
                      "expected stretch | tile | contain | cover");
            }
        }
        return brush;
    }

    std::optional<Brush> brushValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        if (resolved->is_string()) {
            Brush brush;
            brush.color = colorValue(*resolved, pointer);
            return brush;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected a colour, an asset path or a brush object");
            return std::nullopt;
        }
        checkUnknownKeys(*resolved, {"color", "image", "radius"}, pointer);
        Brush brush;
        if (const Json *color = member(*resolved, "color")) {
            brush.color = colorValue(*color, joinPointer(pointer, "color"));
        }
        if (const Json *image = member(*resolved, "image")) {
            brush.image = imageBrushValue(*image, joinPointer(pointer, "image"));
        }
        if (auto radius = numberField(*resolved, "radius", pointer, 0.0, 4096.0)) {
            brush.radius = *radius;
        }
        if (!brush.color && !brush.image) {
            warning(pointer, codes::FieldValue, "brush has neither colour nor image");
        }
        return brush;
    }

    std::optional<Brush> brushField(const Json &object, std::string_view key,
                                    const std::string &base) {
        const Json *node = member(object, key);
        if (node == nullptr) {
            return std::nullopt;
        }
        return brushValue(*node, joinPointer(base, key));
    }

    std::optional<Shadow> shadowValue(const Json &node, const std::string &pointer) {
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return std::nullopt;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected a shadow object");
            return std::nullopt;
        }
        checkUnknownKeys(*resolved, {"color", "blur", "offset"}, pointer);
        Shadow shadow;
        if (const Json *color = member(*resolved, "color")) {
            shadow.color = colorValue(*color, joinPointer(pointer, "color"));
        }
        if (auto blur = numberField(*resolved, "blur", pointer, 0.0, 512.0)) {
            shadow.blur = *blur;
        }
        if (const Json *offset = member(*resolved, "offset")) {
            if (!offset->is_array() || offset->size() != 2) {
                error(joinPointer(pointer, "offset"), codes::FieldType,
                      "expected [x, y]");
            } else {
                for (std::size_t i = 0; i < 2; ++i) {
                    if (!(*offset)[i].is_number()) {
                        error(indexPointer(joinPointer(pointer, "offset"), i), codes::FieldType,
                              "expected a number");
                        continue;
                    }
                    const double value = (*offset)[i].get<double>();
                    if (i == 0) {
                        shadow.offsetX = value;
                    } else {
                        shadow.offsetY = value;
                    }
                }
            }
        }
        return shadow;
    }

    TextSpec textSpecValue(const Json &node, const std::string &pointer,
                           std::initializer_list<std::string_view> extraColorKeys = {}) {
        TextSpec text;
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return text;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected a text object");
            return text;
        }
        std::vector<std::string_view> known{"font", "color"};
        known.insert(known.end(), extraColorKeys.begin(), extraColorKeys.end());
        checkUnknownKeys(*resolved, known, pointer);
        if (const Json *font = member(*resolved, "font")) {
            text.font = fontValue(*font, joinPointer(pointer, "font"));
        }
        if (const Json *color = member(*resolved, "color")) {
            text.color = colorValue(*color, joinPointer(pointer, "color"));
        }
        return text;
    }

    CandidateStyle candidateStyleValue(const Json &node, const std::string &pointer) {
        CandidateStyle candidates;
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return candidates;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected a candidate object");
            return candidates;
        }
        checkUnknownKeys(*resolved, {"flow", "maxVisible", "cell", "text", "selected", "hover",
                                     "paging"},
                         pointer);

        if (const auto flow = stringField(*resolved, "flow", pointer)) {
            if (*flow == "horizontal") {
                candidates.flow = CandidateFlow::Horizontal;
            } else if (*flow == "vertical") {
                candidates.flow = CandidateFlow::Vertical;
            } else {
                error(joinPointer(pointer, "flow"), codes::EnumInvalid,
                      "expected horizontal | vertical");
            }
        }
        if (auto visible = intField(*resolved, "maxVisible", pointer, 1, 32)) {
            candidates.maxVisible = *visible;
        }
        if (const Json *cell = member(*resolved, "cell")) {
            if (cell->is_object()) {
                const std::string cellPointer = joinPointer(pointer, "cell");
                checkUnknownKeys(*cell, {"padding", "gap"}, cellPointer);
                if (const Json *padding = member(*cell, "padding")) {
                    if (auto parsed = insetsValue(*padding, joinPointer(cellPointer, "padding"))) {
                        candidates.cell.padding = *parsed;
                    }
                }
                if (auto gap = numberField(*cell, "gap", cellPointer, 0.0, 256.0)) {
                    candidates.cell.gap = *gap;
                }
            } else {
                error(joinPointer(pointer, "cell"), codes::FieldType, "expected an object");
            }
        }
        if (const Json *text = member(*resolved, "text")) {
            const std::string textPointer = joinPointer(pointer, "text");
            const auto parsed = textSpecValue(*text, textPointer, {"firstColor", "commentColor"});
            candidates.text.font = parsed.font;
            candidates.text.color = parsed.color;
            if (const auto first = colorField(*text, "firstColor", textPointer)) {
                candidates.text.firstColor = *first;
            }
            if (const auto comment = colorField(*text, "commentColor", textPointer)) {
                candidates.text.commentColor = *comment;
            }
        }
        if (auto selected = brushField(*resolved, "selected", pointer)) {
            candidates.selected = *selected;
        }
        if (auto hover = brushField(*resolved, "hover", pointer)) {
            candidates.hover = *hover;
        }
        if (const auto paging = stringField(*resolved, "paging", pointer)) {
            if (*paging == "none") {
                candidates.paging = PagingIndicator::None;
            } else if (*paging == "dots") {
                candidates.paging = PagingIndicator::Dots;
            } else if (*paging == "arrows") {
                candidates.paging = PagingIndicator::Arrows;
            } else {
                error(joinPointer(pointer, "paging"), codes::EnumInvalid,
                      "expected none | dots | arrows");
            }
        }
        return candidates;
    }

    PanelStyle panelStyleValue(const Json &node, const std::string &pointer) {
        PanelStyle style;
        const auto resolved = resolveNode(node, pointer);
        if (!resolved) {
            return style;
        }
        if (!resolved->is_object()) {
            error(pointer, codes::FieldType, "expected a panel style object");
            return style;
        }
        checkUnknownKeys(*resolved,
                         {"background", "backgroundColor", "padding", "gap", "preedit",
                          "candidates", "separator", "shadow"},
                         pointer);

        if (auto background = brushField(*resolved, "background", pointer)) {
            style.background = *background;
        }
        if (const auto color = colorField(*resolved, "backgroundColor", pointer)) {
            style.background.color = *color;
        }
        if (const Json *padding = member(*resolved, "padding")) {
            if (auto parsed = insetsValue(*padding, joinPointer(pointer, "padding"))) {
                style.padding = *parsed;
            }
        }
        if (auto gap = numberField(*resolved, "gap", pointer, 0.0, 256.0)) {
            style.gap = *gap;
        }
        if (const Json *preedit = member(*resolved, "preedit")) {
            style.preedit = textSpecValue(*preedit, joinPointer(pointer, "preedit"));
        }
        if (const Json *candidates = member(*resolved, "candidates")) {
            style.candidates =
                candidateStyleValue(*candidates, joinPointer(pointer, "candidates"));
        }
        if (const Json *separator = member(*resolved, "separator")) {
            const std::string separatorPointer = joinPointer(pointer, "separator");
            if (!separator->is_object()) {
                error(separatorPointer, codes::FieldType, "expected an object");
            } else {
                checkUnknownKeys(*separator, {"enabled", "color", "thickness"}, separatorPointer);
                style.separator.enabled =
                    boolField(*separator, "enabled", separatorPointer).value_or(true);
                if (const auto color = colorField(*separator, "color", separatorPointer)) {
                    style.separator.color = *color;
                }
                if (auto thickness = numberField(*separator, "thickness", separatorPointer, 0.0, 32.0)) {
                    style.separator.thickness = *thickness;
                }
            }
        }
        if (const Json *shadow = member(*resolved, "shadow")) {
            if (auto parsed = shadowValue(*shadow, joinPointer(pointer, "shadow"))) {
                style.shadow = *parsed;
            }
        }
        return style;
    }

    void parseModes(SkinSpec &spec) {
        const Json *modes = member(document_, "modes");
        if (modes != nullptr) {
            if (!modes->is_array()) {
                error("/modes", codes::FieldType, "'modes' must be an array of strings");
            } else {
                for (std::size_t i = 0; i < modes->size(); ++i) {
                    const auto &entry = (*modes)[i];
                    if (!entry.is_string() || entry.get<std::string>().empty()) {
                        error(indexPointer("/modes", i), codes::FieldType,
                              "expected a non-empty string");
                        continue;
                    }
                    spec.modes.push_back(entry.get<std::string>());
                }
            }
        }
        if (auto defaultMode = stringField(document_, "defaultMode", "")) {
            spec.defaultMode = *defaultMode;
        }
        if (spec.modes.empty()) {
            if (!spec.defaultMode.empty()) {
                warning("/defaultMode", codes::FieldValue,
                        "defaultMode is set but 'modes' is empty");
            }
            return;
        }
        if (spec.defaultMode.empty()) {
            spec.defaultMode = spec.modes.front();
            return;
        }
        if (std::find(spec.modes.begin(), spec.modes.end(), spec.defaultMode) == spec.modes.end()) {
            error("/defaultMode", codes::FieldValue,
                  "defaultMode '" + spec.defaultMode + "' is not listed in 'modes'");
        }
    }

    void parsePanel(SkinSpec &spec) {
        const Json *panel = member(document_, "panel");
        if (panel == nullptr) {
            error("/panel", codes::StyleMissing, "missing required 'panel' object");
            return;
        }
        if (!panel->is_object()) {
            error("/panel", codes::FieldType, "'panel' must be an object");
            return;
        }
        checkUnknownKeys(*panel, {"variants", "styles"}, "/panel");

        if (const Json *styles = member(*panel, "styles")) {
            if (!styles->is_object()) {
                error("/panel/styles", codes::FieldType, "'styles' must be an object");
            } else {
                for (auto it = styles->begin(); it != styles->end(); ++it) {
                    if (!it->is_object()) {
                        error(joinPointer("/panel/styles", it.key()), codes::FieldType,
                              "expected a panel style object");
                        continue;
                    }
                    spec.panelStyles.emplace(
                        it.key(), panelStyleValue(*it, joinPointer("/panel/styles", it.key())));
                }
            }
        }
        if (spec.panelStyles.empty()) {
            error("/panel/styles", codes::StyleMissing, "at least one panel style is required");
        }

        if (const Json *variants = member(*panel, "variants")) {
            if (!variants->is_array()) {
                error("/panel/variants", codes::FieldType, "'variants' must be an array");
            } else {
                for (std::size_t i = 0; i < variants->size(); ++i) {
                    const std::string base = indexPointer("/panel/variants", i);
                    const auto &entry = (*variants)[i];
                    if (!entry.is_object()) {
                        error(base, codes::FieldType, "expected a variant object");
                        continue;
                    }
                    checkUnknownKeys(entry, {"when", "use"}, base);
                    PanelVariant variant;
                    if (const Json *when = member(entry, "when")) {
                        if (!when->is_object()) {
                            error(joinPointer(base, "when"), codes::FieldType,
                                  "expected an object");
                        } else {
                            checkUnknownKeys(*when, {"orientation", "layout"},
                                             joinPointer(base, "when"));
                            variant.orientation =
                                stringField(*when, "orientation", joinPointer(base, "when"));
                            variant.layout =
                                stringField(*when, "layout", joinPointer(base, "when"));
                        }
                    }
                    const auto use = stringField(entry, "use", base);
                    if (!use) {
                        error(base, codes::FieldMissing, "variant requires 'use'");
                        continue;
                    }
                    variant.use = *use;
                    if (!spec.panelStyles.contains(variant.use)) {
                        error(joinPointer(base, "use"), codes::VariantStyleUnknown,
                              "unknown panel style '" + variant.use + "'");
                    }
                    spec.panelVariants.push_back(std::move(variant));
                }
            }
        }
    }

    void parseStatus(SkinSpec &spec) {
        const Json *status = member(document_, "status");
        if (status == nullptr) {
            return;
        }
        if (!status->is_object()) {
            error("/status", codes::FieldType, "'status' must be an object");
            return;
        }
        checkUnknownKeys(*status, {"background", "orientation", "padding", "spacing", "items"},
                        "/status");

        if (auto background = brushField(*status, "background", "/status")) {
            spec.status.background = *background;
        }
        if (const auto orientation = stringField(*status, "orientation", "/status")) {
            if (*orientation == "horizontal") {
                spec.status.orientation = CandidateFlow::Horizontal;
            } else if (*orientation == "vertical") {
                spec.status.orientation = CandidateFlow::Vertical;
            } else {
                error("/status/orientation", codes::EnumInvalid, "expected horizontal | vertical");
            }
        }
        if (const Json *padding = member(*status, "padding")) {
            if (auto parsed = insetsValue(*padding, "/status/padding")) {
                spec.status.padding = *parsed;
            }
        }
        if (auto spacing = numberField(*status, "spacing", "/status", 0.0, 256.0)) {
            spec.status.spacing = *spacing;
        }

        const Json *items = member(*status, "items");
        if (items == nullptr) {
            return;
        }
        if (!items->is_array()) {
            error("/status/items", codes::FieldType, "'items' must be an array");
            return;
        }
        std::set<std::string> ids;
        for (std::size_t i = 0; i < items->size(); ++i) {
            const std::string base = indexPointer("/status/items", i);
            const auto &entry = (*items)[i];
            if (!entry.is_object()) {
                error(base, codes::FieldType, "expected a status item object");
                continue;
            }
            checkUnknownKeys(entry, {"id", "kind", "action", "size", "icons", "background"}, base);
            StatusItem item;
            const auto id = stringField(entry, "id", base);
            if (!id || id->empty()) {
                error(base, codes::FieldMissing, "status item requires a non-empty 'id'");
                continue;
            }
            item.id = *id;
            if (!ids.insert(item.id).second) {
                error(joinPointer(base, "id"), codes::FieldValue,
                      "duplicate status item id '" + item.id + "'");
            }
            if (const auto kind = stringField(entry, "kind", base)) {
                if (*kind == "toggle") {
                    item.kind = StatusItemKind::Toggle;
                } else if (*kind == "button") {
                    item.kind = StatusItemKind::Button;
                } else {
                    error(joinPointer(base, "kind"), codes::EnumInvalid,
                          "expected toggle | button");
                }
            }
            item.action = stringField(entry, "action", base).value_or("");
            if (const Json *size = member(entry, "size")) {
                if (!size->is_array() || size->size() != 2) {
                    error(joinPointer(base, "size"), codes::FieldType, "expected [width, height]");
                } else {
                    const Json &width = (*size)[0];
                    const Json &height = (*size)[1];
                    if (!width.is_number() || !height.is_number()) {
                        error(joinPointer(base, "size"), codes::FieldType,
                              "expected numeric [width, height]");
                    } else {
                        item.width = width.get<double>();
                        item.height = height.get<double>();
                    }
                }
            }
            if (const Json *icons = member(entry, "icons")) {
                if (!icons->is_object()) {
                    error(joinPointer(base, "icons"), codes::FieldType, "expected an object");
                } else {
                    for (auto icon = icons->begin(); icon != icons->end(); ++icon) {
                        if (!icon->is_string()) {
                            error(joinPointer(joinPointer(base, "icons"), icon.key()),
                                  codes::FieldType, "expected an asset path");
                            continue;
                        }
                        const std::string path = icon->get<std::string>();
                        const std::string pointer =
                            joinPointer(joinPointer(base, "icons"), icon.key());
                        noteAsset(path, pointer);
                        item.icons.emplace(icon.key(), path);
                    }
                }
            }
            if (auto background = brushField(entry, "background", base)) {
                item.background = *background;
            }
            spec.status.items.push_back(std::move(item));
        }
    }

    void parseVoice(SkinSpec &spec) {
        const Json *voice = member(document_, "voice");
        if (voice == nullptr) {
            return;
        }
        if (!voice->is_object()) {
            error("/voice", codes::FieldType, "'voice' must be an object");
            return;
        }
        checkUnknownKeys(*voice, {"background", "mic", "meter", "partialText"}, "/voice");

        if (auto background = brushField(*voice, "background", "/voice")) {
            spec.voice.background = *background;
        }
        if (const Json *mic = member(*voice, "mic")) {
            const std::string base = "/voice/mic";
            if (!mic->is_object()) {
                error(base, codes::FieldType, "expected an object");
            } else {
                checkUnknownKeys(*mic, {"idle", "active", "size"}, base);
                if (auto idle = stringField(*mic, "idle", base)) {
                    spec.voice.micIdle = *idle;
                    noteAsset(*idle, joinPointer(base, "idle"));
                }
                if (auto active = stringField(*mic, "active", base)) {
                    spec.voice.micActive = *active;
                    noteAsset(*active, joinPointer(base, "active"));
                }
                if (const Json *size = member(*mic, "size")) {
                    if (!size->is_array() || size->size() != 2) {
                        error(joinPointer(base, "size"), codes::FieldType,
                              "expected [width, height]");
                    } else if ((*size)[0].is_number() && (*size)[1].is_number()) {
                        spec.voice.micWidth = (*size)[0].get<double>();
                        spec.voice.micHeight = (*size)[1].get<double>();
                    } else {
                        error(joinPointer(base, "size"), codes::FieldType,
                              "expected numeric [width, height]");
                    }
                }
            }
        }
        if (const Json *meter = member(*voice, "meter")) {
            const std::string base = "/voice/meter";
            if (!meter->is_object()) {
                error(base, codes::FieldType, "expected an object");
            } else {
                checkUnknownKeys(*meter, {"bars", "color", "decay"}, base);
                if (auto bars = intField(*meter, "bars", base, 1, 256)) {
                    spec.voice.meter.bars = *bars;
                }
                if (const auto color = colorField(*meter, "color", base)) {
                    spec.voice.meter.color = *color;
                }
                if (auto decay = numberField(*meter, "decay", base, 0.0, 1.0)) {
                    spec.voice.meter.decay = *decay;
                }
            }
        }
        if (const Json *partial = member(*voice, "partialText")) {
            spec.voice.partialText = textSpecValue(*partial, "/voice/partialText");
        }
    }

    void parseMotion(SkinSpec &spec) {
        const Json *motion = member(document_, "motion");
        if (motion == nullptr) {
            return;
        }
        if (!motion->is_object()) {
            error("/motion", codes::FieldType, "'motion' must be an object");
            return;
        }
        for (auto it = motion->begin(); it != motion->end(); ++it) {
            const std::string base = joinPointer("/motion", it.key());
            if (!it->is_object()) {
                error(base, codes::FieldType, "expected a motion object");
                continue;
            }
            checkUnknownKeys(*it, {"type", "durationMs", "easing", "stiffness", "damping", "mass"},
                             base);
            MotionSpec parsed;
            const auto type = stringField(*it, "type", base);
            if (!type) {
                error(base, codes::FieldMissing, "motion requires 'type'");
                continue;
            }
            if (*type == "tween") {
                parsed.kind = MotionSpec::Kind::Tween;
                if (auto duration = numberField(*it, "durationMs", base, 0.0, 10000.0)) {
                    parsed.tween.durationMs = *duration;
                }
                parsed.tween.easing = stringField(*it, "easing", base).value_or(parsed.tween.easing);
            } else if (*type == "spring") {
                parsed.kind = MotionSpec::Kind::Spring;
                if (auto stiffness = numberField(*it, "stiffness", base, 0.0, 10000.0)) {
                    parsed.spring.stiffness = *stiffness;
                }
                if (auto damping = numberField(*it, "damping", base, 0.0, 10000.0)) {
                    parsed.spring.damping = *damping;
                }
                if (auto mass = numberField(*it, "mass", base, 0.0, 1000.0)) {
                    parsed.spring.mass = *mass;
                }
            } else {
                error(joinPointer(base, "type"), codes::MotionTypeInvalid,
                      "expected tween | spring");
                continue;
            }
            spec.motion.emplace(it.key(), std::move(parsed));
        }
    }

    // ------------------------------------------------------------------ assets

    void noteAsset(const std::string &path, const std::string &pointer) {
        if (path.empty()) {
            return;
        }
        assets_.try_emplace(path, pointer);
    }

    std::vector<AssetEntry> collectAssets() {
        std::vector<AssetEntry> entries;
        entries.reserve(assets_.size());
        for (const auto &[path, pointer] : assets_) {
            if (!isSafeRelativePath(path)) {
                error(pointer, codes::AssetPathInvalid,
                      "'" + path + "' must be a relative path inside the package");
                continue;
            }
            const std::filesystem::path file = root_ / std::filesystem::path(path);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec)) {
                error(pointer, codes::AssetMissing, "asset '" + path + "' does not exist");
                continue;
            }
            if (!isSupportedAsset(file)) {
                error(pointer, codes::AssetExtension,
                      "'" + path + "' has an unsupported extension");
                continue;
            }
            AssetEntry entry;
            entry.path = path;
            entry.size = std::filesystem::file_size(file, ec);
            if (ec) {
                error(pointer, codes::FileRead, "cannot stat '" + path + "': " + ec.message());
                continue;
            }
            if (options_.hashAssets) {
                std::ifstream stream(file, std::ios::binary);
                if (!stream) {
                    error(pointer, codes::FileRead, "cannot read '" + path + "'");
                    continue;
                }
                const std::string bytes{std::istreambuf_iterator<char>(stream),
                                        std::istreambuf_iterator<char>()};
                entry.fnv1a = fnv1a64(bytes);
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const AssetEntry &lhs, const AssetEntry &rhs) { return lhs.path < rhs.path; });
        return entries;
    }

    std::filesystem::path root_;
    const Json &document_;
    LoadOptions options_;
    const Json *tokens_ = nullptr;
    std::vector<Diagnostic> diagnostics_;
    std::map<std::string, std::string> assets_;
};

namespace diag = superior::diagnostics;

void logDiagnostics(const LoadResult &result, const std::filesystem::path &root) {
    for (const auto &diagnostic : result.diagnostics) {
        auto log = diag::Log(diagnostic.severity == Severity::Error ? diag::Level::Error
                              : diagnostic.severity == Severity::Warning ? diag::Level::Warning
                                                                         : diag::Level::Debug,
                             diag::categories::Skin, __FILE__, __LINE__);
        if (log) {
            log.field("skin", root.filename().string())
                .field("code", diagnostic.code)
                .field("pointer", diagnostic.pointer)
                << diagnostic.message;
        }
    }
}

std::string readFile(const std::filesystem::path &file, bool &ok) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        ok = false;
        return {};
    }
    std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    ok = true;
    return text;
}

} // namespace

bool LoadResult::ok() const noexcept {
    if (!spec.has_value()) {
        return false;
    }
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic &d) { return d.severity == Severity::Error; });
}

bool LoadResult::hasErrors() const noexcept {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic &d) { return d.severity == Severity::Error; });
}

std::string LoadResult::summary() const {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error) {
            ++errors;
        } else if (diagnostic.severity == Severity::Warning) {
            ++warnings;
        }
    }
    std::string out = std::to_string(errors) + " error(s), " + std::to_string(warnings) +
                      " warning(s)";
    if (spec) {
        out += ", skin '" + spec->id + "' " + spec->version;
        if (!spec->hash.empty()) {
            out += " hash=" + spec->hash;
        }
    }
    return out;
}

LoadResult loadSkin(const std::filesystem::path &root, const LoadOptions &options) {
    LoadResult result;

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        result.diagnostics.push_back(Diagnostic{Severity::Error, "", std::string(codes::FileMissing),
                                                "package '" + root.string() + "' does not exist"});
        return result;
    }
    if (!std::filesystem::is_directory(root, ec)) {
        result.diagnostics.push_back(Diagnostic{Severity::Error, "", std::string(codes::NotADirectory),
                                                "'" + root.string() + "' is not a directory"});
        return result;
    }

    const std::filesystem::path skinFile = root / std::string(kSkinFileName);
    bool readOk = false;
    const std::string text = readFile(skinFile, readOk);
    if (!readOk) {
        result.diagnostics.push_back(Diagnostic{Severity::Error, "", std::string(codes::FileRead),
                                                "cannot read '" + skinFile.string() + "'"});
        return result;
    }

    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::parse_error &error) {
        result.diagnostics.push_back(Diagnostic{Severity::Error, "", std::string(codes::JsonParse),
                                                error.what()});
        return result;
    }

    Parser parser(root, document, options);
    result = parser.run();

    if (result.spec) {
        SUPERIOR_DEBUG(diag::categories::Skin)
            .field("skin", result.spec->id)
            .field("version", result.spec->version)
            .field("hash", result.spec->hash)
            .field("assets", static_cast<std::uint64_t>(result.spec->assets.size()))
            .field("styles", static_cast<std::uint64_t>(result.spec->panelStyles.size()))
            .field("bytes", static_cast<std::uint64_t>(std::filesystem::file_size(skinFile, ec)))
            << "skin loaded";
    } else {
        SUPERIOR_DEBUG(diag::categories::Skin)
            .field("dir", root.filename().string())
            << "skin rejected";
    }
    logDiagnostics(result, root);
    return result;
}

std::vector<SkinSummary> scanSkins(const std::filesystem::path &root) {
    std::vector<SkinSummary> summaries;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return summaries;
    }
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryError;
        if (!entry.is_directory(entryError)) {
            continue;
        }
        const auto result = loadSkin(entry.path());
        SkinSummary summary;
        summary.path = entry.path().string();
        if (result.spec) {
            summary.id = result.spec->id;
            summary.name = result.spec->name;
            summary.version = result.spec->version;
            summary.hash = result.spec->hash;
            summary.valid = result.ok();
        } else {
            summary.id = entry.path().filename().string();
        }
        summaries.push_back(std::move(summary));
    }
    std::sort(summaries.begin(), summaries.end(),
              [](const SkinSummary &lhs, const SkinSummary &rhs) { return lhs.id < rhs.id; });

    SUPERIOR_DEBUG(diag::categories::Skin)
        .field("found", static_cast<std::uint64_t>(summaries.size()))
        .field("root", root.string())
        << "skin scan finished";
    return summaries;
}

std::uint64_t fnv1a64(std::string_view bytes, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (const char raw : bytes) {
        const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(raw));
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool PanelVariant::matches(bool vertical, std::string_view layoutName) const {
    if (orientation) {
        const std::string &expected = *orientation;
        if (vertical ? expected != "vertical" : expected != "horizontal") {
            return false;
        }
    }
    if (layout && *layout != layoutName) {
        return false;
    }
    return true;
}

} // namespace superior::skin
