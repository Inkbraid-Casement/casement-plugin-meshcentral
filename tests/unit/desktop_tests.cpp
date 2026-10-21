// Desktop commands: the bytes the plugin sends and how it splits what the
// agent sends.

#include "check.h"

#include "meshcentral/desktop.h"

using namespace meshcentral::desktop;

namespace {

Bytes bytes(std::initializer_list<int> list) {
    Bytes b;
    for (int v : list) b.push_back(static_cast<std::uint8_t>(v));
    return b;
}

/// A PICTURE command with `size` bytes of image data, plain or jumbo.
Bytes picture_command(std::uint16_t x, std::uint16_t y, std::size_t size, bool jumbo_form) {
    Bytes inner;
    std::size_t total = 8 + size;
    inner.push_back(0);
    inner.push_back(picture);
    inner.push_back(jumbo_form ? 0 : static_cast<std::uint8_t>(total >> 8));
    inner.push_back(jumbo_form ? 0 : static_cast<std::uint8_t>(total));
    inner.push_back(static_cast<std::uint8_t>(x >> 8));
    inner.push_back(static_cast<std::uint8_t>(x));
    inner.push_back(static_cast<std::uint8_t>(y >> 8));
    inner.push_back(static_cast<std::uint8_t>(y));
    for (std::size_t i = 0; i < size; ++i) inner.push_back(static_cast<std::uint8_t>(i * 7));
    if (!jumbo_form) return inner;
    Bytes b = bytes({0, jumbo, 0, 8});
    for (int shift : {24, 16, 8, 0}) b.push_back(static_cast<std::uint8_t>(total >> shift));
    b.insert(b.end(), inner.begin(), inner.end());
    return b;
}

}  // namespace

TEST(desktop_encodes_input) {
    // The web client's encodings (agent-desktop-0.0.2.js).
    CHECK(key_message(KeyAction::down, 0x41) == bytes({0x00, 0x01, 0x00, 0x06, 0x00, 0x41}));
    CHECK(key_message(KeyAction::up, 0x41) == bytes({0x00, 0x01, 0x00, 0x06, 0x01, 0x41}));
    CHECK(key_message(KeyAction::extended_down, 0x25) == bytes({0x00, 0x01, 0x00, 0x06, 0x04, 0x25}));
    CHECK(key_message(KeyAction::extended_up, 0x25) == bytes({0x00, 0x01, 0x00, 0x06, 0x03, 0x25}));
    CHECK(mouse_message(left_down, 0x0123, 0x0456) == bytes({0x00, 0x02, 0x00, 0x0A, 0x00, 0x02, 0x01, 0x23, 0x04, 0x56}));
    CHECK(mouse_message(0, 1919, 1079) == bytes({0x00, 0x02, 0x00, 0x0A, 0x00, 0x00, 0x07, 0x7F, 0x04, 0x37}));
    CHECK(ctrl_alt_del_message() == bytes({0x00, 0x0A, 0x00, 0x04}));
    CHECK(pause_message(false) == bytes({0x00, 0x08, 0x00, 0x05, 0x00}));
    CHECK(pause_message(true) == bytes({0x00, 0x08, 0x00, 0x05, 0x01}));
    CHECK(refresh_message() == bytes({0x00, 0x06, 0x00, 0x04}));
    CHECK(get_displays_message() == bytes({0x00, 0x0B, 0x00, 0x04}));
    CHECK(set_display_message(2) == bytes({0x00, 0x0C, 0x00, 0x06, 0x00, 0x02}));
    CHECK(set_display_message(all_displays) == bytes({0x00, 0x0C, 0x00, 0x06, 0xFF, 0xFF}));
    // JPEG, quality 50, full size, a frame every 100 ms.
    CHECK(compression_message(jpeg, 50, full_scale, 100) ==
          bytes({0x00, 0x05, 0x00, 0x0A, 0x01, 0x32, 0x04, 0x00, 0x00, 0x64}));
}

TEST(desktop_encodes_wheel) {
    // Length 12, no buttons, position, then the delta as a signed 16-bit
    // number: 120 per notch, as Windows and MeshCentral Router count.
    CHECK(wheel_message(10, 20, 120) == bytes({0x00, 0x02, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x14, 0x00, 0x78}));
    CHECK(wheel_message(10, 20, -120) == bytes({0x00, 0x02, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x14, 0xFF, 0x88}));
    CHECK(wheel_message(0, 0, -360) == bytes({0x00, 0x02, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x98}));
    CHECK(wheel_message(0, 0, 32767) == bytes({0x00, 0x02, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF}));
}

TEST(desktop_reads_one_command_per_message) {
    Reader r;
    r.feed(bytes({0x00, 0x07, 0x00, 0x08, 0x05, 0x00, 0x03, 0x20}));  // SCREEN 1280x800
    auto m = r.next();
    REQUIRE(m);
    CHECK_EQ(m->command, screen);
    auto s = parse_screen(*m);
    REQUIRE(s);
    CHECK_EQ(s->width, 1280);
    CHECK_EQ(s->height, 800);
    CHECK(!r.next());
    CHECK_EQ(r.pending(), 0u);

    r.feed(bytes({0x00, 0x58, 0x00, 0x05, 0x0E}));  // MOUSE_CURSOR 14
    m = r.next();
    REQUIRE(m);
    CHECK_EQ(m->command, mouse_cursor);
    CHECK(parse_byte(*m) == std::optional<std::uint8_t>(14));
}

TEST(desktop_reads_several_and_split_commands) {
    Reader r;
    Bytes stream = bytes({0x00, 0x12, 0x00, 0x05, 0x05});  // KEYSTATE: Num and Caps Lock
    Bytes pic = picture_command(64, 128, 300, false);
    stream.insert(stream.end(), pic.begin(), pic.end());
    // Two commands, the second split over three messages.
    r.feed(std::span(stream).first(20));
    auto m = r.next();
    REQUIRE(m);
    CHECK_EQ(m->command, keystate);
    CHECK(parse_byte(*m) == std::optional<std::uint8_t>(num_lock | caps_lock));
    CHECK(!r.next());
    CHECK_EQ(r.pending(), 15u);
    r.feed(std::span(stream).subspan(20, 100));
    CHECK(!r.next());
    r.feed(std::span(stream).subspan(120));
    m = r.next();
    REQUIRE(m);
    CHECK_EQ(m->command, picture);
    auto p = parse_picture(*m);
    REQUIRE(p);
    CHECK_EQ(p->x, 64);
    CHECK_EQ(p->y, 128);
    CHECK_EQ(p->image.size(), 300u);
    CHECK_EQ(p->image[1], 7);
    CHECK(!r.next());
    CHECK_EQ(r.malformed(), 0u);
}

TEST(desktop_reads_jumbo_commands) {
    // A tile too large for a 16-bit length.
    Reader r;
    Bytes jumbo_tile = picture_command(10, 20, 100000, true);
    r.feed(jumbo_tile);
    auto m = r.next();
    REQUIRE(m);
    CHECK_EQ(m->command, picture);
    CHECK_EQ(m->data.size(), 100008u);
    auto p = parse_picture(*m);
    REQUIRE(p);
    CHECK_EQ(p->x, 10);
    CHECK_EQ(p->y, 20);
    CHECK_EQ(p->image.size(), 100000u);
    CHECK_EQ(p->image[99999], static_cast<std::uint8_t>(99999 * 7));
    CHECK(!r.next());

    // The same, arriving in pieces, then a normal command.
    Bytes stream = picture_command(1, 2, 70000, true);
    Bytes screen_cmd = bytes({0x00, 0x07, 0x00, 0x08, 0x07, 0x80, 0x04, 0x38});
    stream.insert(stream.end(), screen_cmd.begin(), screen_cmd.end());
    for (std::size_t at = 0; at < stream.size(); at += 4096) {
        r.feed(std::span(stream).subspan(at, std::min<std::size_t>(4096, stream.size() - at)));
        if (at + 4096 < stream.size() - screen_cmd.size()) CHECK(!r.next());
    }
    m = r.next();
    REQUIRE(m);
    CHECK_EQ(parse_picture(*m)->image.size(), 70000u);
    m = r.next();
    REQUIRE(m);
    CHECK_EQ(parse_screen(*m)->width, 1920);
}

TEST(desktop_skips_malformed_input) {
    Reader r;
    // Messages too short for a header are ignored, as the web client does.
    r.feed(bytes({0x00, 0x07}));
    CHECK_EQ(r.pending(), 0u);
    // A length below the header's own size can't be followed.
    r.feed(bytes({0x00, 0x07, 0x00, 0x02, 0x01, 0x02, 0x03}));
    CHECK(!r.next());
    CHECK_EQ(r.malformed(), 1u);
    CHECK_EQ(r.pending(), 0u);
    // A jumbo length beyond the limit.
    r.feed(bytes({0x00, 27, 0x00, 0x08, 0x7F, 0xFF, 0xFF, 0xFF, 0x00, 0x03}));
    CHECK(!r.next());
    CHECK_EQ(r.malformed(), 2u);
    // And the next good command still comes through.
    r.feed(bytes({0x00, 0x12, 0x00, 0x05, 0x01}));
    auto m = r.next();
    REQUIRE(m);
    CHECK_EQ(m->command, keystate);
}

TEST(desktop_parses_display_lists) {
    // As the Linux agent sends it: one display, number 0, showing 0.
    Message m{get_displays, {}};
    Bytes one = bytes({0x00, 0x0B, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00});
    m.data = one;
    auto d = parse_displays(m);
    REQUIRE(d);
    CHECK(d->ids == std::vector<std::uint16_t>{0});
    CHECK_EQ(d->current, 0);
    // Windows with two monitors: "all", 1 and 2, showing all.
    Bytes three = bytes({0x00, 0x0B, 0x00, 0x0E, 0x00, 0x03, 0xFF, 0xFF, 0x00, 0x01, 0x00, 0x02, 0xFF, 0xFF});
    m.data = three;
    d = parse_displays(m);
    REQUIRE(d);
    CHECK(d->ids == (std::vector<std::uint16_t>{65535, 1, 2}));
    CHECK_EQ(d->current, 65535);
    // Windows with one monitor lists none.
    Bytes none = bytes({0x00, 0x0B, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00});
    m.data = none;
    d = parse_displays(m);
    REQUIRE(d);
    CHECK(d->ids.empty());
    Bytes truncated = bytes({0x00, 0x0B, 0x00, 0x08, 0x00, 0x03, 0x00, 0x01});
    m.data = truncated;
    CHECK(!parse_displays(m));

    // The display an agent switched to: a byte from Linux, 16 bits from Windows.
    Message shown{set_display, bytes({0x00, 0x0C, 0x00, 0x05, 0x01})};
    CHECK(parse_display_number(shown) == std::optional<std::uint16_t>(1));
    shown.data = bytes({0x00, 0x0C, 0x00, 0x06, 0x00, 0x02});
    CHECK(parse_display_number(shown) == std::optional<std::uint16_t>(2));
    shown.data = bytes({0x00, 0x0C, 0x00, 0x04});
    CHECK(!parse_display_number(shown));

    Bytes info = bytes({0x00, 0x52, 0x00, 0x18, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 0x80, 0x04, 0x38,
                        0x00, 0x02, 0x07, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00});
    m.data = info;
    auto displays = parse_display_info(m);
    REQUIRE(displays.size() == 2);
    CHECK_EQ(displays[1].id, 2);
    CHECK_EQ(displays[1].x, 1920);
    CHECK_EQ(displays[1].width, 1280);
    CHECK_EQ(displays[1].height, 1024);

    Bytes text = bytes({0x00, 0x41, 0x00, 0x07, 'H', 'i', '!'});
    m.data = text;
    CHECK_EQ(parse_text(m), std::string("Hi!"));
}
