// End-to-end tests: the real plugin, run through casement-sim, against a
// MeshCentral server and a Linux MeshAgent on an Xvfb display that
// ci/e2e.sh sets up. The tests look at the X display with xev, xdotool and
// xclip, which Wine can start like Windows programs.
//
// ci/e2e.sh writes the setup into a JSON file named by the environment
// variable CASEMENT_MESHCENTRAL_E2E. Without it the tests are skipped,
// unless CASEMENT_E2E=1 asks for them.

#include "check.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include "meshcentral/cursors.h"
#include "sim.h"

using namespace casement::sim;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

struct Box {
    int x = 0, y = 0, width = 0, height = 0;
};

struct Config {
    std::string server;  // host:port
    std::string thumbprint;
    std::string user, password;
    std::string token_user, token_password;
    std::string two_factor_user, two_factor_password;
    std::string device, group, node_id;
    std::string offline_group, offline_node_id;
    std::string state;  // a Unix directory for scratch files
    std::string x_env;  // "DISPLAY=… XAUTHORITY=…"
    std::string xev_log;
    Box xev;
    Box root;  // a part of the screen no window covers
    std::string second_screen;  // the X display name of the agent's display 1
    int second_width = 0, second_height = 0;
    std::string control;  // command prefix: … ensure-agent | stop-agent | continue-agent | restart-server
};

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

/// A Unix path as Wine's Windows programs see it.
std::wstring windows_path(const std::string& unix_path) {
    std::wstring p = L"Z:" + widen(unix_path);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

std::optional<std::string> read_file(const std::string& unix_path) {
    std::ifstream f(std::filesystem::path(windows_path(unix_path)), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string env(const char* name) {
    char value[4096];
    DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n > 0 && n < sizeof(value) ? std::string(value, n) : std::string();
}

/// The test setup, or nothing when the tests should be skipped.
const Config* config() {
    static std::optional<Config> loaded;
    static bool tried = false;
    if (tried) return loaded ? &*loaded : nullptr;
    tried = true;
    std::string path = env("CASEMENT_MESHCENTRAL_E2E");
    bool required = env("CASEMENT_E2E") == "1";
    if (path.empty()) {
        if (required) check::fail(__FILE__, __LINE__, "CASEMENT_MESHCENTRAL_E2E is not set", "(run ci/e2e.sh)");
        else std::fprintf(stderr, "MeshCentral end-to-end tests skipped: CASEMENT_MESHCENTRAL_E2E is not set\n");
        return nullptr;
    }
    auto text = read_file(path);
    json j = text ? json::parse(*text, nullptr, false) : json();
    if (!j.is_object()) {
        check::fail(__FILE__, __LINE__, "can't read the test setup", path);
        return nullptr;
    }
    auto box = [&](const char* key) {
        const json& b = j.at(key);
        return Box{b.at("x").get<int>(), b.at("y").get<int>(), b.at("width").get<int>(), b.at("height").get<int>()};
    };
    Config c;
    c.server = j.at("server");
    c.thumbprint = j.at("thumbprint");
    c.user = j.at("user");
    c.password = j.at("password");
    c.token_user = j.at("token_user");
    c.token_password = j.at("token_password");
    c.two_factor_user = j.at("two_factor_user");
    c.two_factor_password = j.at("two_factor_password");
    c.device = j.at("device");
    c.group = j.at("group");
    c.node_id = j.at("node_id");
    c.offline_group = j.at("offline_group");
    c.offline_node_id = j.at("offline_node_id");
    c.state = j.at("state");
    c.x_env = j.at("x_env");
    c.xev_log = j.at("xev_log");
    c.xev = box("xev");
    c.root = box("root");
    const json& second = j.at("second_screen");
    c.second_screen = second.at("display");
    c.second_width = second.at("width");
    c.second_height = second.at("height");
    c.control = j.at("control");
    loaded = std::move(c);
    return &*loaded;
}


std::string percent_encode(std::string_view s) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

/// A meshcentral:// address, the way Casement assembles it from the fields.
std::string address(const Config& cfg, const std::string& user, const std::string& device, const std::string& group,
                    std::optional<std::string> thumbprint = std::nullopt) {
    std::string a = "meshcentral://" + percent_encode(user) + "@" + cfg.server + "/" + percent_encode(device);
    std::string query;
    if (!group.empty()) query += "group=" + percent_encode(group);
    std::string t = thumbprint ? *thumbprint : cfg.thumbprint;
    if (!t.empty()) query += (query.empty() ? "" : "&") + std::string("thumbprint=") + percent_encode(t);
    return query.empty() ? a : a + "?" + query;
}

/// Runs a shell command on the Linux side and returns its output. Wine
/// starts Unix programs but can't wait for them, so the command leaves a
/// file behind when it's done.
///
/// For a Unix program Wine's CreateProcessW returns no process or thread
/// handle: those fields get whatever its local variables held (Wine 9.0 to
/// 10.0), sometimes the value of a live handle of this process. Closing them
/// closed registry keys, events and pipes, the one carrying the plugin's
/// messages among them, so they are left alone.
std::string shell(const Config& cfg, const std::string& command, std::chrono::seconds timeout = 60s) {
    static int counter = 0;
    std::string out = cfg.state + "/shell-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter);
    std::string script = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; export PATH; (" + command +
                         ") > " + out + " 2>&1; echo $? > " + out + ".done";
    // The script has no double quotes, so it can be quoted as one argument.
    std::wstring line = L"/bin/sh -c \"" + widen(script) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        check::fail(__FILE__, __LINE__, "can't start /bin/sh", command);
        return {};
    }
    auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        if (read_file(out + ".done")) {
            std::string output = read_file(out).value_or("");
            DeleteFileW(windows_path(out).c_str());
            DeleteFileW(windows_path(out + ".done").c_str());
            return output;
        }
        std::this_thread::sleep_for(50ms);
    }
    check::fail(__FILE__, __LINE__, "shell command timed out", command);
    return {};
}

/// The same, with the X display's environment.
std::string x(const Config& cfg, const std::string& command) { return shell(cfg, "env " + cfg.x_env + " " + command); }

/// A shell script (no double quotes) with the X display's environment.
std::string x_script(const Config& cfg, const std::string& script) {
    return shell(cfg, "export " + cfg.x_env + "; " + script);
}

/// Starts the agent again if it stopped: the agent from MeshCentral 1.2.5
/// crashes now and then, and the other tests shouldn't fail for that.
void ensure_agent(const Config& cfg) {
    std::string out = shell(cfg, cfg.control + " ensure-agent", 300s);
    if (!out.empty()) std::fprintf(stderr, "%s", out.c_str());
}

/// Ends the test program, naming the test, when one test runs far longer
/// than any should (restarting a crashed agent included): a hang then fails
/// with the test's name rather than at ctest's limit.
void watch(const char* test) {
    struct State {
        std::mutex lock;
        std::string test;
        Clock::time_point deadline;
    };
    // Never freed: the watching thread may outlive static destructors.
    static State* state = [] {
        auto* s = new State;
        std::thread([s] {
            for (;;) {
                std::this_thread::sleep_for(1s);
                std::lock_guard g(s->lock);
                if (Clock::now() < s->deadline) continue;
                std::fprintf(stderr, "%s has run for over 7 minutes; stopping the tests\n", s->test.c_str());
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }).detach();
        return s;
    }();
    std::lock_guard g(state->lock);
    state->test = test;
    state->deadline = Clock::now() + 420s;
}

#define E2E_CONFIG()              \
    const Config* cfg = config(); \
    if (!cfg) return;             \
    watch(__func__);              \
    ensure_agent(*cfg);

std::wstring plugin_path() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return (std::filesystem::path(self).parent_path() / PLUGIN_EXE).wstring();
}

bool close_to(std::uint32_t bgrx, std::uint32_t rgb, int tolerance) {
    int b = bgrx & 0xFF, g = (bgrx >> 8) & 0xFF, r = (bgrx >> 16) & 0xFF;
    return std::abs(r - static_cast<int>(rgb >> 16)) <= tolerance &&
           std::abs(g - static_cast<int>((rgb >> 8) & 0xFF)) <= tolerance &&
           std::abs(b - static_cast<int>(rgb & 0xFF)) <= tolerance;
}

/// A Casement tab: spawns the plugin, connects, and answers surfaces and
/// frames while waiting, the way Casement does.
class Tab {
public:
    Tab(const std::string& address, const std::string& password, const std::string& username = {}) {
        casement_ = Casement::spawn(plugin_path(), {}, true, {{"CASEMENT_MESHCENTRAL_LOG", "debug"}});
        if (!casement_ || !casement_->hello()) return;
        casement_->connect(address, password, username);
        ok_ = true;
    }

    bool ok() const { return ok_; }

    /// Handles messages until `done` says so; false on the timeout.
    bool pump(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
        auto end = Clock::now() + timeout;
        while (!done()) {
            if (Clock::now() >= end) return false;
            auto m = casement_->recv(100ms);
            if (!m) {
                if (!exited_ && casement_->wait(0ms)) {
                    exited_ = true;
                    exited_at_ = Clock::now();
                }
                // The plugin's last messages can still be in the pipe when
                // it has ended; they get a moment to arrive.
                if (exited_ && !done() && Clock::now() - exited_at_ > 1s) return false;
                continue;
            }
            std::string type = m->value("type", "");
            if (type == "surface_request") {
                auto [s, msg] = casement_->create_surface(++surface_id_, (*m)["width"], (*m)["height"], 32);
                if (!s) return false;
                surface_ = std::move(s);
                casement_->send(msg);
            } else if (type == "frame") {
                ++frames_;
                casement_->send(json{{"type", "frame_ack"}, {"surface", (*m)["surface"]}});
            } else if (type == "state") {
                states_.push_back(*m);
            } else if (type == "cursor") {
                cursors_.push_back(*m);
            } else if (type == "clipboard") {
                clipboards_.push_back((*m)["text"].get<std::string>());
            }
        }
        return true;
    }

    /// The first state the plugin reports.
    std::optional<json> state(std::chrono::milliseconds timeout = 60s) {
        pump([&] { return !states_.empty(); }, timeout);
        if (states_.empty()) return std::nullopt;
        return states_.front();
    }

    std::string state_name(std::chrono::milliseconds timeout = 60s) {
        auto s = state(timeout);
        return s ? s->value("state", "") : "none";
    }

    std::string state_text() const { return states_.empty() ? "" : states_.back().value("text", ""); }

    /// Connected, with a picture of the screen.
    bool showing(std::chrono::milliseconds timeout = 30s) {
        return state_name() == "connected" && pump([&] { return surface_ && frames_ > 0; }, timeout);
    }

    void send(const json& msg) { casement_->send(msg); }
    void pointer(int x, int y, int buttons = 0) { send(json{{"type", "pointer"}, {"x", x}, {"y", y}, {"buttons", buttons}}); }
    void key(int scancode, bool down) { send(json{{"type", "key"}, {"scancode", scancode}, {"down", down}}); }
    void press(int scancode) {
        key(scancode, true);
        key(scancode, false);
    }
    /// Lets the plugin work for a while.
    void idle(std::chrono::milliseconds time) { pump([] { return false; }, time); }

    std::uint32_t pixel(int x, int y) const {
        if (!surface_ || x < 0 || y < 0) return 0;
        return surface_->pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    }
    bool has_surface() const { return surface_ != nullptr; }
    int frames() const { return frames_; }
    std::uint32_t surface_width() const { return surface_ ? surface_->width : 0; }
    std::uint32_t surface_height() const { return surface_ ? surface_->height : 0; }
    bool logged(std::string_view text) const {
        for (const auto& line : casement_->stderr_lines())
            if (line.find(text) != std::string::npos) return true;
        return false;
    }
    const std::vector<json>& cursors() const { return cursors_; }
    const std::vector<std::string>& clipboards() const { return clipboards_; }
    const std::vector<json>& states() const { return states_; }
    std::vector<std::string> log() const { return casement_->stderr_lines(); }

    /// Disconnects the way Casement does; the exit code.
    std::optional<DWORD> disconnect() {
        casement_->send(json{{"type", "disconnect"}});
        casement_->close_stdin();
        return casement_->wait(3000ms);
    }

    std::optional<DWORD> wait(std::chrono::milliseconds timeout) { return casement_->wait(timeout); }

private:
    std::unique_ptr<Casement> casement_;
    std::unique_ptr<HostSurface> surface_;
    std::uint32_t surface_id_ = 0;
    int frames_ = 0;
    bool ok_ = false;
    bool exited_ = false;
    Clock::time_point exited_at_;
    std::vector<json> states_, cursors_;
    std::vector<std::string> clipboards_;
};

/// The KeyPress keysym names and ButtonPress buttons xev logged after
/// `offset` bytes.
struct XevEvents {
    std::vector<std::string> keys;
    std::vector<std::pair<int, std::pair<int, int>>> buttons;  // button, screen position
};

int number(std::string_view text) {
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

XevEvents xev_events(const std::string& log, std::size_t offset) {
    XevEvents e;
    std::istringstream in(log.substr(std::min(offset, log.size())));
    std::string line, kind;
    while (std::getline(in, line)) {
        if (line.starts_with("KeyPress event")) kind = "key";
        else if (line.starts_with("ButtonPress event")) kind = "button";
        else if (!line.empty() && line[0] != ' ') kind.clear();
        if (kind == "key") {
            // "    state 0x0, keycode 38 (keysym 0x61, a), same_screen YES,"
            auto at = line.find("(keysym ");
            if (at == std::string::npos) continue;
            auto comma = line.find(", ", at);
            auto close = line.find(')', comma);
            if (comma != std::string::npos && close != std::string::npos)
                e.keys.push_back(line.substr(comma + 2, close - comma - 2));
            kind.clear();
        } else if (kind == "button") {
            // "    root 0x…, subw 0x0, time …, (328,218), root:(330,220),"
            if (auto at = line.find("root:("); at != std::string::npos) {
                auto comma = line.find(',', at);
                auto close = line.find(')', at);
                if (comma != std::string::npos && close != std::string::npos)
                    e.buttons.push_back({0, {number(line.substr(at + 6, comma - at - 6)),
                                             number(line.substr(comma + 1, close - comma - 1))}});
            }
            // "    state 0x0, button 1, same_screen YES"
            if (auto at = line.find("button "); at != std::string::npos && line.find("state ") != std::string::npos) {
                if (!e.buttons.empty()) e.buttons.back().first = number(line.substr(at + 7, line.find(',', at) - at - 7));
                kind.clear();
            }
        }
    }
    return e;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

/// "on" or "off": the X server's Num Lock.
std::string num_lock(const Config& cfg) {
    std::string q = x(cfg, "xset q");
    auto at = q.find("Num Lock:");
    if (at == std::string::npos) return "unknown";
    std::istringstream in(q.substr(at + 9));
    std::string state;
    in >> state;
    return state;
}

}  // namespace

TEST(e2e_desktop_by_name_and_group) {
    E2E_CONFIG();
    x(*cfg, "xsetroot -solid '#3366cc'");
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    int px = cfg->root.x + cfg->root.width / 2, py = cfg->root.y + cfg->root.height / 2;
    // The tiles are JPEG, so the colours come back close.
    CHECK(tab.pump([&] { return close_to(tab.pixel(px, py), 0x3366CC, 24); }, 20s));
    // A change on the screen arrives as new tiles.
    x(*cfg, "xsetroot -solid '#cc3311'");
    CHECK(tab.pump([&] { return close_to(tab.pixel(px, py), 0xCC3311, 24); }, 20s));
    // The xev window is white.
    CHECK(close_to(tab.pixel(cfg->xev.x + 20, cfg->xev.y + 20), 0xFFFFFF, 24));
    auto code = tab.disconnect();
    REQUIRE(code);
    CHECK_EQ(*code, 0ul);
}

TEST(e2e_display_option) {
    E2E_CONFIG();
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group) + "&display=0", cfg->password);
        REQUIRE(tab.ok());
        CHECK(tab.showing());
        CHECK(!tab.logged("has no display"));
        tab.disconnect();
    }
    {
        // The second X screen is the agent's display 1; the plugin selects it
        // once the agent has listed it.
        x(*cfg, "xsetroot -display " + cfg->second_screen + " -solid '#22aa44'");
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group) + "&display=1", cfg->password);
        REQUIRE(tab.ok());
        CHECK(tab.showing());
        CHECK(tab.pump([&] {
            return tab.surface_width() == static_cast<std::uint32_t>(cfg->second_width) &&
                   tab.surface_height() == static_cast<std::uint32_t>(cfg->second_height) &&
                   close_to(tab.pixel(cfg->second_width / 2, cfg->second_height / 2), 0x22AA44, 24);
        }, 20s));
        tab.disconnect();
    }
    // A display the device doesn't have: the plugin keeps the one the agent
    // shows (asking this Linux agent for it would crash its desktop code).
    for (std::string display : {"5", "all"}) {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group) + "&display=" + display, cfg->password);
        REQUIRE(tab.ok());
        CHECK(tab.showing());
        CHECK(tab.pump([&] { return tab.logged("the device has no display " + display); }, 10s));
        // Still working a moment later.
        tab.idle(2s);
        CHECK_EQ(tab.states().size(), 1u);
        tab.disconnect();
    }
}

TEST(e2e_keyboard) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    // X gives the keyboard to the window under the pointer.
    tab.pointer(cfg->xev.x + cfg->xev.width / 2, cfg->xev.y + cfg->xev.height / 2);
    tab.idle(1500ms);
    std::size_t offset = read_file(cfg->xev_log).value_or("").size();

    tab.press(0x1E);  // a
    tab.key(0x2A, true);  // Shift+b
    tab.press(0x30);
    tab.key(0x2A, false);
    tab.press(0x14B);  // Left (E0)
    tab.press(0x153);  // Delete (E0)
    tab.press(0x11C);  // keypad Enter (E0)
    tab.press(0x3F);   // F5
    tab.key(0x21D, true);  // Pause: E1 1D, then 45
    tab.key(0x45, true);
    tab.key(0x21D, false);
    tab.key(0x45, false);
    // Ctrl+Alt+Del goes to the agent as its own command, not as keys.
    tab.key(0x1D, true);
    tab.key(0x38, true);
    tab.press(0x153);
    tab.key(0x38, false);
    tab.key(0x1D, false);
    tab.press(0x35);  // slash, the last key
    std::string seen;
    tab.pump([&] {
        seen = joined(xev_events(read_file(cfg->xev_log).value_or(""), offset).keys);
        return contains(seen, "slash");
    }, 15s);
    CHECK_EQ(seen, std::string("a Shift_L B Left Delete Return F5 Pause Control_L Alt_L slash"));

    // Num Lock toggles the device's lock state, which the agent reports.
    CHECK_EQ(num_lock(*cfg), std::string("off"));
    auto reports = [&](std::string_view state) {
        int n = 0;
        for (const auto& line : tab.log()) n += contains(line, "keyboard: Num Lock " + std::string(state)) ? 1 : 0;
        return n;
    };
    int on_reports = reports("on");
    tab.press(0x45);
    CHECK(tab.pump([&] { return num_lock(*cfg) == "on"; }, 10s));
    CHECK(tab.pump([&] { return reports("on") > on_reports; }, 10s));
    // With Num Lock on, keypad 7 is a digit on the device.
    offset = read_file(cfg->xev_log).value_or("").size();
    tab.press(0x47);
    tab.pump([&] { return !xev_events(read_file(cfg->xev_log).value_or(""), offset).keys.empty(); }, 10s);
    CHECK_EQ(joined(xev_events(read_file(cfg->xev_log).value_or(""), offset).keys), std::string("KP_7"));
    tab.press(0x45);
    CHECK(tab.pump([&] { return num_lock(*cfg) == "off"; }, 10s));
    tab.disconnect();
}

TEST(e2e_mouse) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    int x0 = cfg->xev.x + 120, y0 = cfg->xev.y + 90;
    tab.pointer(x0, y0);
    std::string where;
    CHECK(tab.pump([&] {
        where = x(*cfg, "xdotool getmouselocation");
        return contains(where, "x:" + std::to_string(x0) + " y:" + std::to_string(y0) + " ");
    }, 10s));
    std::size_t offset = read_file(cfg->xev_log).value_or("").size();
    tab.pointer(x0 + 10, y0 + 10, 1);  // left button, moving
    tab.pointer(x0 + 10, y0 + 10, 0);
    tab.pointer(x0 + 20, y0 + 20, 4);  // right
    tab.pointer(x0 + 20, y0 + 20, 0);
    tab.pointer(x0 + 30, y0 + 30, 2);  // middle
    tab.pointer(x0 + 30, y0 + 30, 0);
    tab.send(json{{"type", "wheel"}, {"delta", 1}});   // away from the user: up
    tab.send(json{{"type", "wheel"}, {"delta", -2}});  // two notches down
    XevEvents events;
    tab.pump([&] {
        events = xev_events(read_file(cfg->xev_log).value_or(""), offset);
        return events.buttons.size() >= 6;
    }, 15s);
    REQUIRE(events.buttons.size() == 6);
    using Press = std::pair<int, std::pair<int, int>>;
    CHECK(events.buttons[0] == (Press{1, {x0 + 10, y0 + 10}}));
    CHECK(events.buttons[1] == (Press{3, {x0 + 20, y0 + 20}}));
    CHECK(events.buttons[2] == (Press{2, {x0 + 30, y0 + 30}}));
    CHECK_EQ(events.buttons[3].first, 4);
    CHECK_EQ(events.buttons[4].first, 5);
    CHECK_EQ(events.buttons[5].first, 5);
    tab.disconnect();
}

TEST(e2e_cursor_shapes) {
    E2E_CONFIG();
    // What the plugin should send for a cursor number from this Linux agent.
    auto expected = [](std::uint8_t index) {
        return meshcentral::cursors::message(meshcentral::cursors::for_index(index, false));
    };
    x(*cfg, "xsetroot -cursor_name left_ptr");
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    // Until the viewer moves the pointer, the agent draws the device's
    // pointer into the picture and asks for no cursor on top of it.
    CHECK(tab.pump([&] { return !tab.cursors().empty(); }, 10s));
    if (!tab.cursors().empty()) CHECK(tab.cursors().front() == expected(14));
    // Once the viewer moves it, the agent reports which cursor shows.
    tab.pointer(cfg->root.x + 10, cfg->root.y + 10);
    tab.pointer(cfg->root.x + 20, cfg->root.y + 20);
    auto summary = [](const json& c) {
        if (c.value("default", false)) return std::string("default");
        return std::format("{}x{} hot {},{} rgba {}", c.value("width", 0), c.value("height", 0), c.value("hot_x", 0),
                           c.value("hot_y", 0), std::hash<std::string>{}(c.value("rgba", std::string())));
    };
    auto shows = [&](std::uint8_t index) {
        json want = expected(index);
        if (tab.pump([&] { return !tab.cursors().empty() && tab.cursors().back() == want; }, 10s)) return true;
        std::fprintf(stderr, "wanted cursor %d: %s; got:\n", index, summary(want).c_str());
        for (const auto& c : tab.cursors()) std::fprintf(stderr, "  %s\n", summary(c).c_str());
        return false;
    };
    // The agent can't name the cursor it finds when it starts, and calls it
    // help; for a Linux agent that is the arrow.
    CHECK(shows(4));
    // Cursors from the cursor theme (ci/e2e.sh uses Adwaita) have names the
    // agent knows.
    x(*cfg, "xsetroot -cursor_name xterm");
    CHECK(shows(5));  // I-beam
    x(*cfg, "xsetroot -cursor_name hand2");
    CHECK(shows(3));  // hand
    x(*cfg, "xsetroot -cursor_name fleur");
    CHECK(shows(7));  // move
    x(*cfg, "xsetroot -cursor_name left_ptr");
    CHECK(shows(0));  // arrow
    CHECK(tab.cursors().back().value("default", false));
    tab.disconnect();
}

TEST(e2e_clipboard_to_device) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    std::string text = "Grüße aus Casement ✓ " + std::to_string(GetTickCount64());
    tab.send(json{{"type", "clipboard"}, {"text", text}});
    std::string got;
    CHECK(tab.pump([&] {
        got = x(*cfg, "xclip -selection clipboard -o");
        return got == text;
    }, 15s));
    CHECK_EQ(got, text);
    // Casement doesn't get its own text back.
    tab.idle(3s);
    for (const auto& c : tab.clipboards()) CHECK(c != text);
    tab.disconnect();
}

TEST(e2e_clipboard_from_device) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    tab.idle(3s);  // the device's earlier clipboard isn't news
    std::string text = "Zwischenablage vom Gerät " + std::to_string(GetTickCount64());
    std::string file = cfg->state + "/clip.txt";
    {
        std::ofstream f(std::filesystem::path(windows_path(file)), std::ios::binary);
        f << text;
    }
    // xclip keeps serving the selection until it is replaced; its PID is
    // kept to stop it afterwards.
    x(*cfg, "xclip -selection clipboard -i -quiet " + file + " < /dev/null > /dev/null 2>&1 & echo $! > " + cfg->state + "/xclip.pid");
    CHECK(tab.pump([&] {
        for (const auto& c : tab.clipboards())
            if (c == text) return true;
        return false;
    }, 15s));
    tab.disconnect();
    shell(*cfg, "kill $(cat " + cfg->state + "/xclip.pid) 2>/dev/null; true");
}

/// A window on the device's screen, from xwininfo -tree: size, position in
/// its parent and on the screen.
struct Window {
    int width = 0, height = 0, x = 0, y = 0, screen_x = 0, screen_y = 0;
};

/// The buttons of the question the agent shows before a session (with
/// xmessage, titled MeshCentral), left to right: No, Yes.
std::vector<Window> consent_buttons(const Config& cfg) {
    std::string tree =
        x_script(cfg, "w=$(xdotool search --name '^MeshCentral$' | head -n 1); test -z $w || xwininfo -tree -id $w");
    // "     0xc0002c (has no name): ()  30x25+4+37  +419+402"
    static const std::regex line(R"((\d+)x(\d+)\+(-?\d+)\+(-?\d+)\s+\+(-?\d+)\+(-?\d+)\s*$)");
    std::vector<Window> buttons;
    std::istringstream in(tree);
    std::string text;
    while (std::getline(in, text)) {
        std::smatch m;
        if (!std::regex_search(text, m, line)) continue;
        Window w{std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]), std::stoi(m[4]), std::stoi(m[5]), std::stoi(m[6])};
        // The buttons sit in a row below the message.
        if (w.x >= 0 && w.y >= 10) buttons.push_back(w);
    }
    std::sort(buttons.begin(), buttons.end(), [](const Window& a, const Window& b) { return a.screen_x < b.screen_x; });
    return buttons;
}

/// Answers the agent's question the way the person at the device would.
bool answer_consent(const Config& cfg, Tab& tab, bool allow) {
    std::vector<Window> buttons;
    tab.pump([&] {
        buttons = consent_buttons(cfg);
        return buttons.size() == 2;
    }, 15s);
    if (buttons.size() != 2) return false;
    const Window& b = buttons[allow ? 1 : 0];
    x_script(cfg, std::format("xdotool mousemove {} {} click 1", b.screen_x + b.width / 2, b.screen_y + b.height / 2));
    return true;
}

TEST(e2e_consent_allowed_and_denied) {
    E2E_CONFIG();
    // The Office group asks the person at the device before a session, for
    // as long as this test runs.
    struct Consent {
        const Config& cfg;
        explicit Consent(const Config& c) : cfg(c) { shell(cfg, cfg.control + " set-consent 8"); }
        ~Consent() { shell(cfg, cfg.control + " set-consent 0"); }
    } consent(*cfg);
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("connected"));
        // The agent asks first; the screen comes once the answer is yes.
        CHECK(tab.pump([&] { return tab.logged("Waiting for user to grant access"); }, 15s));
        CHECK_EQ(tab.frames(), 0);
        REQUIRE(answer_consent(*cfg, tab, true));
        CHECK(tab.showing());
        tab.disconnect();
    }
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("connected"));
        REQUIRE(answer_consent(*cfg, tab, false));
        CHECK(tab.pump([&] { return tab.states().size() >= 2; }, 15s));
        REQUIRE(tab.states().size() >= 2);
        CHECK_EQ(tab.states().back().value("state", ""), std::string("failed"));
        CHECK_EQ(tab.state_text(), "The person at “" + cfg->device + "” didn't allow the session.");
        auto code = tab.wait(5000ms);
        CHECK(code && *code == 1);  // as after every failure
    }
}

TEST(e2e_login_token) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->token_user, cfg->device, cfg->group), cfg->token_password);
    REQUIRE(tab.ok());
    CHECK(tab.showing());
    tab.disconnect();
}

TEST(e2e_wrong_or_missing_password) {
    E2E_CONFIG();
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), "not the password");
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("auth_failed"));
        auto code = tab.wait(5000ms);
        CHECK(code && *code == 0);
    }
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), "");
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("auth_failed"));
        CHECK_EQ(tab.state_text(), std::string("A password is needed."));
    }
    {
        Tab tab(address(*cfg, "nobody-here", cfg->device, cfg->group), "x");
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("auth_failed"));
    }
}

TEST(e2e_two_factor_account) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->two_factor_user, cfg->device, cfg->group), cfg->two_factor_password);
    REQUIRE(tab.ok());
    CHECK_EQ(tab.state_name(), std::string("failed"));
    CHECK(tab.state_text().starts_with("This account uses two-factor authentication; use a login token"));
}

TEST(e2e_device_by_node_id) {
    E2E_CONFIG();
    {
        Tab tab(address(*cfg, cfg->user, cfg->node_id, ""), cfg->password);
        REQUIRE(tab.ok());
        CHECK(tab.showing());
        tab.disconnect();
    }
    // As MeshCentral's device page shows it: without "node//".
    std::string shown = cfg->node_id.substr(cfg->node_id.rfind('/') + 1);
    {
        Tab tab(address(*cfg, cfg->user, shown, ""), cfg->password);
        REQUIRE(tab.ok());
        CHECK(tab.showing());
        tab.disconnect();
    }
}

TEST(e2e_duplicate_names) {
    E2E_CONFIG();
    // The online and the offline agent have the same name in two groups.
    Tab tab(address(*cfg, cfg->user, cfg->device, ""), cfg->password);
    REQUIRE(tab.ok());
    CHECK_EQ(tab.state_name(), std::string("failed"));
    std::string text = tab.state_text();
    CHECK(contains(text, "There are 2 devices named “" + cfg->device + "”"));
    CHECK(contains(text, cfg->node_id));
    CHECK(contains(text, cfg->offline_node_id));
}

TEST(e2e_offline_device) {
    E2E_CONFIG();
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->offline_group), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("failed"));
        CHECK(contains(tab.state_text(), "is offline"));
    }
    {
        Tab tab(address(*cfg, cfg->user, cfg->offline_node_id, ""), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("failed"));
        CHECK(contains(tab.state_text(), "is offline"));
    }
}

TEST(e2e_unknown_device_or_group) {
    E2E_CONFIG();
    {
        Tab tab(address(*cfg, cfg->user, "no-such-device", ""), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("failed"));
        CHECK_EQ(tab.state_text(), std::string("There's no device named “no-such-device” that this account can see."));
    }
    {
        Tab tab(address(*cfg, cfg->user, cfg->device, "No Such Group"), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("failed"));
        CHECK(contains(tab.state_text(), "no device group named “No Such Group”"));
    }
}

TEST(e2e_certificate_checks) {
    E2E_CONFIG();
    {
        // No fingerprint: Windows doesn't know the server's own authority.
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group, ""), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("failed"));
        CHECK(contains(tab.state_text(), "isn't trusted by Windows"));
        CHECK(contains(tab.state_text(), "The server presented " + cfg->thumbprint));
    }
    {
        std::string wrong = "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group, wrong), cfg->password);
        REQUIRE(tab.ok());
        CHECK_EQ(tab.state_name(), std::string("failed"));
        CHECK(contains(tab.state_text(), "doesn't match the Certificate SHA-256 field"));
        CHECK(contains(tab.state_text(), "the server presented " + cfg->thumbprint));
    }
    {
        // Written the way Windows shows certificate hashes.
        std::string spaced;
        for (char c : cfg->thumbprint) spaced += c == ':' ? ' ' : static_cast<char>(std::tolower(c));
        Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group, spaced), cfg->password);
        REQUIRE(tab.ok());
        CHECK(tab.showing());
        tab.disconnect();
    }
}

TEST(e2e_agent_not_answering) {
    E2E_CONFIG();
    // A stopped agent still looks connected to the server, but never joins
    // the relay.
    struct Stopped {
        const Config& cfg;
        explicit Stopped(const Config& c) : cfg(c) { shell(cfg, cfg.control + " stop-agent"); }
        ~Stopped() {
            shell(cfg, cfg.control + " continue-agent");
            std::this_thread::sleep_for(3s);  // time to catch up with the server
        }
    } stopped(*cfg);
    auto start = Clock::now();
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    std::string state = tab.state_name(60s);
    auto took = Clock::now() - start;
    CHECK_EQ(state, std::string("failed"));
    CHECK(contains(tab.state_text(), "didn't answer"));
    CHECK(took >= 10s);
    CHECK(took <= 45s);
}

TEST(e2e_server_restart_closes_the_tab) {
    E2E_CONFIG();
    Tab tab(address(*cfg, cfg->user, cfg->device, cfg->group), cfg->password);
    REQUIRE(tab.ok());
    REQUIRE(tab.showing());
    shell(*cfg, cfg->control + " restart-server", 120s);
    CHECK(tab.pump([&] { return tab.states().size() >= 2; }, 30s));
    REQUIRE(tab.states().size() >= 2);
    CHECK_EQ(tab.states().back().value("state", ""), std::string("closed"));
    std::string text = tab.state_text();
    CHECK(text == "The connection to the MeshCentral server was lost." ||
          text == "“" + cfg->device + "” or the MeshCentral server ended the session.");
    auto code = tab.wait(5000ms);
    CHECK(code && *code == 0);
}

CHECK_MAIN
