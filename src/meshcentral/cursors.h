// The remote cursor. Agents don't send cursor images, only which standard
// cursor is showing (MOUSE_CURSOR): an index into the list MeshCentral's
// agents use, which follows the Windows system cursors. The plugin shows
// the matching Windows cursor.
//
// Linux agents name the cursor when the X cursor theme does, and fall back
// to 4 (help) for every cursor they can't name, the plain arrow included.
// From agents other than Windows ones, 4 is therefore the arrow.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "casement/protocol.h"

namespace meshcentral::cursors {

/// What a cursor index stands for.
enum class Shape {
    arrow,   // the standard arrow: Casement's own
    hidden,  // the agent draws the cursor into the picture itself
    system,  // a Windows system cursor
};

struct Kind {
    Shape shape = Shape::arrow;
    /// For `system`: the IDC_* resource id.
    std::uint16_t idc = 0;

    bool operator==(const Kind&) const = default;
};

/// The cursor for an index from the agent; unknown indexes are the arrow.
Kind for_index(std::uint8_t index, bool windows_agent);

/// An RGBA image of a cursor, straight alpha, rows top first.
struct Picture {
    int width = 0, height = 0, hot_x = 0, hot_y = 0;
    std::vector<std::uint8_t> rgba;
};

/// Renders the system cursor with resource id `idc`.
std::optional<Picture> render(std::uint16_t idc);

/// The `cursor` message for a kind of cursor.
casement::json message(const Kind& kind);

}  // namespace meshcentral::cursors
