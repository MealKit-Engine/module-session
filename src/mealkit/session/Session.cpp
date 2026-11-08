#include "mealkit/session/Session.h"

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

} // namespace mealkit::session
