// SPDX-License-Identifier: GPL-3.0-or-later
#include "Canonical.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "superior/skin/Skin.hpp"

namespace superior::skin::detail {
namespace {

using Json = nlohmann::json;

Json colorJson(const Color &color) {
    char buffer[10] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x",
                  static_cast<unsigned>(color.r), static_cast<unsigned>(color.g),
                  static_cast<unsigned>(color.b), static_cast<unsigned>(color.a));
    return std::string(buffer);
}

Json fontJson(const Font &font) {
    Json out;
    out["family"] = font.family;
    out["size"] = font.size;
    out["weight"] = font.weight;
    out["italic"] = font.italic;
    return out;
}

Json insetsJson(const Insets &insets) {
    return Json::array({insets.top, insets.right, insets.bottom, insets.left});
}

Json sliceJson(const Slice &slice) {
    return Json{{"left", slice.left},
                {"top", slice.top},
                {"right", slice.right},
                {"bottom", slice.bottom}};
}

Json imageBrushJson(const ImageBrush &image) {
    Json out;
    out["image"] = image.image;
    out["slice"] = sliceJson(image.slice);
    switch (image.fit) {
    case ImageFit::Stretch:
        out["fit"] = "stretch";
        break;
    case ImageFit::Tile:
        out["fit"] = "tile";
        break;
    case ImageFit::Contain:
        out["fit"] = "contain";
        break;
    case ImageFit::Cover:
        out["fit"] = "cover";
        break;
    }
    return out;
}

void putBrush(Json &out, const Brush &brush) {
    out["radius"] = brush.radius;
    if (brush.color) {
        out["color"] = colorJson(*brush.color);
    }
    if (brush.image) {
        out["image"] = imageBrushJson(*brush.image);
    }
}

Json brushJson(const Brush &brush) {
    Json out = Json::object();
    putBrush(out, brush);
    return out;
}

Json shadowJson(const Shadow &shadow) {
    Json out;
    if (shadow.color) {
        out["color"] = colorJson(*shadow.color);
    }
    out["blur"] = shadow.blur;
    out["offset"] = Json::array({shadow.offsetX, shadow.offsetY});
    return out;
}

Json textSpecJson(const TextSpec &text) {
    Json out;
    if (text.font) {
        out["font"] = fontJson(*text.font);
    }
    if (text.color) {
        out["color"] = colorJson(*text.color);
    }
    return out;
}

Json candidateStyleJson(const CandidateStyle &candidates) {
    Json out;
    out["flow"] = candidates.flow == CandidateFlow::Horizontal ? "horizontal" : "vertical";
    out["maxVisible"] = candidates.maxVisible;
    out["cell"]["padding"] = insetsJson(candidates.cell.padding);
    out["cell"]["gap"] = candidates.cell.gap;
    if (candidates.text.font) {
        out["text"]["font"] = fontJson(*candidates.text.font);
    }
    if (candidates.text.color) {
        out["text"]["color"] = colorJson(*candidates.text.color);
    }
    if (candidates.text.firstColor) {
        out["text"]["firstColor"] = colorJson(*candidates.text.firstColor);
    }
    if (candidates.text.commentColor) {
        out["text"]["commentColor"] = colorJson(*candidates.text.commentColor);
    }
    out["selected"] = brushJson(candidates.selected);
    out["hover"] = brushJson(candidates.hover);
    switch (candidates.paging) {
    case PagingIndicator::None:
        out["paging"] = "none";
        break;
    case PagingIndicator::Dots:
        out["paging"] = "dots";
        break;
    case PagingIndicator::Arrows:
        out["paging"] = "arrows";
        break;
    }
    return out;
}

Json panelStyleJson(const PanelStyle &style) {
    Json out;
    out["background"] = brushJson(style.background);
    out["padding"] = insetsJson(style.padding);
    out["gap"] = style.gap;
    out["preedit"] = textSpecJson(style.preedit);
    out["candidates"] = candidateStyleJson(style.candidates);
    out["separator"] = Json{{"enabled", style.separator.enabled},
                            {"color", colorJson(style.separator.color)},
                            {"thickness", style.separator.thickness}};
    out["shadow"] = shadowJson(style.shadow);
    return out;
}

Json statusItemJson(const StatusItem &item) {
    Json out;
    out["id"] = item.id;
    out["kind"] = item.kind == StatusItemKind::Toggle ? "toggle" : "button";
    out["action"] = item.action;
    out["size"] = Json::array({item.width, item.height});
    out["icons"] = item.icons;
    if (item.background) {
        out["background"] = brushJson(*item.background);
    }
    return out;
}

Json statusJson(const StatusStyle &status) {
    Json out;
    out["background"] = brushJson(status.background);
    out["orientation"] = status.orientation == CandidateFlow::Horizontal ? "horizontal" : "vertical";
    out["padding"] = insetsJson(status.padding);
    out["spacing"] = status.spacing;
    Json items = Json::array();
    for (const auto &item : status.items) {
        items.push_back(statusItemJson(item));
    }
    out["items"] = std::move(items);
    return out;
}

Json voiceJson(const VoiceStyle &voice) {
    Json out;
    out["background"] = brushJson(voice.background);
    out["mic"]["idle"] = voice.micIdle;
    out["mic"]["active"] = voice.micActive;
    out["mic"]["size"] = Json::array({voice.micWidth, voice.micHeight});
    out["meter"]["bars"] = voice.meter.bars;
    if (voice.meter.color) {
        out["meter"]["color"] = colorJson(*voice.meter.color);
    }
    out["meter"]["decay"] = voice.meter.decay;
    out["partialText"] = textSpecJson(voice.partialText);
    return out;
}

Json motionJson(const MotionSpec &motion) {
    if (motion.kind == MotionSpec::Kind::Spring) {
        return Json{{"type", "spring"},
                    {"stiffness", motion.spring.stiffness},
                    {"damping", motion.spring.damping},
                    {"mass", motion.spring.mass}};
    }
    return Json{{"type", "tween"},
                {"durationMs", motion.tween.durationMs},
                {"easing", motion.tween.easing}};
}

std::string hex64(std::uint64_t value) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

} // namespace

std::string specToCanonicalJson(const SkinSpec &spec) {
    Json out;
    out["schema"] = spec.schema;
    out["id"] = spec.id;
    out["name"] = spec.name;
    out["version"] = spec.version;
    out["author"] = spec.author;
    out["license"] = spec.license;
    out["description"] = spec.description;
    out["preview"] = spec.preview;
    out["modes"] = spec.modes;
    out["defaultMode"] = spec.defaultMode;

    Json styles = Json::object();
    for (const auto &[name, style] : spec.panelStyles) {
        styles[name] = panelStyleJson(style);
    }
    Json variants = Json::array();
    for (const auto &variant : spec.panelVariants) {
        Json item = Json::object();
        if (variant.orientation) {
            item["orientation"] = *variant.orientation;
        }
        if (variant.layout) {
            item["layout"] = *variant.layout;
        }
        item["use"] = variant.use;
        variants.push_back(std::move(item));
    }
    out["panel"] = Json{{"styles", std::move(styles)}, {"variants", std::move(variants)}};
    out["status"] = statusJson(spec.status);
    out["voice"] = voiceJson(spec.voice);

    Json motion = Json::object();
    for (const auto &[name, value] : spec.motion) {
        motion[name] = motionJson(value);
    }
    out["motion"] = std::move(motion);

    return out.dump();
}

std::string computeSpecHash(const SkinSpec &spec) {
    std::string canonical = specToCanonicalJson(spec);
    for (const auto &asset : spec.assets) {
        canonical.push_back('\n');
        canonical.append(asset.path);
        canonical.push_back('\0');
        canonical.append(std::to_string(asset.size));
        canonical.push_back('\0');
        canonical.append(hex64(asset.fnv1a));
    }
    return hex64(fnv1a64(canonical));
}

} // namespace superior::skin::detail

namespace superior::skin {

std::string toCanonicalJson(const SkinSpec &spec) {
    return detail::specToCanonicalJson(spec);
}

} // namespace superior::skin
