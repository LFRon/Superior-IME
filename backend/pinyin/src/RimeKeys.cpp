// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/RimeKeys.hpp"

namespace superior::pinyin {

namespace {

/// X11 keysym values. They are librime's wire format, not a platform detail
/// (`架构设计.md` §6.7); keeping them in one function is what stops every host
/// from growing its own table.
constexpr std::uint32_t kKeysymSpace = 0x0020U;
constexpr std::uint32_t kKeysymEscape = 0xFF1BU;
constexpr std::uint32_t kKeysymReturn = 0xFF0DU;
constexpr std::uint32_t kKeysymTab = 0xFF09U;
constexpr std::uint32_t kKeysymBackspace = 0xFF08U;
constexpr std::uint32_t kKeysymDelete = 0xFFFFU;
constexpr std::uint32_t kKeysymLeft = 0xFF51U;
constexpr std::uint32_t kKeysymUp = 0xFF52U;
constexpr std::uint32_t kKeysymRight = 0xFF53U;
constexpr std::uint32_t kKeysymDown = 0xFF54U;
constexpr std::uint32_t kKeysymHome = 0xFF50U;
constexpr std::uint32_t kKeysymEnd = 0xFF57U;
constexpr std::uint32_t kKeysymPageUp = 0xFF55U;
constexpr std::uint32_t kKeysymPageDown = 0xFF56U;
constexpr std::uint32_t kKeysymKpSubtract = 0xFFADU;
constexpr std::uint32_t kKeysymShiftLeft = 0xFFE1U;
constexpr std::uint32_t kKeysymShiftRight = 0xFFE2U;
constexpr std::uint32_t kKeysymControlLeft = 0xFFE3U;
constexpr std::uint32_t kKeysymControlRight = 0xFFE4U;
constexpr std::uint32_t kKeysymAltLeft = 0xFFE9U;
constexpr std::uint32_t kKeysymAltRight = 0xFFEAU;
constexpr std::uint32_t kKeysymSuperLeft = 0xFFEBU;
constexpr std::uint32_t kKeysymSuperRight = 0xFFECU;

/// X11 modifier bits. librime uses the same layout.
constexpr std::uint32_t kShiftMask = 1U << 0U;
constexpr std::uint32_t kControlMask = 1U << 2U;
constexpr std::uint32_t kAltMask = 1U << 3U;
constexpr std::uint32_t kSuperMask = 1U << 6U;

std::uint32_t symFor(const api::KeyPress &key) noexcept {
    switch (key.code) {
    case api::KeyCode::Space:
        return kKeysymSpace;
    case api::KeyCode::Escape:
        return kKeysymEscape;
    case api::KeyCode::Return:
        return kKeysymReturn;
    case api::KeyCode::Tab:
        return kKeysymTab;
    case api::KeyCode::Backspace:
        return kKeysymBackspace;
    case api::KeyCode::Delete:
        return kKeysymDelete;
    case api::KeyCode::Left:
        return kKeysymLeft;
    case api::KeyCode::Right:
        return kKeysymRight;
    case api::KeyCode::Up:
        return kKeysymUp;
    case api::KeyCode::Down:
        return kKeysymDown;
    case api::KeyCode::Home:
        return kKeysymHome;
    case api::KeyCode::End:
        return kKeysymEnd;
    case api::KeyCode::PageUp:
        return kKeysymPageUp;
    case api::KeyCode::PageDown:
        return kKeysymPageDown;
    case api::KeyCode::Plus:
        return static_cast<std::uint32_t>('+');
    case api::KeyCode::Minus:
        return static_cast<std::uint32_t>('-');
    case api::KeyCode::KpSubtract:
        return kKeysymKpSubtract;
    case api::KeyCode::ShiftLeft:
        return kKeysymShiftLeft;
    case api::KeyCode::ShiftRight:
        return kKeysymShiftRight;
    case api::KeyCode::ControlLeft:
        return kKeysymControlLeft;
    case api::KeyCode::ControlRight:
        return kKeysymControlRight;
    case api::KeyCode::AltLeft:
        return kKeysymAltLeft;
    case api::KeyCode::AltRight:
        return kKeysymAltRight;
    case api::KeyCode::SuperLeft:
        return kKeysymSuperLeft;
    case api::KeyCode::SuperRight:
        return kKeysymSuperRight;
    case api::KeyCode::Other:
        break;
    }
    // A printable key's keysym is its **unshifted** character code, with Shift
    // carried separately in `states`. Using `text` here would double-apply Shift
    // for `A` or `:`.
    const char32_t base = key.baseText != 0 ? key.baseText : key.text;
    if (base == 0) {
        return 0;
    }
    // X11 encodes a Unicode keysym as 0x01000000 + codepoint; Latin-1 uses the
    // codepoint directly.
    if (base < 0x100U) {
        return static_cast<std::uint32_t>(base);
    }
    return 0x01000000U + static_cast<std::uint32_t>(base);
}

} // namespace

KeyInput rimeKeyFor(const api::KeyPress &key) noexcept {
    KeyInput out;
    out.sym = symFor(key);
    if (key.shift) {
        out.states |= kShiftMask;
    }
    if (key.control) {
        out.states |= kControlMask;
    }
    if (key.alt) {
        out.states |= kAltMask;
    }
    if (key.super) {
        out.states |= kSuperMask;
    }
    out.release = key.release;
    return out;
}

} // namespace superior::pinyin
