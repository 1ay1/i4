// src/server/keyboard.cpp — xkbcommon, once.
#include "keyboard.hpp"

#include <cstring>
#include <memory>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

namespace i4::server {

namespace {

struct ContextFree {
    void operator()(xkb_context* c) const { xkb_context_unref(c); }
};
struct KeymapFree {
    void operator()(xkb_keymap* k) const { xkb_keymap_unref(k); }
};
struct Free {
    void operator()(char* s) const { std::free(s); }
};

std::uint32_t bit(xkb_keymap* k, const char* name) {
    const auto i = xkb_keymap_mod_get_index(k, name);
    return i == XKB_MOD_INVALID ? 0 : std::uint32_t{1} << i;
}

}  // namespace

std::expected<Keymap, std::string> Keymap::compile(const std::string& layout) {
    std::unique_ptr<xkb_context, ContextFree> ctx{xkb_context_new(XKB_CONTEXT_NO_FLAGS)};
    if (!ctx) return std::unexpected("xkb: no context");
    const xkb_rule_names names{nullptr, nullptr, layout.c_str(), nullptr, nullptr};
    std::unique_ptr<xkb_keymap, KeymapFree> km{
        xkb_keymap_new_from_names(ctx.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS)};
    if (!km) return std::unexpected("xkb: cannot compile layout '" + layout + "'");
    std::unique_ptr<char, Free> text{xkb_keymap_get_as_string(km.get(), XKB_KEYMAP_FORMAT_TEXT_V1)};
    if (!text) return std::unexpected("xkb: cannot serialise the keymap");

    // The NUL is part of what wl_keyboard.keymap promises.
    const auto size = std::strlen(text.get()) + 1;
    wl::OwnedFd fd{::memfd_create("i4-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (!fd) return std::unexpected("memfd_create failed");
    if (::ftruncate(fd.get(), static_cast<off_t>(size)) != 0 ||
        ::pwrite(fd.get(), text.get(), size, 0) != static_cast<ssize_t>(size))
        return std::unexpected("cannot write the keymap");
    ::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);

    Keymap k;
    k.fd_ = std::move(fd);
    k.size_ = static_cast<std::uint32_t>(size);
    k.shift_ = bit(km.get(), XKB_MOD_NAME_SHIFT);
    k.ctrl_ = bit(km.get(), XKB_MOD_NAME_CTRL);
    k.alt_ = bit(km.get(), XKB_MOD_NAME_ALT);
    k.super_ = bit(km.get(), XKB_MOD_NAME_LOGO);
    return k;
}

std::uint32_t Keymap::mask(wm::Mods m) const {
    return (m.has(wm::Mod::shift) ? shift_ : 0) | (m.has(wm::Mod::ctrl) ? ctrl_ : 0) |
           (m.has(wm::Mod::alt) ? alt_ : 0) | (m.has(wm::Mod::super) ? super_ : 0);
}

}  // namespace i4::server
