#include "meshcentral/websocket.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <format>

#include "casement/log.h"
#include "casement/text.h"

namespace meshcentral {

namespace log = casement::log;
using Clock = std::chrono::steady_clock;

/// What the WinHTTP callbacks and the waiting threads share. WinHTTP may
/// call back after a handle was closed, until it reports the handle as
/// closing; each open handle therefore holds a reference, as does the
/// WebSocket object.
struct WebSocket::Core {
    std::atomic<int> refs{1};
    std::mutex lock;
    std::condition_variable changed;

    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    std::atomic<HINTERNET> request{nullptr};
    std::atomic<HINTERNET> socket{nullptr};
    /// The WebSocket handle, kept after closing to recognise its callbacks.
    std::atomic<HINTERNET> socket_id{nullptr};

    std::optional<Fingerprint> pin;
    /// Only fetching the certificate: abandon the request after the handshake.
    bool probe = false;

    // The upgrade request.
    bool step_done = false;
    DWORD step_error = 0;
    DWORD secure_flags = 0;
    std::optional<Fingerprint> fingerprint;
    bool mismatch = false;

    // The WebSocket. The buffers belong here, not to the callers, because
    // WinHTTP may use them until the operation's callback.
    std::vector<std::uint8_t> read_buffer = std::vector<std::uint8_t>(64 * 1024);
    bool read_pending = false, read_done = false;
    DWORD read_error = 0, read_bytes = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE read_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    std::vector<std::uint8_t> write_buffer;
    bool write_done = false;
    DWORD write_error = 0;
    /// A send never completed: the buffer may still be in use.
    bool broken = false;
    bool shutdown_done = false;
    bool closing = false;
    bool closed = false;

    void add_ref() { refs.fetch_add(1); }
    void release() {
        if (refs.fetch_sub(1) == 1) delete this;
    }

    void finish_step(DWORD error) {
        {
            std::lock_guard g(lock);
            step_done = true;
            step_error = error;
        }
        changed.notify_all();
    }

    void finish_read(DWORD error, DWORD bytes, WINHTTP_WEB_SOCKET_BUFFER_TYPE type) {
        {
            std::lock_guard g(lock);
            read_done = true;
            read_error = error;
            read_bytes = bytes;
            read_type = type;
        }
        changed.notify_all();
    }

    void finish_write(DWORD error) {
        {
            std::lock_guard g(lock);
            write_done = true;
            write_error = error;
        }
        changed.notify_all();
    }

    void finish_shutdown() {
        {
            std::lock_guard g(lock);
            shutdown_done = true;
        }
        changed.notify_all();
    }

    /// Closes the request handle, once.
    void close_request() {
        if (HINTERNET h = request.exchange(nullptr)) WinHttpCloseHandle(h);
    }

    /// Closes the connection now; waiting calls return.
    void abort() {
        {
            std::lock_guard g(lock);
            closed = true;
        }
        changed.notify_all();
        if (HINTERNET h = socket.exchange(nullptr)) WinHttpCloseHandle(h);
        close_request();
    }

    void check_certificate(HINTERNET handle);
};

namespace {

std::optional<Fingerprint> sha256(const BYTE* data, DWORD size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return std::nullopt;
    Fingerprint out{};
    NTSTATUS status = BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data), size, out.data(), static_cast<ULONG>(out.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(status)) return std::nullopt;
    return out;
}

std::optional<Fingerprint> certificate_of(HINTERNET request) {
    if (!request) return std::nullopt;
    PCCERT_CONTEXT cert = nullptr;
    DWORD size = sizeof(cert);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) || !cert) return std::nullopt;
    auto digest = sha256(cert->pbCertEncoded, cert->cbCertEncoded);
    CertFreeCertificateContext(cert);
    return digest;
}

void CALLBACK on_status(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    auto* core = reinterpret_cast<WebSocket::Core*>(context);
    if (!core) return;
    bool socket = handle == core->socket_id.load();
    switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
            core->check_certificate(handle);
            break;
        case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
            if (info && length >= sizeof(DWORD)) {
                std::lock_guard g(core->lock);
                core->secure_flags = *static_cast<DWORD*>(info);
            }
            break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            if (!socket) core->finish_step(0);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (socket) {
                auto* r = static_cast<WINHTTP_WEB_SOCKET_ASYNC_RESULT*>(info);
                DWORD error = r->AsyncResult.dwError ? r->AsyncResult.dwError : ERROR_WINHTTP_INTERNAL_ERROR;
                if (r->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION)
                    core->finish_read(error, 0, WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE);
                else if (r->Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION)
                    core->finish_write(error);
                else
                    core->finish_shutdown();
            } else {
                auto* r = static_cast<WINHTTP_ASYNC_RESULT*>(info);
                core->finish_step(r->dwError ? r->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
            }
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            if (socket) {
                auto* s = static_cast<WINHTTP_WEB_SOCKET_STATUS*>(info);
                core->finish_read(0, s->dwBytesTransferred, s->eBufferType);
            }
            break;
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            if (socket) core->finish_write(0);
            break;
        case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
            core->finish_shutdown();
            break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            core->release();
            break;
        default:
            break;
    }
}

bool is_certificate_error(DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
            return true;
        default:
            return false;
    }
}

constexpr DWORD certificate_flags =
    WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED | WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT |
    WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED | WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA |
    WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID | WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID |
    WINHTTP_CALLBACK_STATUS_FLAG_CERT_WRONG_USAGE;

/// Why Windows rejected the certificate. Windows reports flags with
/// ERROR_WINHTTP_SECURE_FAILURE; Wine reports the specific error.
std::string untrusted_reason(DWORD error, DWORD flags) {
    if (error == ERROR_WINHTTP_SECURE_INVALID_CA || (flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA))
        return "it isn't issued by a certification authority that Windows trusts";
    if (error == ERROR_WINHTTP_SECURE_CERT_CN_INVALID || (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID))
        return "it is for a different server name";
    if (error == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID || (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID))
        return "it has expired or isn't valid yet";
    if (error == ERROR_WINHTTP_SECURE_CERT_REVOKED || (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED))
        return "it has been revoked";
    if (error == ERROR_WINHTTP_SECURE_CERT_REV_FAILED || (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED))
        return "Windows can't check whether it was revoked";
    if (error == ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE || (flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_WRONG_USAGE))
        return "it isn't meant for servers";
    if (error == ERROR_WINHTTP_SECURE_INVALID_CERT || (flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT))
        return "it isn't valid";
    return {};
}

/// Waits for the request's next step: 0 when it went well, else the error.
DWORD wait_step(WebSocket::Core* c, Clock::time_point deadline) {
    std::unique_lock g(c->lock);
    if (!c->changed.wait_until(g, deadline, [&] { return c->step_done || c->closed; })) return ERROR_WINHTTP_TIMEOUT;
    if (!c->step_done) return ERROR_WINHTTP_OPERATION_CANCELLED;
    c->step_done = false;
    return c->step_error;
}

/// Opens the handles and sends the request.
DWORD start(WebSocket::Core* c, const Endpoint& endpoint, std::string_view path, std::string_view headers,
            bool upgrade) {
    // System proxy settings, including automatic configuration (Windows 8.1
    // and later); the older setting before that.
    const wchar_t* agent = L"Casement-MeshCentral/" CASEMENT_PLUGIN_VERSION;
    c->session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!c->session)
        c->session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!c->session) return GetLastError();
    if (WinHttpSetStatusCallback(c->session, on_status, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
        return GetLastError();
    c->connect = WinHttpConnect(c->session, casement::widen(endpoint.host).c_str(), endpoint.port, 0);
    if (!c->connect) return GetLastError();
    HINTERNET request = WinHttpOpenRequest(c->connect, L"GET", casement::widen(path).c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) return GetLastError();
    // From here on the request's callbacks carry the core.
    auto context = reinterpret_cast<DWORD_PTR>(c);
    if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        DWORD error = GetLastError();
        WinHttpCloseHandle(request);
        return error;
    }
    c->add_ref();
    c->request = request;
    // No WinHTTP receive timeout: the WebSocket inherits it and would fail
    // on a quiet desktop. Waits here have their own deadline instead.
    WinHttpSetTimeouts(request, 0, 30000, 30000, 0);
    if (c->pin || c->probe) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) return GetLastError();
    }
    DWORD no_redirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirects, sizeof(no_redirects));
    if (upgrade && !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return GetLastError();
    std::wstring extra = casement::widen(headers);
    if (!WinHttpSendRequest(request, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
                            extra.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, context))
        return GetLastError();
    return 0;
}

}  // namespace

void WebSocket::Core::check_certificate(HINTERNET handle) {
    // Runs after the TLS handshake and before the request goes out.
    auto digest = certificate_of(handle);
    bool abandon = false;
    {
        std::lock_guard g(lock);
        if (digest) fingerprint = digest;
        if (probe) {
            abandon = true;
        } else if (pin && (!digest || *digest != *pin)) {
            mismatch = true;
            abandon = true;
        }
    }
    if (abandon) {
        // Closing the request handle stops WinHTTP from sending the request.
        finish_step(ERROR_WINHTTP_OPERATION_CANCELLED);
        close_request();
    }
}

std::unique_ptr<WebSocket> WebSocket::open(const Endpoint& endpoint, std::string_view path,
                                           std::string_view headers, std::chrono::milliseconds timeout,
                                           OpenError& error) {
    auto deadline = Clock::now() + timeout;
    auto* core = new Core();
    std::unique_ptr<WebSocket> ws(new WebSocket(core));
    core->pin = endpoint.pin;

    DWORD e = start(core, endpoint, path, headers, true);
    if (!e) e = wait_step(core, deadline);
    if (!e) {
        HINTERNET request = core->request.load();
        if (!request || !WinHttpReceiveResponse(request, nullptr)) e = request ? GetLastError() : ERROR_WINHTTP_OPERATION_CANCELLED;
        else e = wait_step(core, deadline);
    }
    if (auto digest = certificate_of(core->request.load())) {
        std::lock_guard g(core->lock);
        core->fingerprint = digest;
    }
    std::optional<Fingerprint> fingerprint;
    DWORD flags = 0;
    bool mismatch = false;
    {
        std::lock_guard g(core->lock);
        fingerprint = core->fingerprint;
        flags = core->secure_flags;
        mismatch = core->mismatch;
    }
    // The handshake's certificate is checked before the request goes out;
    // this catches a WinHTTP that sent it anyway.
    if (endpoint.pin && fingerprint && *fingerprint != *endpoint.pin) mismatch = true;
    if (mismatch) {
        error = {OpenError::Kind::mismatch, "the certificate doesn't match the fingerprint", fingerprint, 0};
        return nullptr;
    }
    if (e) {
        log::debug("connecting to {}:{} failed: error {}, certificate flags {:#x}", endpoint.host, endpoint.port, e, flags);
        bool certificate = is_certificate_error(e) || (e == ERROR_WINHTTP_SECURE_FAILURE && (flags & certificate_flags));
        if (!certificate && !endpoint.pin && (e == ERROR_WINHTTP_SECURE_FAILURE || e == ERROR_WINHTTP_SECURE_CHANNEL_ERROR)) {
            // Some systems report a rejected certificate as a failed TLS
            // connection (Wine does). If TLS works without the certificate
            // checks, the certificate was the problem.
            if (!fingerprint) fingerprint = fetch_certificate(endpoint, std::chrono::seconds(15));
            certificate = fingerprint.has_value();
        }
        if (certificate) {
            // Ask again, without checks, only to show the fingerprint.
            if (!fingerprint) fingerprint = fetch_certificate(endpoint, std::chrono::seconds(15));
            error = {OpenError::Kind::untrusted, untrusted_reason(e, flags), fingerprint, 0};
        } else if (e == ERROR_WINHTTP_TIMEOUT) {
            error = {OpenError::Kind::timeout, "the server didn't answer in time", fingerprint, 0};
        } else {
            error = {OpenError::Kind::network, winhttp_error_text(e), fingerprint, 0};
        }
        return nullptr;
    }
    if (endpoint.pin && !fingerprint) {
        error = {OpenError::Kind::mismatch, "the server certificate couldn't be read", fingerprint, 0};
        return nullptr;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(core->request.load(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        error = {OpenError::Kind::http, std::format("the server answered with HTTP status {}", status), fingerprint,
                 static_cast<int>(status)};
        return nullptr;
    }
    core->add_ref();
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(core->request.load(), reinterpret_cast<DWORD_PTR>(core));
    if (!socket) {
        DWORD upgrade_error = GetLastError();
        core->release();
        error = {OpenError::Kind::network, winhttp_error_text(upgrade_error), fingerprint, 0};
        return nullptr;
    }
    core->socket_id = socket;
    core->socket = socket;
    core->close_request();
    return ws;
}

std::optional<Fingerprint> WebSocket::fetch_certificate(const Endpoint& endpoint, std::chrono::milliseconds timeout) {
    auto deadline = Clock::now() + timeout;
    auto* core = new Core();
    std::unique_ptr<WebSocket> ws(new WebSocket(core));
    core->probe = true;
    if (start(core, endpoint, "/", {}, false) == 0) wait_step(core, deadline);
    std::lock_guard g(core->lock);
    return core->fingerprint;
}

WebSocket::~WebSocket() {
    core_->abort();
    if (core_->connect) WinHttpCloseHandle(core_->connect);
    if (core_->session) WinHttpCloseHandle(core_->session);
    core_->release();
}

bool WebSocket::send_text(std::string_view text) {
    return send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text.data(), text.size());
}

bool WebSocket::send_binary(std::span<const std::uint8_t> data) {
    return send(WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, data.data(), data.size());
}

bool WebSocket::send(int type, const void* data, std::size_t size) {
    if (size == 0 || size > max_message) return false;
    std::lock_guard serial(send_lock_);
    Core* c = core_;
    HINTERNET socket;
    {
        std::lock_guard g(c->lock);
        socket = c->socket.load();
        if (c->closed || c->broken || !socket) return false;
        auto bytes = static_cast<const std::uint8_t*>(data);
        c->write_buffer.assign(bytes, bytes + size);
        c->write_done = false;
        c->write_error = 0;
    }
    DWORD e = WinHttpWebSocketSend(socket, static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(type), c->write_buffer.data(),
                                   static_cast<DWORD>(size));
    if (e != NO_ERROR) {
        log::debug("WebSocket send: {}", winhttp_error_text(e));
        return false;
    }
    std::unique_lock g(c->lock);
    if (!c->changed.wait_for(g, std::chrono::seconds(60), [&] { return c->write_done || c->closed; })) {
        c->broken = true;
        return false;
    }
    if (!c->write_done) {
        c->broken = true;  // closed with the send pending
        return false;
    }
    return c->write_error == 0;
}

std::optional<Message> WebSocket::receive(std::chrono::milliseconds timeout) {
    Core* c = core_;
    const bool wait_forever = timeout == forever;
    const auto deadline = wait_forever ? Clock::time_point() : Clock::now() + timeout;
    auto failure = [](DWORD code, std::string text) {
        Message m;
        m.type = Message::Type::error;
        m.code = code;
        m.text = std::move(text);
        return m;
    };
    for (;;) {
        std::unique_lock g(c->lock);
        if (!c->read_pending) {
            HINTERNET socket = c->socket.load();
            if (c->closed || !socket) return failure(ERROR_WINHTTP_OPERATION_CANCELLED, "the connection was closed");
            c->read_pending = true;
            c->read_done = false;
            g.unlock();
            DWORD e = WinHttpWebSocketReceive(socket, c->read_buffer.data(), static_cast<DWORD>(c->read_buffer.size()),
                                              nullptr, nullptr);
            g.lock();
            if (e != NO_ERROR) {
                c->read_pending = false;
                return failure(e, winhttp_error_text(e));
            }
        }
        auto ready = [&] { return c->read_done || c->closed; };
        if (wait_forever) c->changed.wait(g, ready);
        else if (!c->changed.wait_until(g, deadline, ready)) return std::nullopt;
        if (!c->read_done) return failure(ERROR_WINHTTP_OPERATION_CANCELLED, "the connection was closed");
        c->read_done = false;
        c->read_pending = false;
        if (c->read_error) return failure(c->read_error, winhttp_error_text(c->read_error));
        DWORD count = c->read_bytes;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = c->read_type;
        HINTERNET socket = c->socket.load();
        g.unlock();

        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            Message m;
            m.type = Message::Type::closed;
            USHORT status = 0;
            char reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
            DWORD reason_length = 0;
            if (socket && WinHttpWebSocketQueryCloseStatus(socket, &status, reason, sizeof(reason), &reason_length) == NO_ERROR) {
                m.code = status;
                m.text.assign(reason, reason_length);
            }
            partial_.clear();
            return m;
        }
        // No read is pending, so the buffer is ours.
        if (partial_.size() + count > max_message) {
            partial_.clear();
            return failure(ERROR_NOT_ENOUGH_MEMORY, "a message was too long");
        }
        partial_.insert(partial_.end(), c->read_buffer.begin(), c->read_buffer.begin() + count);
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            continue;
        Message m;
        m.type = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ? Message::Type::text : Message::Type::binary;
        m.data = std::move(partial_);
        partial_.clear();
        return m;
    }
}

void WebSocket::close() {
    Core* c = core_;
    HINTERNET socket;
    {
        std::lock_guard g(c->lock);
        if (c->closing) return;
        c->closing = true;
        c->shutdown_done = false;
        socket = c->socket.load();
    }
    if (socket && WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0) == NO_ERROR) {
        std::unique_lock g(c->lock);
        c->changed.wait_for(g, std::chrono::seconds(1), [&] { return c->shutdown_done || c->closed; });
    }
    c->abort();
}

Fingerprint WebSocket::certificate() const {
    std::lock_guard g(core_->lock);
    return core_->fingerprint.value_or(Fingerprint{});
}

std::string winhttp_error_text(unsigned long code) {
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server name can't be resolved";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "the server can't be reached or refused the connection";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
        case ERROR_WINHTTP_TIMEOUT: return "the server didn't answer in time";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "the server's answer isn't valid";
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_FAILURE: return "the TLS connection failed";
        case ERROR_WINHTTP_OPERATION_CANCELLED: return "the connection was abandoned";
        default: break;
    }
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (n && buffer) {
        text = casement::narrow(std::wstring_view(buffer, n));
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.' || text.back() == ' '))
        text.pop_back();
    if (text.empty()) text = std::format("error {}", code);
    return text;
}

}  // namespace meshcentral
