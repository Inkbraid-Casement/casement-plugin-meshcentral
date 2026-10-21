#include "meshcentral/desktop.h"

namespace meshcentral::desktop {

namespace {

void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}

Bytes header(std::uint16_t command, std::uint16_t length) {
    Bytes b;
    b.reserve(length);
    put16(b, command);
    put16(b, length);
    return b;
}

std::uint16_t get16(std::span<const std::uint8_t> d, std::size_t at) {
    return static_cast<std::uint16_t>(d[at] << 8 | d[at + 1]);
}

std::uint32_t get32(std::span<const std::uint8_t> d, std::size_t at) {
    return std::uint32_t{d[at]} << 24 | std::uint32_t{d[at + 1]} << 16 | std::uint32_t{d[at + 2]} << 8 | d[at + 3];
}

}  // namespace

Bytes key_message(KeyAction action, std::uint8_t virtual_key) {
    Bytes b = header(key, 6);
    b.push_back(static_cast<std::uint8_t>(action));
    b.push_back(virtual_key);
    return b;
}

Bytes mouse_message(std::uint8_t buttons, std::uint16_t x, std::uint16_t y) {
    Bytes b = header(mouse, 10);
    b.push_back(0);
    b.push_back(buttons);
    put16(b, x);
    put16(b, y);
    return b;
}

Bytes wheel_message(std::uint16_t x, std::uint16_t y, std::int16_t delta) {
    // The agents read the delta as a signed 16-bit number: Windows passes it
    // on as the wheel data, Linux turns its sign into button 4 or 5.
    Bytes b = header(mouse, 12);
    b.push_back(0);
    b.push_back(0);
    put16(b, x);
    put16(b, y);
    put16(b, static_cast<std::uint16_t>(delta));
    return b;
}

Bytes compression_message(std::uint8_t type, std::uint8_t quality, std::uint16_t scaling, std::uint16_t frame_timer_ms) {
    Bytes b = header(compression, 10);
    b.push_back(type);
    b.push_back(quality);
    put16(b, scaling);
    put16(b, frame_timer_ms);
    return b;
}

Bytes get_displays_message() { return header(get_displays, 4); }

Bytes set_display_message(std::uint16_t display) {
    Bytes b = header(set_display, 6);
    put16(b, display);
    return b;
}

Bytes ctrl_alt_del_message() { return header(ctrl_alt_del, 4); }

Bytes pause_message(bool paused) {
    Bytes b = header(pause, 5);
    b.push_back(paused ? 1 : 0);
    return b;
}

Bytes refresh_message() { return header(refresh, 4); }

void Reader::feed(std::span<const std::uint8_t> bytes) {
    // MeshCentral's web client ignores messages too short for a header unless
    // they continue a command; do the same.
    if (pending() == 0 && bytes.size() < 4) return;
    if (pos_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Message> Reader::next() {
    std::span<const std::uint8_t> rest(buffer_.data() + pos_, buffer_.size() - pos_);
    if (rest.size() < 4) return std::nullopt;
    std::uint16_t command = get16(rest, 0), length = get16(rest, 2);
    if (command == jumbo && length == 8) {
        if (rest.size() < 8) return std::nullopt;
        std::uint32_t inner = get32(rest, 4);
        if (inner < 4 || inner > max_command) {
            // Nothing after a broken header can be trusted; start again with
            // the next message.
            ++malformed_;
            pos_ = buffer_.size();
            return std::nullopt;
        }
        if (rest.size() - 8 < inner) return std::nullopt;
        pos_ += 8 + inner;
        auto data = rest.subspan(8, inner);
        return Message{get16(data, 0), data};
    }
    if (length < 4) {
        ++malformed_;
        pos_ = buffer_.size();
        return std::nullopt;
    }
    if (rest.size() < length) return std::nullopt;
    pos_ += length;
    return Message{command, rest.subspan(0, length)};
}

std::optional<Screen> parse_screen(const Message& m) {
    if (m.data.size() < 8) return std::nullopt;
    return Screen{get16(m.data, 4), get16(m.data, 6)};
}

std::optional<Picture> parse_picture(const Message& m) {
    if (m.data.size() < 8) return std::nullopt;
    return Picture{get16(m.data, 4), get16(m.data, 6), m.data.subspan(8)};
}

std::optional<Displays> parse_displays(const Message& m) {
    // Count, the display numbers, then the current one.
    if (m.data.size() < 6) return std::nullopt;
    std::size_t count = get16(m.data, 4);
    Displays d;
    if (count == 0) return d;
    if (m.data.size() < 6 + 2 * count + 2) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) d.ids.push_back(get16(m.data, 6 + 2 * i));
    d.current = get16(m.data, 6 + 2 * count);
    return d;
}

std::vector<DisplayInfo> parse_display_info(const Message& m) {
    std::vector<DisplayInfo> out;
    if (m.data.size() < 4 || (m.data.size() - 4) % 10 != 0) return out;
    for (std::size_t at = 4; at + 10 <= m.data.size(); at += 10)
        out.push_back({get16(m.data, at), get16(m.data, at + 2), get16(m.data, at + 4), get16(m.data, at + 6),
                       get16(m.data, at + 8)});
    return out;
}

std::optional<std::uint8_t> parse_byte(const Message& m) {
    if (m.data.size() != 5) return std::nullopt;
    return m.data[4];
}

std::optional<std::uint16_t> parse_display_number(const Message& m) {
    if (m.data.size() == 5) return m.data[4];
    if (m.data.size() >= 6) return static_cast<std::uint16_t>(get16(m.data, 4));
    return std::nullopt;
}

std::string parse_text(const Message& m) {
    if (m.data.size() <= 4) return {};
    return std::string(reinterpret_cast<const char*>(m.data.data() + 4), m.data.size() - 4);
}

}  // namespace meshcentral::desktop
