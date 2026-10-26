// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/KeyTranslation.hpp"

namespace superior::fcitx5 {

namespace {

// X11 keysym values, written out instead of pulled from fcitx-utils so this
// translation can be linked and tested without Fcitx5. They are the *input*
// format of the adapter, not something the decision layer ever sees.
constexpr std::uint32_t kSpace = 0x0020U;
constexpr std::uint32_t kTab = 0xff09U;
constexpr std::uint32_t kReturn = 0xff0dU;
constexpr std::uint32_t kEscape = 0xff1bU;
constexpr std::uint32_t kBackspace = 0xff08U;
constexpr std::uint32_t kDelete = 0xffffU;
constexpr std::uint32_t kKpEnter = 0xff8dU;
constexpr std::uint32_t kHome = 0xff50U;
constexpr std::uint32_t kLeft = 0xff51U;
constexpr std::uint32_t kUp = 0xff52U;
constexpr std::uint32_t kRight = 0xff53U;
constexpr std::uint32_t kDown = 0xff54U;
constexpr std::uint32_t kPageUp = 0xff55U;
constexpr std::uint32_t kPageDown = 0xff56U;
constexpr std::uint32_t kEnd = 0xff57U;
constexpr std::uint32_t kShiftLeft = 0xffe1U;
constexpr std::uint32_t kShiftRight = 0xffe2U;
constexpr std::uint32_t kControlLeft = 0xffe3U;
constexpr std::uint32_t kControlRight = 0xffe4U;
constexpr std::uint32_t kAltLeft = 0xffe9U;
constexpr std::uint32_t kAltRight = 0xffeaU;
constexpr std::uint32_t kSuperLeft = 0xffebU;
constexpr std::uint32_t kSuperRight = 0xffecU;
constexpr std::uint32_t kKpAdd = 0xffabU;
constexpr std::uint32_t kKpSubtract = 0xffadU;
// Latin-1 printable keys arrive as their own code point.
constexpr std::uint32_t kPlusCodePoint = 0x002bU;
constexpr std::uint32_t kMinusCodePoint = 0x002dU;

} // namespace

// The modifier entries have no policy consumer yet: Shift-tap and right-Ctrl
// gestures are still timed inside the adapter. They are mapped anyway so this
// table is complete rather than "complete except the keys the policy will need
// next", and so `Other` keeps meaning "genuinely not a special key".
core::KeyCode keyCodeFor(std::uint32_t keysym) noexcept {
    switch (keysym) {
    case kSpace:
        return core::KeyCode::Space;
    case kTab:
        return core::KeyCode::Tab;
    case kReturn:
    case kKpEnter:
        return core::KeyCode::Return;
    case kEscape:
        return core::KeyCode::Escape;
    case kBackspace:
        return core::KeyCode::Backspace;
    case kDelete:
        return core::KeyCode::Delete;
    case kHome:
        return core::KeyCode::Home;
    case kLeft:
        return core::KeyCode::Left;
    case kUp:
        return core::KeyCode::Up;
    case kRight:
        return core::KeyCode::Right;
    case kDown:
        return core::KeyCode::Down;
    case kPageUp:
        return core::KeyCode::PageUp;
    case kPageDown:
        return core::KeyCode::PageDown;
    case kEnd:
        return core::KeyCode::End;
    case kShiftLeft:
        return core::KeyCode::ShiftLeft;
    case kShiftRight:
        return core::KeyCode::ShiftRight;
    case kControlLeft:
        return core::KeyCode::ControlLeft;
    case kControlRight:
        return core::KeyCode::ControlRight;
    case kAltLeft:
        return core::KeyCode::AltLeft;
    case kAltRight:
        return core::KeyCode::AltRight;
    case kSuperLeft:
        return core::KeyCode::SuperLeft;
    case kSuperRight:
        return core::KeyCode::SuperRight;
    case kPlusCodePoint:
    case kKpAdd:
        return core::KeyCode::Plus;
    case kMinusCodePoint:
        return core::KeyCode::Minus;
    case kKpSubtract:
        return core::KeyCode::KpSubtract;
    default:
        return core::KeyCode::Other;
    }
}

} // namespace superior::fcitx5
