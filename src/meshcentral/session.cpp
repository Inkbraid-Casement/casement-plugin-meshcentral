#include "meshcentral/session.h"

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "meshcentral/clipboard.h"
#include "meshcentral/control.h"
#include "meshcentral/cursors.h"
#include "meshcentral/desktop.h"
#include "meshcentral/keys.h"
#include "meshcentral/options.h"
#include "meshcentral/tiles.h"
#include "meshcentral/websocket.h"

namespace meshcentral {

namespace log = casement::log;
using casement::get_int;
using casement::get_string;
using casement::Host;
using casement::json;
using casement::State;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

/// How long each step of connecting may take.
constexpr auto step_timeout = 30s;
/// How often the device's clipboard is read.
constexpr auto clipboard_interval = 2s;

constexpr std::string_view tunnel_response_id = "casement-tunnel";

/// The relay's control messages: JSON text with this "ctrlChannel" value.
bool is_control_channel(const json& msg) {
    auto it = msg.find("ctrlChannel");
    if (it == msg.end()) return false;
    if (it->is_string()) return it->get<std::string>() == "102938";
    return it->is_number_integer() && it->get<std::int64_t>() == 102938;
}

/// A random tunnel id, like the web client's: lower-case letters and digits.
std::string random_tunnel_id() {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uint8_t bytes[16] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string id;
    for (std::uint8_t b : bytes) id += alphabet[b % 36];
    return id;
}

/// A connection problem as a sentence for the user.
std::string describe(const OpenError& e, const Options& o, std::string_view what) {
    std::string where = std::format("{}:{}", o.server, o.port);
    switch (e.kind) {
        case OpenError::Kind::untrusted:
            return std::format(
                "The certificate of {} isn't trusted by Windows{}. If the server uses its own certificate, put "
                "its SHA-256 fingerprint in the connection's Certificate SHA-256 field{}",
                where, e.text.empty() ? "" : ": " + e.text,
                e.fingerprint ? std::format(". The server presented {}.", format_fingerprint(*e.fingerprint))
                              : std::string("."));
        case OpenError::Kind::mismatch:
            return std::format(
                "The certificate of {} doesn't match the Certificate SHA-256 field{} If the server's certificate "
                "was replaced on purpose, put the new fingerprint in the field.",
                where,
                e.fingerprint ? std::format(": the server presented {}.", format_fingerprint(*e.fingerprint))
                              : std::string("."));
        case OpenError::Kind::http:
            if (e.http_status == 404)
                return std::format("{} has no MeshCentral {} (HTTP status 404). Is it a MeshCentral server?", where, what);
            return std::format("{} didn't open the MeshCentral {}: {}.", where, what, e.text);
        case OpenError::Kind::timeout:
            return std::format("Can't connect to {}: {}.", where, e.text);
        case OpenError::Kind::network:
            break;
    }
    return std::format("Can't connect to {}: {}.", where, e.text);
}

/// A display number as agents use it; 65535 is all displays together.
std::string display_name(std::uint16_t id) { return id == desktop::all_displays ? "all" : std::to_string(id); }

std::string lock_state_text(std::uint8_t bits) {
    auto on = [&](std::uint8_t bit) { return (bits & bit) ? "on" : "off"; };
    return std::format("Num Lock {}, Caps Lock {}, Scroll Lock {}", on(desktop::num_lock), on(desktop::caps_lock),
                       on(desktop::scroll_lock));
}

class MeshSession final : public casement::Session {
public:
    MeshSession(Host& host, Options options, std::string user, std::string password)
        : host_(host), options_(std::move(options)), user_(std::move(user)), password_(std::move(password)) {
        endpoint_ = Endpoint{options_.url_host, options_.port, options_.thumbprint};
        worker_ = std::thread([this] { run(); });
    }

    ~MeshSession() override {
        stop();
        for (std::thread* t : {&worker_, &control_thread_, &relay_thread_})
            if (t->joinable()) t->join();
    }

    void input(const json& msg) override;
    void disconnect() override;

private:
    // The worker thread: connecting, then reading the device's clipboard.
    void run();
    void login();
    void find_and_join();
    void poll_clipboard();

    // Reader threads.
    void control_loop();
    void relay_loop();
    void on_control(const json& msg);
    void on_relay_text(std::string_view text);
    void on_desktop(const desktop::Message& m, TileDecoder& tiles);
    void on_relay_end(const Message& m);

    // Input from Casement.
    void key(const json& msg);
    void pointer(const json& msg);
    void wheel(const json& msg);
    void clipboard(const json& msg);

    bool send_desktop(const desktop::Bytes& bytes) { return relay_ && relay_->send_binary(bytes); }
    bool send_control(const json& msg) {
        return control_ && control_->send_text(msg.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    void send_key_actions(const std::vector<keys::Action>& actions);

    /// Reports a final state and ends the process.
    [[noreturn]] void end(State state, const std::string& text) { host_.finish(state, text); }

    /// Stops the threads; nothing is reported any more.
    void stop() {
        {
            std::lock_guard g(lock_);
            stopping_ = true;
        }
        changed_.notify_all();
        if (relay_) relay_->close();
        if (control_) control_->close();
    }

    /// Waits until `ready` or the deadline; false on the deadline.
    template <class F>
    bool wait(Clock::time_point deadline, F ready) {
        std::unique_lock g(lock_);
        return changed_.wait_until(g, deadline, [&] { return ready() || stopping_; }) && !stopping_;
    }

    Host& host_;
    Options options_;
    std::string user_;
    std::string password_;
    Endpoint endpoint_;

    // Set before the reader threads start and constant afterwards.
    std::unique_ptr<WebSocket> control_;
    std::unique_ptr<WebSocket> relay_;
    control::Device device_;
    bool windows_agent_ = false;
    std::thread worker_, control_thread_, relay_thread_;

    std::mutex lock_;
    std::condition_variable changed_;
    // Under lock_.
    bool stopping_ = false;
    std::optional<std::vector<control::Group>> groups_;
    std::optional<std::vector<control::Device>> devices_;
    std::string cookie_, relay_cookie_;
    /// The device's node id, once known, for the control thread.
    std::string node_id_;
    bool control_lost_ = false;
    bool tunnel_refused_ = false;
    bool relay_ended_ = false;
    std::string relay_end_text_;
    ClipboardSync clipboard_;
    std::optional<std::string> early_clipboard_;

    // Set by the relay thread.
    std::atomic<bool> connected_{false};
    std::atomic<std::uint32_t> screen_width_{0}, screen_height_{0};
    bool got_screen_ = false;
    std::string consent_text_;
    bool consent_denied_ = false;
    std::optional<cursors::Kind> cursor_;
    /// Rendered system cursors by IDC_* id.
    std::map<std::uint16_t, json> cursor_images_;
    std::optional<std::uint8_t> lock_state_;
    std::optional<desktop::Displays> displays_;
    /// Whether the `display` option was looked at against the agent's list.
    bool display_chosen_ = false;

    // Input, on Casement's thread; the translator also takes the relay
    // thread's Num Lock news.
    std::mutex input_lock_;
    keys::Translator keys_;
    int buttons_ = 0;
    int pointer_x_ = 0, pointer_y_ = 0;
    double wheel_rest_ = 0;
};

void MeshSession::run() {
    login();
    find_and_join();
    poll_clipboard();
}

void MeshSession::login() {
    log::info("connecting to {}:{} as {}", options_.server, options_.port, user_);
    OpenError error;
    // "x-meshauth: *" asks for the credentials inside the WebSocket, so they
    // only leave once the server's certificate has passed its checks.
    control_ = WebSocket::open(endpoint_, "/control.ashx", "x-meshauth: *\r\n", step_timeout, error);
    if (!control_) end(State::failed, describe(error, options_, "control channel"));
    if (options_.thumbprint) log::info("server certificate {}", format_fingerprint(control_->certificate()));

    bool sent = control_->send_text(control::user_auth(user_, password_).dump());
    std::fill(password_.begin(), password_.end(), '\0');
    password_.clear();
    if (!sent) end(State::failed, std::format("The connection to {} broke during the login.", options_.server));

    auto deadline = Clock::now() + step_timeout;
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) end(State::failed, std::format("{} didn't answer the login.", options_.server));
        auto m = control_->receive(left);
        if (!m) continue;
        if (m->type != Message::Type::text) {
            end(State::failed, std::format("{} closed the connection during the login{}.", options_.server,
                                           m->text.empty() ? "" : ": " + m->text));
        }
        json msg = json::parse(m->view(), nullptr, false);
        std::string action = msg.is_object() ? get_string(msg, "action") : std::string();
        if (action == "close") {
            log::info("the server refused the login: {}", m->view());
            auto refusal = control::refusal(msg);
            end(refusal.state, refusal.text);
        }
        if (action == "serverinfo") {
            // MeshCentral sends serverinfo first after every login. Its
            // version is only shown to administrators, so it can't be
            // checked here; see the README for the tested versions.
            auto info = msg.find("serverinfo");
            if (info == msg.end() || !info->is_object())
                end(State::failed, std::format("{} answered the login in a way the plugin doesn't understand. "
                                               "Is it a MeshCentral server?",
                                               options_.server));
            log::info("logged in to MeshCentral server “{}”", get_string(*info, "name"));
            log::debug("serverinfo: {}", info->dump());
            break;
        }
        log::debug("before serverinfo, ignoring {}", action);
    }
    control_thread_ = std::thread([this] { control_loop(); });
}

void MeshSession::find_and_join() {
    for (const json& request : {control::meshes_request(), control::nodes_request(), control::authcookie_request()})
        send_control(request);
    bool ready = wait(Clock::now() + step_timeout,
                      [&] { return (groups_ && devices_ && !cookie_.empty()) || control_lost_; });
    std::vector<control::Group> groups;
    std::vector<control::Device> devices;
    {
        std::lock_guard g(lock_);
        if (stopping_) return;
        if (!ready || control_lost_)
            end(State::failed, std::format("{} didn't send the device list.", options_.server));
        groups = *groups_;
        devices = *devices_;
    }
    log::debug("{} devices in {} device groups", devices.size(), groups.size());
    auto found = control::find_device(devices, groups, {options_.device, options_.device_is_id, options_.group});
    if (!found.device) end(State::failed, found.error);
    device_ = *found.device;
    {
        std::lock_guard g(lock_);
        node_id_ = device_.id;
    }
    log::info("device “{}” ({}), agent type {}, {}", device_.name, device_.id, device_.agent_id, device_.os);
    if (auto problem = control::desktop_problem(device_); !problem.empty()) end(State::failed, problem);
    windows_agent_ = control::windows_agent(device_.agent_id);
    {
        std::lock_guard g(input_lock_);
        keys_.target.windows = windows_agent_;
    }

    std::string tunnel = random_tunnel_id(), cookie, relay_cookie;
    {
        std::lock_guard g(lock_);
        cookie = cookie_;
        relay_cookie = relay_cookie_;
    }
    OpenError error;
    relay_ = WebSocket::open(endpoint_, control::relay_path(device_.id, tunnel, cookie), {}, step_timeout, error);
    if (!relay_) end(State::failed, describe(error, options_, "relay"));
    relay_thread_ = std::thread([this] { relay_loop(); });
    send_control(control::tunnel_request(device_.id, tunnel, relay_cookie, tunnel_response_id));

    // The agent joins the relay, and the server says "c" to both sides.
    ready = wait(Clock::now() + step_timeout, [&] { return connected_ || tunnel_refused_ || relay_ended_; });
    {
        std::lock_guard g(lock_);
        if (stopping_ || connected_) return;
        if (tunnel_refused_)
            end(State::failed, std::format("The server couldn't reach “{}”: its agent isn't connected, or this "
                                           "account may not control the device.",
                                           device_.name));
        if (relay_ended_ && !relay_end_text_.empty())
            end(State::failed, std::format("“{}” didn't start a remote desktop session: {}", device_.name, relay_end_text_));
    }
    end(State::failed, std::format("“{}” didn't answer: its agent may be offline or busy.", device_.name));
}

void MeshSession::poll_clipboard() {
    std::optional<std::string> early;
    {
        std::lock_guard g(lock_);
        if (stopping_) return;
        early = std::move(early_clipboard_);
        early_clipboard_.reset();
    }
    if (early) clipboard(json{{"type", "clipboard"}, {"text", *early}});
    for (;;) {
        {
            std::lock_guard g(lock_);
            if (stopping_ || control_lost_) return;
            clipboard_.polling(Clock::now());
        }
        send_control(control::getclip_request(device_.id));
        std::unique_lock g(lock_);
        if (changed_.wait_for(g, clipboard_interval, [&] { return stopping_; })) return;
    }
}

void MeshSession::control_loop() {
    for (;;) {
        auto m = control_->receive();
        if (!m) continue;
        if (m->type == Message::Type::text) {
            json msg = json::parse(m->view(), nullptr, false);
            if (msg.is_object()) on_control(msg);
            continue;
        }
        if (m->type == Message::Type::binary) continue;
        bool connected;
        {
            std::lock_guard g(lock_);
            if (stopping_) return;
            control_lost_ = true;
            connected = connected_;
        }
        changed_.notify_all();
        if (connected)
            log::warn("the connection to the server's control channel ended ({}); the clipboard isn't shared any more",
                      m->text.empty() ? "closed" : m->text);
        return;
    }
}

void MeshSession::on_control(const json& msg) {
    std::string action = get_string(msg, "action");
    if (action == "meshes") {
        auto groups = control::parse_meshes(msg);
        {
            std::lock_guard g(lock_);
            groups_ = std::move(groups);
        }
        changed_.notify_all();
    } else if (action == "nodes") {
        auto devices = control::parse_nodes(msg);
        {
            std::lock_guard g(lock_);
            devices_ = std::move(devices);
        }
        changed_.notify_all();
    } else if (action == "authcookie") {
        {
            std::lock_guard g(lock_);
            cookie_ = get_string(msg, "cookie");
            relay_cookie_ = get_string(msg, "rcookie");
        }
        changed_.notify_all();
    } else if (action == "msg") {
        std::string type = get_string(msg, "type");
        if (get_string(msg, "responseid") == tunnel_response_id) {
            std::string result = get_string(msg, "result");
            log::debug("tunnel request: {}", result);
            if (result != "OK") {
                {
                    std::lock_guard g(lock_);
                    tunnel_refused_ = true;
                }
                changed_.notify_all();
            }
            return;
        }
        std::string node;
        {
            std::lock_guard g(lock_);
            node = node_id_;
        }
        if (node.empty() || get_string(msg, "nodeid") != node) return;  // another device's business
        if (type == "getclip") {
            auto data = msg.find("data");
            if (data == msg.end() || !data->is_string()) return;
            std::optional<std::string> text;
            {
                std::lock_guard g(lock_);
                text = clipboard_.remote_text(data->get<std::string>(), Clock::now());
            }
            if (text) {
                log::debug("the device's clipboard changed ({} bytes)", text->size());
                host_.send(casement::clipboard_message(*text));
            }
        } else if (type == "setclip") {
            log::debug("the device took the clipboard text: {}", casement::get_bool(msg, "success") ? "yes" : "no");
        } else if (type == "console") {
            log::info("the agent says: {}", get_string(msg, "value"));
        }
    } else if (action == "ping") {
        send_control(control::pong());
    } else if (action == "close") {
        log::warn("the server is closing the control channel: {}", msg.dump());
    }
}

void MeshSession::relay_loop() {
    // WIC needs COM on this thread.
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        TileDecoder tiles;
        if (!tiles.ok()) log::error("{}", tiles.error());
        desktop::Reader reader;
        for (;;) {
            auto m = relay_->receive();
            if (!m) continue;
            if (m->type == Message::Type::text) {
                on_relay_text(m->view());
            } else if (m->type == Message::Type::binary) {
                if (!connected_) continue;
                reader.feed(m->data);
                while (auto command = reader.next()) on_desktop(*command, tiles);
            } else {
                on_relay_end(*m);
                break;
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void MeshSession::on_relay_text(std::string_view text) {
    if (!connected_ && (text == "c" || text == "cr")) {
        // Both sides are on the relay. Say which protocol this is (2:
        // remote desktop), then how the picture should come.
        if (text == "cr") log::info("the server records this session");
        relay_->send_text("2");
        send_desktop(desktop::compression_message(desktop::jpeg, static_cast<std::uint8_t>(options_.quality),
                                                  desktop::full_scale,
                                                  static_cast<std::uint16_t>(options_.frame_timer_ms())));
        // The display is picked from the agent's list once it comes: Linux
        // agents take any number and crash on one they don't have.
        if (options_.display) send_desktop(desktop::get_displays_message());
        connected_ = true;
        changed_.notify_all();
        log::info("connected to “{}”", device_.name);
        host_.send(casement::state_message(State::connected));
        return;
    }
    json msg = json::parse(text, nullptr, false);
    if (!msg.is_object() || !is_control_channel(msg)) {
        log::debug("relay text: {}", text.substr(0, 200));
        return;
    }
    std::string type = get_string(msg, "type");
    if (type == "console") {
        std::string message = get_string(msg, "msg");
        std::int64_t id = get_int(msg, "msgid", -1);
        if (!message.empty()) log::info("the device says: {}", message);
        if (id == 2) {
            consent_denied_ = true;
            consent_text_ = message;
        } else if (!message.empty()) {
            consent_text_ = message;
        }
    } else if (type == "ping") {
        relay_->send_text(R"({"ctrlChannel":"102938","type":"pong"})");
    } else if (type == "metadata") {
        log::debug("session users: {}", msg.dump());
    } else if (type == "close") {
        log::info("the device is closing the session");
    } else {
        log::debug("relay control message: {}", text.substr(0, 200));
    }
}

void MeshSession::on_desktop(const desktop::Message& m, TileDecoder& tiles) {
    using namespace desktop;
    switch (m.command) {
        case picture: {
            auto p = parse_picture(m);
            if (!p || screen_width_ == 0) return;
            Image image;
            std::string error;
            if (!tiles.decode(p->image, image, error)) {
                log::warn("can't decode a tile at {},{}: {}", p->x, p->y, error);
                return;
            }
            auto d = host_.display();
            if (d->width() == 0) return;
            Rect r = blit(image, d->buffer(), d->stride(), d->width(), d->height(), p->x, p->y);
            if (r.empty()) return;
            d->damage(r.x, r.y, r.width, r.height);
            d->flush();
            return;
        }
        case screen: {
            auto s = parse_screen(m);
            if (!s || s->width == 0 || s->height == 0) return;
            bool changed = s->width != screen_width_ || s->height != screen_height_;
            if (changed) {
                log::info("screen {}x{}", s->width, s->height);
                screen_width_ = s->width;
                screen_height_ = s->height;
                host_.display()->set_size(s->width, s->height);
                // As the web client does on a new screen size: make sure
                // the agent isn't paused.
                send_desktop(pause_message(false));
            }
            if (!got_screen_) {
                got_screen_ = true;
                // Modifier keys left down by an earlier viewer would spoil
                // the first keys typed here; the web client releases them too.
                static constexpr std::uint8_t modifiers[] = {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN};
                for (std::uint8_t vk : modifiers) send_desktop(key_message(KeyAction::up, vk));
            }
            return;
        }
        case mouse_cursor: {
            auto index = parse_byte(m);
            if (!index) return;
            auto kind = cursors::for_index(*index, windows_agent_);
            if (kind == cursor_) return;
            cursor_ = kind;
            log::debug("cursor {}", *index);
            if (kind.shape != cursors::Shape::system) {
                host_.send(cursors::message(kind));
                return;
            }
            auto it = cursor_images_.find(kind.idc);
            if (it == cursor_images_.end()) it = cursor_images_.emplace(kind.idc, cursors::message(kind)).first;
            host_.send(it->second);
            return;
        }
        case keystate: {
            auto bits = parse_byte(m);
            if (!bits) return;
            {
                std::lock_guard g(input_lock_);
                keys_.target.num_lock = (*bits & num_lock) != 0;
            }
            if (bits != lock_state_) log::info("the device's keyboard: {}", lock_state_text(*bits));
            lock_state_ = bits;
            return;
        }
        case get_displays: {
            auto d = parse_displays(m);
            if (!d) return;
            std::string list;
            for (std::uint16_t id : d->ids) list += (list.empty() ? "" : ", ") + display_name(id);
            if (!displays_ || displays_->ids != d->ids || displays_->current != d->current)
                log::info("the device's displays: {}; showing {}", list.empty() ? "just one" : list,
                          display_name(d->current));
            displays_ = d;
            // Like the web client, the preferred display is chosen once,
            // from the first list; the agent's own choice stands after that.
            if (options_.display && !display_chosen_) {
                display_chosen_ = true;
                std::uint16_t want = *options_.display;
                if (d->ids.empty())
                    log::info("the device has one display, so the display setting ({}) doesn't apply",
                              display_name(want));
                else if (std::find(d->ids.begin(), d->ids.end(), want) == d->ids.end())
                    log::warn("the device has no display {}; it shows display {}", display_name(want),
                              display_name(d->current));
                else if (d->current != want)
                    send_desktop(set_display_message(want));
            }
            return;
        }
        case set_display:
            if (auto n = parse_display_number(m)) log::debug("the device shows display {}", display_name(*n));
            return;
        case display_info:
            for (const auto& i : parse_display_info(m))
                log::debug("display {}: {}x{} at {},{}", i.id, i.width, i.height, i.x, i.y);
            return;
        case input_lock:
            if (auto b = parse_byte(m)) log::info("the device's own keyboard and mouse are {}", *b ? "locked" : "unlocked");
            return;
        case kvm_message:
        case alert: {
            std::string text = parse_text(m);
            if (!text.empty()) log::info("the device says: {}", text);
            return;
        }
        default:
            log::trace("desktop command {} ({} bytes)", m.command, m.data.size());
            return;
    }
}

void MeshSession::on_relay_end(const Message& m) {
    std::string why = m.type == Message::Type::closed ? "closed" : m.text;
    {
        std::lock_guard g(lock_);
        if (stopping_) return;
        if (!connected_) {
            relay_ended_ = true;
            relay_end_text_ = consent_text_;
        }
    }
    changed_.notify_all();
    if (!connected_) return;
    log::info("the relay ended: {}", why);
    if (consent_denied_) end(State::failed, control::consent_refused(device_.name, consent_text_));
    if (!got_screen_)
        end(State::failed, std::format("“{}” ended the session before showing its screen{}", device_.name,
                                       consent_text_.empty() ? "." : ": " + consent_text_));
    if (m.type == Message::Type::closed)
        end(State::closed, std::format("“{}” or the MeshCentral server ended the session.", device_.name));
    // The reason is in the log; WinHTTP's words for a vanished server vary.
    end(State::closed, "The connection to the MeshCentral server was lost.");
}

void MeshSession::input(const json& msg) {
    std::string type = get_string(msg, "type");
    if (type == "clipboard") {
        clipboard(msg);
        return;
    }
    if (!connected_) return;
    if (type == "key") key(msg);
    else if (type == "pointer") pointer(msg);
    else if (type == "wheel") wheel(msg);
}

void MeshSession::send_key_actions(const std::vector<keys::Action>& actions) {
    for (const auto& a : actions) {
        if (a.kind == keys::Action::Kind::ctrl_alt_del) {
            log::debug("Ctrl+Alt+Del");
            send_desktop(desktop::ctrl_alt_del_message());
            continue;
        }
        auto action = a.key.extended ? (a.down ? desktop::KeyAction::extended_down : desktop::KeyAction::extended_up)
                                     : (a.down ? desktop::KeyAction::down : desktop::KeyAction::up);
        send_desktop(desktop::key_message(action, a.key.vk));
    }
}

void MeshSession::key(const json& msg) {
    auto scancode = static_cast<std::uint32_t>(get_int(msg, "scancode"));
    bool down = casement::get_bool(msg, "down");
    std::vector<keys::Action> actions;
    {
        std::lock_guard g(input_lock_);
        actions = keys_.key(scancode, down);
    }
    if (actions.empty()) log::debug("nothing to send for scancode {:#x} {}", scancode, down ? "down" : "up");
    send_key_actions(actions);
}

void MeshSession::pointer(const json& msg) {
    std::uint32_t w = screen_width_, h = screen_height_;
    if (w == 0 || h == 0) return;
    int x = static_cast<int>(std::clamp<std::int64_t>(get_int(msg, "x"), 0, w - 1));
    int y = static_cast<int>(std::clamp<std::int64_t>(get_int(msg, "y"), 0, h - 1));
    int buttons = static_cast<int>(get_int(msg, "buttons"));
    std::vector<desktop::Bytes> out;
    {
        std::lock_guard g(input_lock_);
        struct Button {
            int bit;
            std::uint8_t down, up;
        };
        // Casement: 1 left, 2 middle, 4 right.
        static constexpr Button map[] = {{1, desktop::left_down, desktop::left_up},
                                         {4, desktop::right_down, desktop::right_up},
                                         {2, desktop::middle_down, desktop::middle_up}};
        int changed = buttons ^ buttons_;
        for (const auto& b : map)
            if (changed & b.bit)
                out.push_back(desktop::mouse_message((buttons & b.bit) ? b.down : b.up, static_cast<std::uint16_t>(x),
                                                     static_cast<std::uint16_t>(y)));
        if (out.empty() && (x != pointer_x_ || y != pointer_y_))
            out.push_back(desktop::mouse_message(0, static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)));
        buttons_ = buttons;
        pointer_x_ = x;
        pointer_y_ = y;
    }
    for (const auto& b : out) send_desktop(b);
}

void MeshSession::wheel(const json& msg) {
    auto it = msg.find("delta");
    if (it == msg.end() || !it->is_number()) return;
    std::vector<desktop::Bytes> out;
    {
        std::lock_guard g(input_lock_);
        // 120 per notch, as Windows counts it and as MeshCentral Router
        // sends it. Linux and macOS agents turn every message into one wheel
        // click, so they get one message per whole notch.
        wheel_rest_ += it->get<double>() * WHEEL_DELTA;
        auto x = static_cast<std::uint16_t>(pointer_x_), y = static_cast<std::uint16_t>(pointer_y_);
        if (keys_.target.windows) {
            double whole = std::clamp(std::trunc(wheel_rest_), -32767.0, 32767.0);
            if (whole != 0) {
                out.push_back(desktop::wheel_message(x, y, static_cast<std::int16_t>(whole)));
                wheel_rest_ -= whole;
            }
        } else {
            for (int i = 0; i < 100 && std::fabs(wheel_rest_) >= WHEEL_DELTA; ++i) {
                auto notch = static_cast<std::int16_t>(wheel_rest_ > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
                out.push_back(desktop::wheel_message(x, y, notch));
                wheel_rest_ -= notch;
            }
            if (std::fabs(wheel_rest_) >= WHEEL_DELTA) wheel_rest_ = 0;
        }
    }
    for (const auto& b : out) send_desktop(b);
}

void MeshSession::clipboard(const json& msg) {
    std::string text = get_string(msg, "text");
    if (text.empty()) return;
    bool send = false;
    {
        std::lock_guard g(lock_);
        if (!connected_) {
            // Sent once connected.
            early_clipboard_ = text;
            return;
        }
        if (control_lost_) return;
        send = clipboard_.local_changed(text, Clock::now());
    }
    if (!send) return;
    log::debug("sending the clipboard to the device ({} bytes)", text.size());
    send_control(control::setclip_request(device_.id, text));
}

void MeshSession::disconnect() {
    log::info("disconnecting");
    if (connected_) {
        std::vector<keys::Action> held;
        {
            std::lock_guard g(input_lock_);
            held = keys_.release_all();
        }
        send_key_actions(held);
        relay_->send_text(R"({"ctrlChannel":"102938","type":"close"})");
    }
    // Still connecting: the worker may wait for the server for a while yet,
    // and there is nothing to tidy up.
    if (!connected_) ExitProcess(0);
    stop();
}

}  // namespace

std::unique_ptr<casement::Session> start_session(Host& host, const casement::Connect& connect) {
    std::string error;
    auto options = parse_options(connect.address, error);
    if (!options) host.finish(State::failed, error);
    std::string user = connect.username.empty() ? options->user : connect.username;
    if (user.empty())
        host.finish(State::failed, "The address has no user name: put the MeshCentral user or login token name in "
                                   "front of the server, like meshcentral://helpdesk@mesh.example.com/Reception-PC.");
    if (connect.password.empty()) host.finish(State::auth_failed, "A password is needed.");
    return std::make_unique<MeshSession>(host, std::move(*options), std::move(user), connect.password);
}

}  // namespace meshcentral
