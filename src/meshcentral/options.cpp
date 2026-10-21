#include "meshcentral/options.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

#include "casement/address.h"
#include "casement/log.h"
#include "casement/text.h"

namespace meshcentral {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

/// A whole number in [low, high], or nothing.
std::optional<int> parse_number(std::string_view text, int low, int high) {
    text = trim(text);
    int value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < low || value > high) return std::nullopt;
    return value;
}

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

}  // namespace

bool is_node_id(std::string_view path) {
    // node/<domain>/<id>: the default domain is empty ("node//…"); the id is
    // MeshCentral's base64 with @ and $ in place of + and /.
    if (!path.starts_with("node/")) return false;
    auto slash = path.find('/', 5);
    if (slash == std::string_view::npos) return false;
    std::string_view domain = path.substr(5, slash - 5), id = path.substr(slash + 1);
    if (id.empty()) return false;
    auto ok = [](std::string_view s, std::string_view extra) {
        return std::all_of(s.begin(), s.end(), [&](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || extra.find(c) != std::string_view::npos;
        });
    };
    return ok(domain, "-_.") && ok(id, "@$");
}

std::optional<std::array<std::uint8_t, 32>> parse_thumbprint(std::string_view text, std::string& error) {
    std::string digits;
    for (char c : trim(text))
        if (c != ':' && c != ' ' && c != '-') digits += c;
    auto bytes = casement::parse_hex(digits);
    if (!bytes) {
        error = "The certificate fingerprint should be hexadecimal digits, like 3F:9A:…";
        return std::nullopt;
    }
    if (bytes->size() == 20) {
        error = "That looks like a SHA-1 thumbprint. The Certificate SHA-256 field needs the "
                "certificate's SHA-256 fingerprint: 64 hexadecimal digits.";
        return std::nullopt;
    }
    if (bytes->size() != 32) {
        error = "The certificate fingerprint should be a SHA-256 fingerprint: 64 hexadecimal digits.";
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> out{};
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return out;
}

std::string format_fingerprint(const std::array<std::uint8_t, 32>& sha256) {
    std::string s = casement::hex(sha256, ":");
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::optional<Options> parse_options(std::string_view address, std::string& error) {
    auto a = casement::parse_address(address, error);
    if (!a) return std::nullopt;
    if (a->scheme != "meshcentral") {
        error = std::format("“{}” isn't a MeshCentral address.", address);
        return std::nullopt;
    }
    Options o;
    o.server = a->host;
    o.url_host = a->url_host();
    if (a->port) o.port = *a->port;
    o.user = a->user;

    std::string_view device = trim(a->path);
    if (device.empty()) {
        error = "The address doesn't say which device to open: add its name or node id.";
        return std::nullopt;
    }
    o.device = std::string(device);
    o.device_is_id = is_node_id(device);
    if (!o.device_is_id && device.starts_with("node/")) {
        error = std::format("“{}” isn't a MeshCentral node id; they look like node//4ZcVb…", device);
        return std::nullopt;
    }

    for (const auto& [name, value] : a->query) {
        if (name == "group") {
            o.group = std::string(trim(value));
        } else if (name == "display") {
            std::string_view v = trim(value);
            if (v.empty()) continue;
            if (equals_ignoring_case(v, "all")) {
                o.display = 65535;
            } else if (auto n = parse_number(v, 0, 65535)) {
                o.display = static_cast<std::uint16_t>(*n);
            } else {
                error = "The display should be a display number, or 65535 (or “all”) for all displays.";
                return std::nullopt;
            }
        } else if (name == "quality") {
            if (trim(value).empty()) continue;
            auto n = parse_number(value, 1, 100);
            if (!n) {
                error = "The JPEG quality should be a number from 1 to 100.";
                return std::nullopt;
            }
            o.quality = *n;
        } else if (name == "fps") {
            if (trim(value).empty()) continue;
            auto n = parse_number(value, 1, 50);
            if (!n) {
                error = "The frames per second should be a number from 1 to 50.";
                return std::nullopt;
            }
            o.fps = *n;
        } else if (name == "thumbprint") {
            if (trim(value).empty()) continue;
            auto t = parse_thumbprint(value, error);
            if (!t) return std::nullopt;
            o.thumbprint = t;
        } else {
            casement::log::debug("ignoring the address parameter “{}”", name);
        }
    }
    return o;
}

}  // namespace meshcentral
