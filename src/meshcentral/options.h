// The settings in a meshcentral:// address:
//
//   meshcentral://<user>@<server>[:<port>]/<device>?group=…&display=…&quality=…&fps=…&thumbprint=…
//
// <device> is a device name, or a node id when it starts with "node/".

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace meshcentral {

struct Options {
    /// The server as the user wrote it, for messages.
    std::string server;
    /// The server as it goes into a URL: IPv6 addresses in brackets.
    std::string url_host;
    std::uint16_t port = 443;
    /// The user part of the address, if any.
    std::string user;

    /// A device name, or a node id ("node/<domain>/<id>") when `device_is_id`.
    std::string device;
    bool device_is_id = false;
    /// A device group name or id ("mesh/<domain>/<id>"); empty for all groups.
    std::string group;

    /// The display to show (65535 for all of them); the agent's choice when empty.
    std::optional<std::uint16_t> display;
    /// JPEG quality, 1 to 100.
    int quality = 50;
    /// The most frames per second the agent sends, 1 to 50.
    int fps = 10;
    /// The SHA-256 fingerprint the server certificate must have. Without one,
    /// the certificate must be trusted by Windows.
    std::optional<std::array<std::uint8_t, 32>> thumbprint;

    /// The agent's frame timer: the time between screen updates.
    int frame_timer_ms() const { return 1000 / fps; }
};

/// Reads an address. On failure `error` is a sentence for the user.
std::optional<Options> parse_options(std::string_view address, std::string& error);

/// Reads a certificate fingerprint: 64 hex digits, optionally separated by
/// colons or spaces. On failure `error` is a sentence for the user.
std::optional<std::array<std::uint8_t, 32>> parse_thumbprint(std::string_view text, std::string& error);

/// A fingerprint as it is usually shown: upper-case hex pairs and colons.
std::string format_fingerprint(const std::array<std::uint8_t, 32>& sha256);

/// Whether a device path is a node id ("node/<domain>/<id>").
bool is_node_id(std::string_view path);

}  // namespace meshcentral
