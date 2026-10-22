// The agents' cursor numbers and the cursor images sent to Casement.

#include "check.h"

#include <windows.h>

#include "casement/text.h"
#include "meshcentral/cursors.h"

using namespace meshcentral::cursors;

namespace {

std::uint16_t id(LPCWSTR idc) { return static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(idc)); }

}  // namespace

TEST(cursor_indexes) {
    for (bool windows : {true, false}) {
        CHECK(for_index(0, windows).shape == Shape::arrow);
        CHECK(for_index(14, windows).shape == Shape::hidden);
        struct Case {
            std::uint8_t index;
            LPCWSTR idc;
        } cases[] = {{1, IDC_APPSTARTING}, {2, IDC_CROSS},     {3, IDC_HAND},    {5, IDC_IBEAM},
                     {6, IDC_NO},          {7, IDC_SIZEALL},   {8, IDC_SIZENESW}, {9, IDC_SIZENS},
                     {10, IDC_SIZENWSE},   {11, IDC_SIZEWE},   {12, IDC_UPARROW}, {13, IDC_WAIT},
                     {15, IDC_NO},         {16, IDC_SIZEWE},   {17, IDC_SIZENS}};
        for (const auto& c : cases) {
            Kind k = for_index(c.index, windows);
            CHECK(k.shape == Shape::system);
            CHECK_EQ(k.idc, id(c.idc));
        }
        // Numbers the agents may add later fall back to the arrow.
        CHECK(for_index(18, windows).shape == Shape::arrow);
        CHECK(for_index(255, windows).shape == Shape::arrow);
    }
    // Help is help on Windows; Linux agents send it for cursors they can't
    // name, the arrow among them.
    CHECK(for_index(4, true) == (Kind{Shape::system, id(IDC_HELP)}));
    CHECK(for_index(4, false) == Kind{Shape::arrow});
}

TEST(cursor_messages) {
    auto arrow = message(Kind{Shape::arrow});
    CHECK_EQ(arrow["type"].get<std::string>(), std::string("cursor"));
    CHECK(arrow["default"].get<bool>());

    // Hidden: the agent draws the cursor into the picture.
    auto hidden = message(Kind{Shape::hidden});
    CHECK_EQ(hidden["width"].get<int>(), 1);
    auto clear = casement::base64_decode(hidden["rgba"].get<std::string>());
    REQUIRE(clear);
    CHECK(*clear == (std::vector<std::uint8_t>{0, 0, 0, 0}));
}

TEST(cursor_renders_system_cursors) {
    for (int index : {1, 2, 3, 4, 5, 7, 9, 11, 13}) {
        auto m = message(for_index(static_cast<std::uint8_t>(index), true));
        REQUIRE(m.contains("rgba"));
        int w = m["width"].get<int>(), h = m["height"].get<int>();
        CHECK(w >= 16 && w <= 256);
        CHECK(h >= 16 && h <= 256);
        CHECK(m["hot_x"].get<int>() >= 0 && m["hot_x"].get<int>() < w);
        CHECK(m["hot_y"].get<int>() >= 0 && m["hot_y"].get<int>() < h);
        auto rgba = casement::base64_decode(m["rgba"].get<std::string>());
        REQUIRE(rgba);
        REQUIRE(rgba->size() == static_cast<std::size_t>(w * h * 4));
        // Some of the image is drawn and some of it is see-through.
        int opaque = 0, clear = 0;
        for (std::size_t i = 3; i < rgba->size(); i += 4) {
            if ((*rgba)[i] == 255) ++opaque;
            if ((*rgba)[i] == 0) ++clear;
        }
        CHECK(opaque > 0);
        CHECK(clear > 0);
    }
    // The text cursor's hot spot is in its middle.
    auto ibeam = render(id(IDC_IBEAM));
    REQUIRE(ibeam);
    CHECK(ibeam->hot_x > 0);
    CHECK(ibeam->hot_y > 0);
}
