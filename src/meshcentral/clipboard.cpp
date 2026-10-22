#include "meshcentral/clipboard.h"

namespace meshcentral {

std::string normalize_line_breaks(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

bool ClipboardSync::local_changed(std::string_view text, Clock::time_point now) {
    std::string n = normalize_line_breaks(text);
    // The device has it already, typically because it came from there.
    if (remote_ && *remote_ == n) return false;
    remote_ = n;
    pending_ = std::move(n);
    pending_until_ = now + settle;
    seen_remote_ = true;
    return true;
}

void ClipboardSync::polling(Clock::time_point) { ++polls_; }

std::optional<std::string> ClipboardSync::remote_text(std::string_view text, Clock::time_point now) {
    std::string n = normalize_line_breaks(text);
    if (pending_) {
        if (n == *pending_) {
            pending_.reset();
            remote_ = std::move(n);
            return std::nullopt;
        }
        if (now < pending_until_) return std::nullopt;
        pending_.reset();
    }
    // What the device had before the session started stays there: copying
    // it over Casement's clipboard on connect would surprise the user.
    bool first = !seen_remote_;
    seen_remote_ = true;
    if (first && polls_ <= 1) {
        remote_ = std::move(n);
        return std::nullopt;
    }
    if (remote_ && *remote_ == n) return std::nullopt;
    remote_ = std::move(n);
    return std::string(text);
}

}  // namespace meshcentral
