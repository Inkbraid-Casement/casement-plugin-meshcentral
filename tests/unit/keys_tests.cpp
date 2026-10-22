// Scancodes to virtual keys, and the translator's state: Pause, Ctrl+Alt+Del
// and keys released after Num Lock changed.

#include "check.h"

#include <windows.h>

#include "meshcentral/keys.h"

using namespace meshcentral::keys;

namespace {

const Target windows_on{true, true};
const Target windows_off{true, false};
const Target linux_agent{false, true};

std::uint8_t vk(int scancode, const Target& t = windows_on) {
    auto k = virtual_key(static_cast<std::uint32_t>(scancode), t);
    return k ? k->vk : 0;
}

bool extended(int scancode, const Target& t = windows_on) {
    auto k = virtual_key(static_cast<std::uint32_t>(scancode), t);
    return k && k->extended;
}

}  // namespace

TEST(keys_letters_digits_and_punctuation) {
    const char* row1 = "QWERTYUIOP";
    for (int i = 0; i < 10; ++i) CHECK_EQ(vk(0x10 + i), static_cast<std::uint8_t>(row1[i]));
    const char* row2 = "ASDFGHJKL";
    for (int i = 0; i < 9; ++i) CHECK_EQ(vk(0x1E + i), static_cast<std::uint8_t>(row2[i]));
    const char* row3 = "ZXCVBNM";
    for (int i = 0; i < 7; ++i) CHECK_EQ(vk(0x2C + i), static_cast<std::uint8_t>(row3[i]));
    for (int i = 0; i < 9; ++i) CHECK_EQ(vk(0x02 + i), static_cast<std::uint8_t>('1' + i));
    CHECK_EQ(vk(0x0B), '0');
    CHECK_EQ(vk(0x0C), VK_OEM_MINUS);
    CHECK_EQ(vk(0x0D), VK_OEM_PLUS);
    CHECK_EQ(vk(0x1A), VK_OEM_4);
    CHECK_EQ(vk(0x1B), VK_OEM_6);
    CHECK_EQ(vk(0x27), VK_OEM_1);
    CHECK_EQ(vk(0x28), VK_OEM_7);
    CHECK_EQ(vk(0x29), VK_OEM_3);
    CHECK_EQ(vk(0x2B), VK_OEM_5);
    CHECK_EQ(vk(0x33), VK_OEM_COMMA);
    CHECK_EQ(vk(0x34), VK_OEM_PERIOD);
    CHECK_EQ(vk(0x35), VK_OEM_2);
    CHECK_EQ(vk(0x56), VK_OEM_102);
    CHECK_EQ(vk(0x39), VK_SPACE);
    CHECK_EQ(vk(0x01), VK_ESCAPE);
    CHECK_EQ(vk(0x0E), VK_BACK);
    CHECK_EQ(vk(0x0F), VK_TAB);
    CHECK_EQ(vk(0x1C), VK_RETURN);
    CHECK(!extended(0x1C));
    CHECK_EQ(vk(0x3A), VK_CAPITAL);
    CHECK_EQ(vk(0x46), VK_SCROLL);
}

TEST(keys_function_keys) {
    for (int i = 0; i < 10; ++i) CHECK_EQ(vk(0x3B + i), static_cast<std::uint8_t>(VK_F1 + i));
    CHECK_EQ(vk(0x57), VK_F11);
    CHECK_EQ(vk(0x58), VK_F12);
    for (int i = 0; i < 11; ++i) CHECK_EQ(vk(0x64 + i), static_cast<std::uint8_t>(VK_F13 + i));
    CHECK_EQ(vk(0x76), VK_F24);
}

TEST(keys_extended_keys) {
    struct Case {
        int scancode;
        std::uint8_t vk;
    } cases[] = {{0x11C, VK_RETURN}, {0x11D, VK_CONTROL}, {0x135, VK_DIVIDE}, {0x137, VK_SNAPSHOT},
                 {0x138, VK_MENU},   {0x146, VK_CANCEL},  {0x147, VK_HOME},   {0x148, VK_UP},
                 {0x149, VK_PRIOR},  {0x14B, VK_LEFT},    {0x14D, VK_RIGHT},  {0x14F, VK_END},
                 {0x150, VK_DOWN},   {0x151, VK_NEXT},    {0x152, VK_INSERT}, {0x153, VK_DELETE},
                 {0x15B, VK_LWIN},   {0x15C, VK_RWIN},    {0x15D, VK_APPS},   {0x120, VK_VOLUME_MUTE},
                 {0x12E, VK_VOLUME_DOWN}, {0x130, VK_VOLUME_UP}, {0x122, VK_MEDIA_PLAY_PAUSE},
                 {0x16A, VK_BROWSER_BACK}};
    for (const auto& c : cases) {
        CHECK_EQ(vk(c.scancode), c.vk);
        CHECK(extended(c.scancode));
        // Other agents ignore the flag; they get the plain key actions.
        CHECK_EQ(vk(c.scancode, linux_agent), c.vk);
        CHECK(!extended(c.scancode, linux_agent));
    }
    // The fake shifts around Print Screen are no keys.
    CHECK(!virtual_key(0x12A, windows_on));
    CHECK(!virtual_key(0x136, windows_on));
    CHECK(!virtual_key(0x00, windows_on));
    CHECK(!virtual_key(0x200, windows_on));
}

TEST(keys_modifiers) {
    CHECK_EQ(vk(0x2A), VK_SHIFT);
    CHECK_EQ(vk(0x1D), VK_CONTROL);
    CHECK(!extended(0x1D));
    CHECK_EQ(vk(0x38), VK_MENU);
    CHECK(!extended(0x38));
    // Windows tells right Shift by its code; other agents only know VK_SHIFT.
    CHECK_EQ(vk(0x36), VK_RSHIFT);
    CHECK_EQ(vk(0x36, linux_agent), VK_SHIFT);
    // Num Lock: 0x45 as set 1 has it, 0x145 as Windows reports it.
    CHECK_EQ(vk(0x45), VK_NUMLOCK);
    CHECK(extended(0x45));
    CHECK_EQ(vk(0x145), VK_NUMLOCK);
    CHECK(!extended(0x45, linux_agent));
}

TEST(keys_keypad_follows_num_lock) {
    // Windows agents take the virtual key as the meaning.
    CHECK_EQ(vk(0x47, windows_on), VK_NUMPAD7);
    CHECK_EQ(vk(0x47, windows_off), VK_HOME);
    CHECK_EQ(vk(0x52, windows_on), VK_NUMPAD0);
    CHECK_EQ(vk(0x52, windows_off), VK_INSERT);
    CHECK_EQ(vk(0x53, windows_on), VK_DECIMAL);
    CHECK_EQ(vk(0x53, windows_off), VK_DELETE);
    CHECK_EQ(vk(0x4C, windows_off), VK_CLEAR);
    CHECK(!extended(0x47, windows_off));
    // Linux and macOS agents map keypad codes to keypad keys either way.
    CHECK_EQ(vk(0x47, Target{false, false}), VK_NUMPAD7);
    CHECK_EQ(vk(0x53, Target{false, false}), VK_DECIMAL);
    // The operators don't depend on Num Lock.
    CHECK_EQ(vk(0x37, windows_off), VK_MULTIPLY);
    CHECK_EQ(vk(0x4A, windows_off), VK_SUBTRACT);
    CHECK_EQ(vk(0x4E, windows_off), VK_ADD);
}

TEST(keys_translator_plain_keys) {
    Translator t;
    auto a = t.key(0x1E, true);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Action::Kind::key);
    CHECK_EQ(a[0].key.vk, 'A');
    CHECK(a[0].down);
    a = t.key(0x1E, false);
    REQUIRE(a.size() == 1);
    CHECK(!a[0].down);
    CHECK(t.key(0x59, true).empty());  // no key there
}

TEST(keys_translator_pause) {
    // Pause comes as 0x21D followed by 0x45; alone, 0x45 is Num Lock.
    Translator t;
    CHECK(t.key(0x21D, true).empty());
    auto a = t.key(0x45, true);
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_PAUSE);
    CHECK(!a[0].key.extended);
    CHECK(t.key(0x21D, false).empty());
    a = t.key(0x45, false);
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_PAUSE);
    CHECK(!a[0].down);
    // Released without the prefix: still Pause, as it went down.
    t.key(0x21D, true);
    t.key(0x45, true);
    a = t.key(0x45, false);
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_PAUSE);
    a = t.key(0x45, true);
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_NUMLOCK);
}

TEST(keys_translator_ctrl_alt_del) {
    Translator t;
    t.key(0x1D, true);   // left Ctrl
    t.key(0x138, true);  // right Alt
    auto a = t.key(0x153, true);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Action::Kind::ctrl_alt_del);
    // Auto-repeat and the release don't reach the agent.
    CHECK(t.key(0x153, true).empty());
    CHECK(t.key(0x153, false).empty());
    a = t.key(0x138, false);
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_MENU);
    CHECK(a[0].key.extended);
    // Without Alt, Del is just Del.
    a = t.key(0x153, true);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Action::Kind::key);
    CHECK_EQ(a[0].key.vk, VK_DELETE);
    t.key(0x153, false);
    // The keypad's Del counts too.
    t.key(0x38, true);
    a = t.key(0x53, true);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Action::Kind::ctrl_alt_del);
    CHECK(t.key(0x53, false).empty());
}

TEST(keys_translator_releases_what_it_sent) {
    Translator t;
    t.target.num_lock = true;
    auto a = t.key(0x48, true);  // keypad 8 with Num Lock on
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_NUMPAD8);
    t.target.num_lock = false;
    a = t.key(0x48, false);
    REQUIRE(a.size() == 1);
    CHECK_EQ(a[0].key.vk, VK_NUMPAD8);
    // Keys still down when the session ends.
    t.key(0x2A, true);
    t.key(0x11D, true);
    auto held = t.release_all();
    REQUIRE(held.size() == 2);
    for (const auto& h : held) CHECK(!h.down);
    CHECK(t.release_all().empty());
}
