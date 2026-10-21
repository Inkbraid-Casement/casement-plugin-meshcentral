// MeshCentral's remote desktop commands, as the agent and MeshCentral's web
// client (public/scripts/agent-desktop-0.0.2.js) exchange them over the
// relay. Each command starts with a 4-byte header: the command number and
// the command's total length, both 16-bit big-endian. Commands too long for
// 16 bits are wrapped in command 27 ("jumbo"): an 8-byte header with a
// 32-bit length, followed by the real command.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace meshcentral::desktop {

enum Command : std::uint16_t {
    key = 1,
    mouse = 2,
    picture = 3,
    compression = 5,
    refresh = 6,
    screen = 7,
    pause = 8,
    ctrl_alt_del = 10,
    get_displays = 11,
    set_display = 12,
    frame_timer = 13,
    init_touch = 14,
    kvm_message = 17,
    keystate = 18,
    jumbo = 27,
    alert = 65,
    display_info = 82,
    key_unicode = 85,
    input_lock = 87,
    mouse_cursor = 88,
};

/// KEY actions. The extended forms set the extended-key flag of the key
/// event on Windows agents (right Ctrl and Alt, the arrows above the
/// numeric keypad and so on).
enum class KeyAction : std::uint8_t { down = 0, up = 1, extended_up = 3, extended_down = 4 };

/// Mouse button flags, the MOUSEEVENTF_* values the agents use.
inline constexpr std::uint8_t left_down = 0x02, left_up = 0x04, right_down = 0x08, right_up = 0x10,
                              middle_down = 0x20, middle_up = 0x40;

/// COMPRESSION image types.
inline constexpr std::uint8_t jpeg = 1;
/// COMPRESSION scaling for full size.
inline constexpr std::uint16_t full_scale = 1024;
/// SET_DISPLAY: all displays at once.
inline constexpr std::uint16_t all_displays = 65535;

using Bytes = std::vector<std::uint8_t>;

Bytes key_message(KeyAction action, std::uint8_t virtual_key);
/// A move with optional button flags, at an absolute position in screen pixels.
Bytes mouse_message(std::uint8_t buttons, std::uint16_t x, std::uint16_t y);
/// A wheel turn at a position. `delta` is in WHEEL_DELTA units: 120 per notch,
/// positive away from the user.
Bytes wheel_message(std::uint16_t x, std::uint16_t y, std::int16_t delta);
Bytes compression_message(std::uint8_t type, std::uint8_t quality, std::uint16_t scaling, std::uint16_t frame_timer_ms);
/// Asks for the list of displays (answered with GET_DISPLAYS).
Bytes get_displays_message();
Bytes set_display_message(std::uint16_t display);
Bytes ctrl_alt_del_message();
Bytes pause_message(bool paused);
Bytes refresh_message();

/// One command from the agent. `data` is the whole command, header
/// included; for a jumbo command it is the inner command.
struct Message {
    std::uint16_t command = 0;
    std::span<const std::uint8_t> data;
};

/// Splits what the agent sends into commands. A WebSocket message normally
/// holds one command, but a command may continue in the following messages,
/// as MeshCentral's web client allows.
class Reader {
public:
    /// Adds one WebSocket message.
    void feed(std::span<const std::uint8_t> bytes);
    /// The next complete command. The data stays valid until the next call
    /// to `feed` or `next`.
    std::optional<Message> next();
    /// Bytes of an incomplete command.
    std::size_t pending() const { return buffer_.size() - pos_; }
    /// Commands skipped because their header was malformed.
    std::size_t malformed() const { return malformed_; }

    /// The longest command accepted; longer ones are dropped.
    static constexpr std::size_t max_command = std::size_t{64} << 20;

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_ = 0;
    std::size_t malformed_ = 0;
};

// Commands from the agent.

struct Screen {
    std::uint16_t width = 0, height = 0;
};
std::optional<Screen> parse_screen(const Message& m);

struct Picture {
    std::uint16_t x = 0, y = 0;
    std::span<const std::uint8_t> image;
};
std::optional<Picture> parse_picture(const Message& m);

struct Displays {
    std::vector<std::uint16_t> ids;
    std::uint16_t current = 0;
};
std::optional<Displays> parse_displays(const Message& m);

struct DisplayInfo {
    std::uint16_t id = 0, x = 0, y = 0, width = 0, height = 0;
};
std::vector<DisplayInfo> parse_display_info(const Message& m);

/// The one-byte value of KEYSTATE, INPUT_LOCK and MOUSE_CURSOR.
std::optional<std::uint8_t> parse_byte(const Message& m);

/// The display an agent says it shows (SET_DISPLAY): one byte from Linux
/// agents, 16 bits from others.
std::optional<std::uint16_t> parse_display_number(const Message& m);

/// The text of a KVM message or alert.
std::string parse_text(const Message& m);

/// KEYSTATE bits.
inline constexpr std::uint8_t num_lock = 1, scroll_lock = 2, caps_lock = 4;

}  // namespace meshcentral::desktop
