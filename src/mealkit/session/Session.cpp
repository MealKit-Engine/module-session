#include "mealkit/session/Session.h"

#include "mealkit/core/Hash.h"

#include <algorithm>
#include <cmath>

namespace mealkit::session {

const char* inputKindName(InputKind kind) {
    switch (kind) {
    case InputKind::KeyboardMouse: return "keyboard/mouse";
    case InputKind::Gamepad: return "gamepad";
    case InputKind::Touch: return "touch";
    }
    return "?";
}

std::string clampUtf8(const std::string& text, u32 maxBytes) {
    if (text.size() <= maxBytes) return text;
    size_t end = maxBytes;
    while (end > 0 && (static_cast<u8>(text[end]) & 0xC0) == 0x80) --end; // don't cut in the middle of a character
    return text.substr(0, end);
}

const char* platformName() {
#if defined(__ANDROID__)
    return "android";
#elif defined(__EMSCRIPTEN__)
    return "web";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#else
    return "other";
#endif
}

const char* determinismGroup() {
#if defined(__clang__)
#define MEALKIT_GROUP_COMPILER "clang"
#elif defined(_MSC_VER)
#define MEALKIT_GROUP_COMPILER "msvc"
#elif defined(__GNUC__)
#define MEALKIT_GROUP_COMPILER "gcc"
#else
#define MEALKIT_GROUP_COMPILER "other"
#endif
#if defined(_M_X64) || defined(__x86_64__)
    return MEALKIT_GROUP_COMPILER "-x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return MEALKIT_GROUP_COMPILER "-arm64";
#else
    return MEALKIT_GROUP_COMPILER "-other";
#endif
}

u32 buildId() {
    static const u32 id = static_cast<u32>(fnv1a(__DATE__ " " __TIME__));
    return id;
}

u32 delayForRtt(f32 rttMs, const AutoDelay& a) {
    const f32 tickMs = a.tickMs > 0.0f ? a.tickMs : 1000.0f / 30.0f;
    const f32 needMs = rttMs * 1.25f + tickMs; // round trip (the longest path via the host) + jitter margin + one processing tick
    const u32 ticks = static_cast<u32>(std::ceil(needMs / tickMs));
    return std::clamp(ticks, std::max(1u, a.minTicks), std::max(a.minTicks, a.maxTicks));
}

} // namespace mealkit::session
