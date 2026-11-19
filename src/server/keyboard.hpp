#pragma once
// src/server/keyboard.hpp — the keymap clients are told about.
//
// Every head speaks evdev keycodes plus i4's Mods (link/protocol.hpp). The
// server compiles ONE xkb keymap, shares it with every client through a
// sealed memfd (so no client can change what another reads), and turns a
// head's Mods into the modifier masks this keymap uses.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <i4/wl/fd.hpp>

#include "i4/wm/bindings.hpp"

struct xkb_keymap;

namespace i4::server {

class Keymap {
public:
    /// Compile `layout` (an xkb layout name: "us", "de", ...).
    [[nodiscard]] static std::expected<Keymap, std::string> compile(const std::string& layout);
    /// A head's own keymap (XKB text v1), checked by compiling it here
    /// first: a client is never handed a keymap xkbcommon cannot read.
    [[nodiscard]] static std::expected<Keymap, std::string> from_text(const std::string& text);

    [[nodiscard]] wl::BorrowedFd fd() const { return wl::BorrowedFd{fd_.get()}; }
    [[nodiscard]] std::uint32_t size() const { return size_; }

    /// The depressed-modifiers mask for wl_keyboard.modifiers.
    [[nodiscard]] std::uint32_t mask(wm::Mods m) const;

private:
    static std::expected<Keymap, std::string> share(::xkb_keymap* km, std::string_view verbatim);

    wl::OwnedFd fd_;
    std::uint32_t size_ = 0;
    std::uint32_t shift_ = 0, ctrl_ = 0, alt_ = 0, super_ = 0;
};

}  // namespace i4::server
