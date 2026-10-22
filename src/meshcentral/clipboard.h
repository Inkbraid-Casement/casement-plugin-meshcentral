// Clipboard text between Casement and the device. MeshCentral has no change
// notification for the remote clipboard: the plugin asks for it every two
// seconds (getclip) and passes it on when it changed. Local changes are sent
// with setclip. This class decides what goes where, so that neither side's
// text bounces back, and an answer to a request sent before a setclip can't
// overwrite the newer local text.

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace meshcentral {

class ClipboardSync {
public:
    using Clock = std::chrono::steady_clock;

    /// How long after a setclip answers with other text are taken to be
    /// stale. Answers to requests sent before it may still arrive.
    static constexpr std::chrono::milliseconds settle{6000};

    /// Casement's clipboard changed. True if the text should go to the device.
    bool local_changed(std::string_view text, Clock::time_point now);

    /// A getclip request is being sent.
    void polling(Clock::time_point now);

    /// The device answered a getclip request with `text` (agents don't answer
    /// while the clipboard is empty). The text for Casement, if it is new.
    std::optional<std::string> remote_text(std::string_view text, Clock::time_point now);

private:
    /// The device's clipboard as far as known, line breaks normalised.
    std::optional<std::string> remote_;
    /// Text sent with setclip that the device hasn't confirmed yet.
    std::optional<std::string> pending_;
    Clock::time_point pending_until_{};
    int polls_ = 0;
    bool seen_remote_ = false;
};

/// Text with CR LF and lone CR turned into LF, for comparing clipboards of
/// Windows and other systems.
std::string normalize_line_breaks(std::string_view text);

}  // namespace meshcentral
