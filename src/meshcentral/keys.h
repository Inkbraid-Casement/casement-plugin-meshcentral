// Casement sends PC set-1 scancodes (0x100 added for E0 keys); MeshCentral
// agents take Windows virtual-key codes plus an extended-key flag. The table
// is the US layout's, written out so the result doesn't depend on the
// keyboard layouts installed where the plugin runs. The remote side applies
// its own layout to the virtual keys, as it does for MeshCentral's own
// viewers.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace meshcentral::keys {

struct VirtualKey {
    std::uint8_t vk = 0;
    bool extended = false;
};

/// What the agent's keyboard expects.
struct Target {
    /// Windows agents inject keys with SendInput and take the extended flag
    /// and the left/right modifier codes. Other agents map the code to their
    /// own key symbols and only know the generic modifiers.
    bool windows = true;
    /// The remote Num Lock: on Windows the keypad keys must be sent as digits
    /// or as navigation keys accordingly.
    bool num_lock = true;
};

/// The virtual key for a scancode, if the key has one.
std::optional<VirtualKey> virtual_key(std::uint32_t scancode, const Target& target);

/// One thing to send to the agent.
struct Action {
    enum class Kind { key, ctrl_alt_del } kind = Kind::key;
    VirtualKey key;
    bool down = false;
};

/// Turns Casement's key events into agent actions, keeping the state that
/// takes: Pause arrives as two scancodes, and Ctrl+Alt+Del is sent as the
/// agent's own command because Windows doesn't accept it as injected keys.
class Translator {
public:
    std::vector<Action> key(std::uint32_t scancode, bool down);
    /// Key-up actions for every key still held down.
    std::vector<Action> release_all();

    Target target;

private:
    bool held(std::uint32_t scancode) const { return held_.count(scancode) != 0; }

    std::set<std::uint32_t> held_;
    /// What each held key was sent as, so its release matches even if the
    /// mapping changed meanwhile (Num Lock).
    std::map<std::uint32_t, VirtualKey> sent_;
    bool pause_prefix_ = false;
    /// Del went to the agent as Ctrl+Alt+Del: swallow its release.
    bool swallow_delete_up_ = false;
};

}  // namespace meshcentral::keys
