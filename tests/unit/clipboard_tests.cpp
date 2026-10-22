// Which clipboard text goes where.

#include "check.h"

#include "meshcentral/clipboard.h"

using namespace meshcentral;
using namespace std::chrono_literals;

namespace {

const ClipboardSync::Clock::time_point t0{};

}  // namespace

TEST(clipboard_line_breaks) {
    CHECK_EQ(normalize_line_breaks("a\r\nb\rc\nd"), std::string("a\nb\nc\nd"));
    CHECK_EQ(normalize_line_breaks("\r\n\r\n"), std::string("\n\n"));
    CHECK_EQ(normalize_line_breaks("plain"), std::string("plain"));
}

TEST(clipboard_keeps_what_the_device_had_before) {
    ClipboardSync c;
    c.polling(t0);
    // The first answer is what was there before the session started.
    CHECK(!c.remote_text("old text", t0 + 100ms));
    c.polling(t0 + 2s);
    CHECK(!c.remote_text("old text", t0 + 2100ms));
    // Then the remote user copies something.
    c.polling(t0 + 4s);
    auto t = c.remote_text("new text", t0 + 4100ms);
    REQUIRE(t);
    CHECK_EQ(*t, std::string("new text"));
    c.polling(t0 + 6s);
    CHECK(!c.remote_text("new text", t0 + 6100ms));
}

TEST(clipboard_first_copy_after_an_empty_start) {
    // Agents don't answer while the clipboard is empty, so the first answer
    // after later requests is news.
    ClipboardSync c;
    c.polling(t0);
    c.polling(t0 + 2s);
    c.polling(t0 + 4s);
    auto t = c.remote_text("copied", t0 + 4100ms);
    REQUIRE(t);
    CHECK_EQ(*t, std::string("copied"));
}

TEST(clipboard_local_text_goes_out_once) {
    ClipboardSync c;
    c.polling(t0);
    CHECK(!c.remote_text("remote", t0 + 100ms));
    CHECK(c.local_changed("local", t0 + 1s));
    CHECK(!c.local_changed("local", t0 + 1200ms));
    // Text from the device that Casement echoes back isn't sent again.
    c.polling(t0 + 2s);
    CHECK(!c.remote_text("local", t0 + 2100ms));
    c.polling(t0 + 4s);
    auto t = c.remote_text("remote again", t0 + 4100ms);
    REQUIRE(t);
    CHECK(!c.local_changed("remote again", t0 + 4200ms));
}

TEST(clipboard_ignores_answers_older_than_a_setclip) {
    ClipboardSync c;
    c.polling(t0);
    CHECK(!c.remote_text("before", t0 + 100ms));
    c.polling(t0 + 2s);
    CHECK(c.local_changed("mine", t0 + 2050ms));
    // The answer to the request sent before the setclip still has the old
    // text: it must not replace Casement's newer clipboard.
    CHECK(!c.remote_text("before", t0 + 2100ms));
    // The device confirms the new text; from then on changes count.
    c.polling(t0 + 4s);
    CHECK(!c.remote_text("mine", t0 + 4100ms));
    c.polling(t0 + 6s);
    auto t = c.remote_text("theirs", t0 + 6100ms);
    REQUIRE(t);
    CHECK_EQ(*t, std::string("theirs"));
}

TEST(clipboard_gives_up_waiting_for_a_confirmation) {
    // If the device never reports the text (it failed to set it, or changed
    // it right away), its answers count again after a while.
    ClipboardSync c;
    c.polling(t0);
    CHECK(!c.remote_text("before", t0 + 100ms));
    CHECK(c.local_changed("mine", t0 + 1s));
    CHECK(!c.remote_text("other", t0 + 3s));
    auto t = c.remote_text("other", t0 + 1s + ClipboardSync::settle + 1ms);
    REQUIRE(t);
    CHECK_EQ(*t, std::string("other"));
}

TEST(clipboard_line_breaks_dont_count_as_changes) {
    // Windows agents report CR LF for text that went out with LF.
    ClipboardSync c;
    c.polling(t0);
    CHECK(c.local_changed("one\ntwo", t0 + 100ms));
    CHECK(!c.remote_text("one\r\ntwo", t0 + 2s));
    c.polling(t0 + 10s);
    CHECK(!c.remote_text("one\r\ntwo", t0 + 10100ms));
    // The original text goes to Casement when it does change.
    c.polling(t0 + 12s);
    auto t = c.remote_text("three\r\nfour", t0 + 12100ms);
    REQUIRE(t);
    CHECK_EQ(*t, std::string("three\r\nfour"));
}
