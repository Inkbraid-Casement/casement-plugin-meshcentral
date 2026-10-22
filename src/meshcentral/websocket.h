// A WebSocket client on WinHTTP, used in asynchronous mode: WinHTTP then
// serialises sends against its own control frames, lets one send and one
// receive run at the same time, and lets a request be abandoned from its
// status callback. The class waits for the callbacks, so callers see
// blocking calls.
//
// Certificates: without a pinned fingerprint WinHTTP validates the server
// certificate the Windows way. With one, WinHTTP's checks are switched off
// and the certificate's SHA-256 is compared right after the TLS handshake,
// before the HTTP request is sent; on a mismatch the request is abandoned.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace meshcentral {

using Fingerprint = std::array<std::uint8_t, 32>;

struct Endpoint {
    /// The host as WinHttpConnect wants it: IPv6 addresses in brackets.
    std::string host;
    std::uint16_t port = 443;
    /// The SHA-256 the server certificate must have. Without it the
    /// certificate must be valid and trusted by Windows.
    std::optional<Fingerprint> pin;
};

struct OpenError {
    enum class Kind { network, untrusted, mismatch, http, timeout };
    Kind kind = Kind::network;
    /// What went wrong, for a sentence for the user ("the name can't be
    /// resolved", "HTTP status 404", …). May be empty for `untrusted`.
    std::string text;
    /// The server certificate's SHA-256, when it was seen.
    std::optional<Fingerprint> fingerprint;
    int http_status = 0;
};

struct Message {
    enum class Type { text, binary, closed, error };
    Type type = Type::error;
    std::vector<std::uint8_t> data;
    /// `closed`: the close status code. `error`: the Windows error code.
    unsigned code = 0;
    /// `closed`: the close reason. `error`: what went wrong.
    std::string text;

    std::string_view view() const { return {reinterpret_cast<const char*>(data.data()), data.size()}; }
};

class WebSocket {
public:
    struct Core;

    /// Connects to wss://host:port<path> and upgrades the connection.
    /// `headers` are extra request headers ("Name: value\r\n" each).
    static std::unique_ptr<WebSocket> open(const Endpoint& endpoint, std::string_view path,
                                           std::string_view headers, std::chrono::milliseconds timeout,
                                           OpenError& error);

    /// Connects only to see the server certificate and returns its SHA-256.
    /// The request is abandoned after the TLS handshake; it carries nothing
    /// secret in case it gets out anyway.
    static std::optional<Fingerprint> fetch_certificate(const Endpoint& endpoint, std::chrono::milliseconds timeout);

    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    /// Sends a message and waits until WinHTTP has taken it. Thread-safe.
    bool send_text(std::string_view text);
    bool send_binary(std::span<const std::uint8_t> data);

    /// The next whole message, a close, or an error. Empty when `timeout`
    /// passes first; the read stays pending for the next call. One thread
    /// at a time.
    std::optional<Message> receive(std::chrono::milliseconds timeout = forever);

    /// Sends a close frame, waits briefly for it to go out and closes the
    /// connection. Pending sends and receives return. Thread-safe.
    void close();

    /// The server certificate's SHA-256.
    Fingerprint certificate() const;

    static constexpr std::chrono::milliseconds forever = std::chrono::milliseconds::max();
    /// The largest message accepted.
    static constexpr std::size_t max_message = std::size_t{64} << 20;

private:
    explicit WebSocket(Core* core) : core_(core) {}
    bool send(int type, const void* data, std::size_t size);

    Core* core_;
    std::mutex send_lock_;
    std::vector<std::uint8_t> partial_;
};

/// Why WinHTTP failed, in words.
std::string winhttp_error_text(unsigned long code);

}  // namespace meshcentral
