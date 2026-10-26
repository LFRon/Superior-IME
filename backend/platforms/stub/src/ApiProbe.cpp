// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile probe for the contract layer (`架构设计.md` §11 P5.5).
//
// Every header under `backend/api/include/` is included here, and this file is
// built for every host, including the ones whose SDK is nowhere in the tree.
// That is the whole assertion: if any contract header grows a platform type —
// an `fd`, a `HWND`, a `QObject`, a keysym — this translation unit stops
// compiling, on the machine of whoever made the change.

#include "superior/api/Capabilities.hpp"
#include "superior/api/Commands.hpp"
#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"
#include "superior/api/Ports.hpp"
#include "superior/api/Scene.hpp"
#include "superior/api/Speech.hpp"
#include "superior/api/TextEdit.hpp"

namespace superior::api {

static_assert(std::is_abstract_v<TextSessionPort>,
              "TextSessionPort must stay a pure contract: an implementation that forgets "
              "`apply` would otherwise silently record nothing");
static_assert(std::is_polymorphic_v<AudioSource>,
              "AudioSource is substituted, so it needs a virtual destructor");

/// Compile-time check that the port vocabulary is made of plain data only: these
/// are the types a Windows or Android adapter must be able to construct without
/// any of our headers' help.
static_assert(std::is_trivially_copyable_v<SessionKey>,
              "a session key is copied into every queued edit, so it must stay plain data");
static_assert(std::is_copy_constructible_v<AudioRequest>,
              "a request is built by the capability and passed to the port, so it must be "
              "copyable without the port knowing what it came from");

} // namespace superior::api
