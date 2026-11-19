#pragma once
// src/app/native_mode.hpp — the display's native HiDPI mode, for a fullscreen head (macOS).
//
// A Mac in a scaled mode ("looks like 1680x1050" on a 2560x1600 panel) draws
// everything at 3360x2100 and scales it to the panel. That scaling pass makes
// WindowServer composite every frame, which costs a whole refresh (16.7 ms
// at 60 Hz) between a frame presented and the glass, whatever the app does.
// In the native 2x mode (looks like 1280x800, exactly the panel's pixels) a
// fullscreen opaque Metal layer can go straight to the display.
//
// The switch is made for this app only (kCGConfigureForAppOnly): macOS puts
// the old mode back when the process exits, however it exits.

#if defined(__APPLE__)
#include <cstddef>
#include <cstdint>
#include <cstdio>

// The CoreGraphics / CoreFoundation calls used, declared here: Apple's own
// headers use blocks, which GCC cannot parse (mayag does the same).
extern "C" {
using CGDirectDisplayID = std::uint32_t;
using CGError = std::int32_t;
using CFIndex = long;
using CGDisplayModeRef = struct CGDisplayMode*;
using CGDisplayConfigRef = struct _CGDisplayConfigRef*;
using CFArrayRef = const struct __CFArray*;
using CFDictionaryRef = const struct __CFDictionary*;
using CFTypeRef = const void*;
using CFAllocatorRef = const struct __CFAllocator*;
using CFStringRef = const struct __CFString*;
using CFBooleanRef = const struct __CFBoolean*;
struct CFDictionaryKeyCallBacks;
struct CFDictionaryValueCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFBooleanRef kCFBooleanTrue;
extern const CFStringRef kCGDisplayShowDuplicateLowResolutionModes;
CGDirectDisplayID CGMainDisplayID();
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef, const void**, const void**, CFIndex, const CFDictionaryKeyCallBacks*,
                                   const CFDictionaryValueCallBacks*);
void CFRelease(CFTypeRef);
CFIndex CFArrayGetCount(CFArrayRef);
const void* CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
CFArrayRef CGDisplayCopyAllDisplayModes(CGDirectDisplayID, CFDictionaryRef);
CGDisplayModeRef CGDisplayCopyDisplayMode(CGDirectDisplayID);
void CGDisplayModeRelease(CGDisplayModeRef);
std::size_t CGDisplayModeGetWidth(CGDisplayModeRef);
std::size_t CGDisplayModeGetPixelWidth(CGDisplayModeRef);
std::size_t CGDisplayModeGetPixelHeight(CGDisplayModeRef);
std::uint32_t CGDisplayModeGetIOFlags(CGDisplayModeRef);
double CGDisplayModeGetRefreshRate(CGDisplayModeRef);
CGError CGBeginDisplayConfiguration(CGDisplayConfigRef*);
CGError CGConfigureDisplayWithDisplayMode(CGDisplayConfigRef, CGDirectDisplayID, CGDisplayModeRef, CFDictionaryRef);
CGError CGCompleteDisplayConfiguration(CGDisplayConfigRef, std::uint32_t);
}

namespace i4::app {

/// Switch the main display to its native-pixel 2x mode. false: none found,
/// or already in it (nothing to do), or refused.
inline bool use_native_display_mode() {
    constexpr CGError kOk = 0;
    constexpr std::uint32_t kForAppOnly = 0;  // kCGConfigureForAppOnly
    const CGDirectDisplayID id = CGMainDisplayID();
    const void* keys[] = {kCGDisplayShowDuplicateLowResolutionModes};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef opts = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, opts);
    CFRelease(opts);
    if (modes == nullptr) return false;
    // The panel's own pixels: the mode flagged native (1x).
    std::size_t pw = 0, ph = 0;
    constexpr std::uint32_t kNative = 0x02000000;  // kDisplayModeNativeFlag
    for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
        auto m = static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(modes, i)));
        if (CGDisplayModeGetIOFlags(m) & kNative) {
            pw = CGDisplayModeGetPixelWidth(m);
            ph = CGDisplayModeGetPixelHeight(m);
        }
    }
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(id);
    const double hz = current ? CGDisplayModeGetRefreshRate(current) : 0.0;
    const bool already = current && CGDisplayModeGetPixelWidth(current) == pw &&
                         CGDisplayModeGetWidth(current) * 2 == pw;
    if (current) CGDisplayModeRelease(current);
    CGDisplayModeRef want = nullptr;
    for (CFIndex i = 0; pw != 0 && !already && i < CFArrayGetCount(modes); ++i) {
        auto m = static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(modes, i)));
        if (CGDisplayModeGetPixelWidth(m) == pw && CGDisplayModeGetPixelHeight(m) == ph &&
            CGDisplayModeGetWidth(m) * 2 == pw && (hz == 0.0 || CGDisplayModeGetRefreshRate(m) == hz))
            want = m;
    }
    bool ok = false;
    if (want != nullptr) {
        CGDisplayConfigRef cfg = nullptr;
        if (CGBeginDisplayConfiguration(&cfg) == kOk) {
            CGConfigureDisplayWithDisplayMode(cfg, id, want, nullptr);
            ok = CGCompleteDisplayConfiguration(cfg, kForAppOnly) == kOk;
        }
        if (ok)
            std::fprintf(stderr, "i4-head: display in its native mode (%zux%zu, looks like %zux%zu) while running\n", pw, ph,
                         pw / 2, ph / 2);
    }
    CFRelease(modes);
    return ok;
}

}  // namespace i4::app
#endif
