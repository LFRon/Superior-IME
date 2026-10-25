// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superior::skin {

/// The only accepted schema identifier. A file with another schema is rejected
/// instead of being guessed at.
inline constexpr std::string_view kSchemaV1 = "superior.skin/1";

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend constexpr bool operator==(const Color &, const Color &) = default;
};

struct Font {
    std::string family = "Noto Sans CJK SC";
    double size = 16.0;
    int weight = 400;
    bool italic = false;

    friend bool operator==(const Font &, const Font &) = default;
};

/// Logical-pixel insets. JSON accepts `[all]`, `[vertical, horizontal]` or
/// `[top, right, bottom, left]`.
struct Insets {
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;

    friend constexpr bool operator==(const Insets &, const Insets &) = default;
};

/// Nine-slice cut distances from each edge, in source-image pixels.
struct Slice {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    friend constexpr bool operator==(const Slice &, const Slice &) = default;
};

enum class ImageFit {
    Stretch,
    Tile,
    Contain,
    Cover,
};

struct ImageBrush {
    std::string image;
    Slice slice{};
    ImageFit fit = ImageFit::Stretch;

    [[nodiscard]] bool valid() const noexcept { return !image.empty(); }
    friend bool operator==(const ImageBrush &, const ImageBrush &) = default;
};

/// A fill: either a flat colour, an image, or both (image drawn over colour).
/// `radius` applies to the colour layer and to the image clip.
struct Brush {
    std::optional<Color> color;
    std::optional<ImageBrush> image;
    double radius = 0.0;

    friend bool operator==(const Brush &, const Brush &) = default;
};

struct Shadow {
    std::optional<Color> color;
    double blur = 0.0;
    double offsetX = 0.0;
    double offsetY = 0.0;

    friend bool operator==(const Shadow &, const Shadow &) = default;
};

struct TextSpec {
    std::optional<Font> font;
    std::optional<Color> color;

    friend bool operator==(const TextSpec &, const TextSpec &) = default;
};

struct Separator {
    bool enabled = false;
    Color color{};
    double thickness = 1.0;
};

struct CandidateText {
    std::optional<Font> font;
    std::optional<Color> color;
    std::optional<Color> firstColor;
    std::optional<Color> commentColor;
};

struct CandidateCell {
    Insets padding{};
    double gap = 0.0;
};

enum class CandidateFlow {
    Horizontal,
    Vertical,
};

enum class PagingIndicator {
    None,
    Dots,
    Arrows,
};

struct CandidateStyle {
    CandidateFlow flow = CandidateFlow::Horizontal;
    int maxVisible = 7;
    CandidateCell cell{};
    CandidateText text{};
    Brush selected{};
    Brush hover{};
    PagingIndicator paging = PagingIndicator::Dots;
};

/// One panel layout: background, spacing and candidate presentation.
struct PanelStyle {
    Brush background{};
    Insets padding{};
    double gap = 6.0;
    TextSpec preedit{};
    CandidateStyle candidates{};
    Separator separator{};
    Shadow shadow{};
};

/// Selects a panel style from runtime facts. `when` fields are optional; a
/// missing field matches anything. The first matching variant wins.
struct PanelVariant {
    std::optional<std::string> orientation; // "horizontal" | "vertical"
    std::optional<std::string> layout;      // "single-row" | "split"
    std::string use;

    [[nodiscard]] bool matches(bool vertical, std::string_view layoutName) const;
};

enum class StatusItemKind {
    Toggle,
    Button,
};

struct StatusItem {
    std::string id;
    StatusItemKind kind = StatusItemKind::Button;
    std::string action;
    double width = 22.0;
    double height = 22.0;
    std::map<std::string, std::string> icons; // state -> relative asset path
    std::optional<Brush> background;
};

struct StatusStyle {
    Brush background{};
    CandidateFlow orientation = CandidateFlow::Horizontal;
    Insets padding{};
    double spacing = 6.0;
    std::vector<StatusItem> items;
};

struct LevelMeter {
    int bars = 24;
    std::optional<Color> color;
    double decay = 0.85;
};

struct VoiceStyle {
    Brush background{};
    std::string micIdle;
    std::string micActive;
    double micWidth = 48.0;
    double micHeight = 48.0;
    LevelMeter meter{};
    TextSpec partialText{};
};

struct TweenSpec {
    double durationMs = 150.0;
    std::string easing = "cubic-out";
};

struct SpringSpec {
    double stiffness = 420.0;
    double damping = 34.0;
    double mass = 1.0;
};

/// Deterministic motion description. Only these two kinds are accepted; the UI
/// samples them with a monotonic clock it already owns.
struct MotionSpec {
    enum class Kind { Tween, Spring };
    Kind kind = Kind::Tween;
    TweenSpec tween{};
    SpringSpec spring{};
};

struct AssetEntry {
    std::string path;   // relative to the skin root, forward slashes
    std::uint64_t size = 0;
    std::uint64_t fnv1a = 0;

    friend bool operator==(const AssetEntry &, const AssetEntry &) = default;
};

struct SkinSpec {
    std::string schema;
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string license;
    std::string description;
    std::string preview;

    std::vector<std::string> modes;
    std::string defaultMode;

    std::vector<PanelVariant> panelVariants;
    std::map<std::string, PanelStyle> panelStyles;
    StatusStyle status;
    VoiceStyle voice;
    std::map<std::string, MotionSpec> motion;

    std::vector<AssetEntry> assets;

    /// Stable content hash: normalized spec + asset content. Empty if hashing
    /// was disabled or failed.
    std::string hash;
};

struct SkinSummary {
    std::string id;
    std::string name;
    std::string version;
    std::string path;
    std::string hash;
    bool valid = false;
};

} // namespace superior::skin
