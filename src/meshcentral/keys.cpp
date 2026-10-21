#include "meshcentral/keys.h"

#include <windows.h>

namespace meshcentral::keys {

namespace {

constexpr std::uint32_t pause_prefix = 0x21D;  // E1 1D, followed by 0x45
constexpr std::uint32_t num_lock_key = 0x45;
constexpr std::uint32_t delete_key = 0x153, keypad_delete = 0x53;
constexpr std::uint32_t left_ctrl = 0x1D, right_ctrl = 0x11D, left_alt = 0x38, right_alt = 0x138;

/// Keys without the E0 prefix, US layout. Zero: no key.
constexpr std::uint8_t plain[0x80] = {
    /* 00 */ 0, VK_ESCAPE, '1', '2', '3', '4', '5', '6',
    /* 08 */ '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
    /* 10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 18 */ 'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_CONTROL, 'A', 'S',
    /* 20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1,
    /* 28 */ VK_OEM_7, VK_OEM_3, VK_SHIFT, VK_OEM_5, 'Z', 'X', 'C', 'V',
    /* 30 */ 'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_SHIFT, VK_MULTIPLY,
    /* 38 */ VK_MENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,
    /* 40 */ VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_NUMLOCK, VK_SCROLL, VK_HOME,
    /* 48 */ VK_UP, VK_PRIOR, VK_SUBTRACT, VK_LEFT, VK_CLEAR, VK_RIGHT, VK_ADD, VK_END,
    /* 50 */ VK_DOWN, VK_NEXT, VK_INSERT, VK_DELETE, VK_SNAPSHOT, 0, VK_OEM_102, VK_F11,
    /* 58 */ VK_F12, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, 0, 0, VK_F13, VK_F14, VK_F15, VK_F16,
    /* 68 */ VK_F17, VK_F18, VK_F19, VK_F20, VK_F21, VK_F22, VK_F23, 0,
    /* 70 */ 0, 0, 0, 0, 0, 0, VK_F24, 0,
    /* 78 */ 0, 0, 0, 0, 0, 0, 0, 0,
};

/// The keypad keys that are digits with Num Lock on: 0x47 to 0x53.
constexpr std::uint8_t keypad_digit[0x0D] = {
    VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, 0, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6,
    0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL,
};

/// Keys with the E0 prefix (scancode 0x1xx).
std::uint8_t extended_key(std::uint8_t code) {
    switch (code) {
        case 0x10: return VK_MEDIA_PREV_TRACK;
        case 0x19: return VK_MEDIA_NEXT_TRACK;
        case 0x1C: return VK_RETURN;  // keypad Enter
        case 0x1D: return VK_CONTROL;
        case 0x20: return VK_VOLUME_MUTE;
        case 0x21: return VK_LAUNCH_APP2;
        case 0x22: return VK_MEDIA_PLAY_PAUSE;
        case 0x24: return VK_MEDIA_STOP;
        case 0x2E: return VK_VOLUME_DOWN;
        case 0x30: return VK_VOLUME_UP;
        case 0x32: return VK_BROWSER_HOME;
        case 0x35: return VK_DIVIDE;
        case 0x37: return VK_SNAPSHOT;
        case 0x38: return VK_MENU;
        case 0x45: return VK_NUMLOCK;  // what Windows reports for Num Lock
        case 0x46: return VK_CANCEL;   // Ctrl+Break
        case 0x47: return VK_HOME;
        case 0x48: return VK_UP;
        case 0x49: return VK_PRIOR;
        case 0x4B: return VK_LEFT;
        case 0x4D: return VK_RIGHT;
        case 0x4F: return VK_END;
        case 0x50: return VK_DOWN;
        case 0x51: return VK_NEXT;
        case 0x52: return VK_INSERT;
        case 0x53: return VK_DELETE;
        case 0x5B: return VK_LWIN;
        case 0x5C: return VK_RWIN;
        case 0x5D: return VK_APPS;
        case 0x5F: return VK_SLEEP;
        case 0x65: return VK_BROWSER_SEARCH;
        case 0x66: return VK_BROWSER_FAVORITES;
        case 0x67: return VK_BROWSER_REFRESH;
        case 0x68: return VK_BROWSER_STOP;
        case 0x69: return VK_BROWSER_FORWARD;
        case 0x6A: return VK_BROWSER_BACK;
        case 0x6B: return VK_LAUNCH_APP1;
        case 0x6C: return VK_LAUNCH_MAIL;
        case 0x6D: return VK_LAUNCH_MEDIA_SELECT;
        default: return 0;  // including the fake shifts E0 2A and E0 36
    }
}

}  // namespace

std::optional<VirtualKey> virtual_key(std::uint32_t scancode, const Target& target) {
    if (scancode >= 0x100 && scancode < 0x180) {
        std::uint8_t vk = extended_key(static_cast<std::uint8_t>(scancode & 0x7F));
        if (!vk) return std::nullopt;
        return VirtualKey{vk, target.windows};
    }
    if (scancode >= 0x80) return std::nullopt;
    std::uint8_t vk = plain[scancode];
    if (!vk) return std::nullopt;
    if (scancode >= 0x47 && scancode <= 0x53 && keypad_digit[scancode - 0x47]) {
        // Linux and macOS agents map the keypad codes to keypad keys and let
        // their own Num Lock decide; Windows takes the code as the meaning.
        if (!target.windows || target.num_lock) vk = keypad_digit[scancode - 0x47];
        return VirtualKey{vk, false};
    }
    if (scancode == 0x36 && target.windows) return VirtualKey{VK_RSHIFT, false};
    // Windows sets the extended flag for Num Lock; keybd_event needs it too.
    return VirtualKey{vk, target.windows && scancode == num_lock_key};
}

std::vector<Action> Translator::key(std::uint32_t scancode, bool down) {
    std::vector<Action> out;
    if (scancode == pause_prefix) {
        pause_prefix_ = true;
        return out;
    }
    bool pause = scancode == num_lock_key && pause_prefix_;
    pause_prefix_ = false;

    if (scancode == delete_key || scancode == keypad_delete) {
        bool ctrl = held(left_ctrl) || held(right_ctrl), alt = held(left_alt) || held(right_alt);
        if (down && (swallow_delete_up_ || (ctrl && alt))) {
            // Auto-repeat of a held Del must not repeat the command.
            if (!swallow_delete_up_) out.push_back(Action{Action::Kind::ctrl_alt_del, {}, true});
            swallow_delete_up_ = true;
            return out;
        }
        if (!down && swallow_delete_up_) {
            swallow_delete_up_ = false;
            return out;
        }
    }

    if (down) {
        std::optional<VirtualKey> vk = pause ? VirtualKey{VK_PAUSE, false} : virtual_key(scancode, target);
        if (!vk) return out;
        held_.insert(scancode);
        sent_[scancode] = *vk;
        out.push_back(Action{Action::Kind::key, *vk, true});
    } else {
        held_.erase(scancode);
        std::optional<VirtualKey> vk;
        if (auto it = sent_.find(scancode); it != sent_.end()) {
            vk = it->second;
            sent_.erase(it);
        } else {
            vk = pause ? VirtualKey{VK_PAUSE, false} : virtual_key(scancode, target);
        }
        if (!vk) return out;
        out.push_back(Action{Action::Kind::key, *vk, false});
    }
    return out;
}

std::vector<Action> Translator::release_all() {
    std::vector<Action> out;
    for (const auto& [scancode, vk] : sent_) out.push_back(Action{Action::Kind::key, vk, false});
    sent_.clear();
    held_.clear();
    pause_prefix_ = false;
    swallow_delete_up_ = false;
    return out;
}

}  // namespace meshcentral::keys
