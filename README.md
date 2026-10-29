# MeshCentral support for Casement

A protocol plugin for Casement, the Windows remote desktop client by
Inkbraid. It opens the desktops of computers that run the MeshCentral agent,
through the user's own MeshCentral server, in a Casement tab: the screen,
keyboard and mouse, the shape of the remote cursor, and clipboard text in
both directions.

This is an independent project. It is not made or endorsed by the
MeshCentral project; "MeshCentral" names the software it works with.

## Status

| | |
|---|---|
| Login with a password or a login token; server certificate checked by Windows or against a SHA-256 fingerprint | done |
| Device by name (within a device group if one is given) or by node id; clear failures for unknown, ambiguous and offline devices | done |
| Remote desktop: JPEG tiles, quality and frame rate settings, choice of display | done |
| Keyboard, Ctrl+Alt+Del, mouse buttons and wheel, cursor shapes | done |
| Clipboard text both ways | done |
| Asking the person at the device for consent (done by the agent) | works; the waiting message only goes to the log (needs protocol v2, P1) |
| Accounts with two-factor authentication | through a login token; a prompt for the code needs protocol v2 (P2) |
| Display picker; reading the clipboard only while the tab has focus | needs protocol v2 (P11, P4) |
| Terminal and file sessions, Intel AMT, WebRTC | not yet |

See [CHANGELOG.md](CHANGELOG.md).

## Installing

Download `casement-meshcentral-v<version>-windows-x64.zip` from this
repository's [Releases](https://github.com/Inkbraid-Casement/casement-plugin-meshcentral/releases)
and unpack it into `%LOCALAPPDATA%\Casement\plugins\`, so that you get

```
%LOCALAPPDATA%\Casement\plugins\
    meshcentral\
        plugin.json
        icon.png
        casement-meshcentral.exe
        LICENSE, README.md, CHANGELOG.md
```

then restart Casement. The program is a single 64-bit executable for
Windows 10 or later; everything else it uses (WinHTTP, WIC) is part of
Windows. Each release also carries the matching source code.

## Connections

Casement offers a "MeshCentral device" connection with these fields:

| Field | In the address | |
|---|---|---|
| User or login token name | user | The MeshCentral account, or a login token's user name (`~t:…`). Required. |
| Server | host | The server's host name or IP address, as in the browser. Required. |
| Port | port | The HTTPS port; 443 if empty. |
| Device name or node id | path | The device's name as MeshCentral shows it, or its node id. Required. |
| Device group | `group` | Look for the device only in this group: its name, or its id (`mesh//…`). |
| Display | `display` | Which display to show: its number, or `all`. |
| JPEG quality | `quality` | 1 to 100; 50 if empty. |
| Max frames per second | `fps` | 1 to 50; 10 if empty. |
| Certificate SHA-256 | `thumbprint` | The server certificate's SHA-256 fingerprint, for a server whose certificate Windows doesn't trust (see [Certificates](#certificates)). |

The password comes from Casement, saved or typed when the plugin reports
that one is needed; the plugin doesn't store it.

The address Casement builds from the fields:

```
meshcentral://<user>@<server>[:<port>]/<device>[?group=…&display=…&quality=…&fps=…&thumbprint=…]
```

For example:

```
meshcentral://helpdesk@mesh.example.com/Reception-PC?group=Office
meshcentral://helpdesk@mesh.example.com/node//n6UakAar$uxFZ3Rk28vdIeNaVUFnbeRRtqQkK$9vuJVHLNcfZcmI0YTCPbY3X5Xy
meshcentral://~t:Kx3pVnq9Tz2LsWbE@10.0.0.5:8443/Lab-PC?thumbprint=28:DF:0A:9C:…:46:96&quality=70&fps=15
```

- **Device names** are compared exactly first, then ignoring case. When
  several devices the account can see have the name, the connection fails
  with a list of their node ids and groups; add the group, or use the node
  id.
- **Node ids** are written `node//<id>`, as MeshCentral's own messages name
  them. The "Node ID" that MeshCentral's device page shows (the part after
  the last slash) is accepted as well.
- **Displays**: Windows agents number their monitors 1, 2, … and offer
  `all` (65535) when there is more than one; Linux agents number X screens
  from 0. The plugin picks the display from the list the agent sends when
  the session starts. A display the device doesn't have is ignored, with a
  note in the log, and the agent's own choice stays.
- **Fingerprints** are 64 hexadecimal digits, with or without colons,
  spaces or dashes, in either case. A 40-digit SHA-1 thumbprint is refused
  with a note saying so.

What Casement sees as `state`:

| | |
|---|---|
| `connected` | the agent has joined the session; the picture follows (after the person at the device agreed, if the device group asks them) |
| `auth_failed` | no password, a wrong user name or password, an expired login |
| `failed` | a sentence saying what went wrong: the certificate, the server, two-factor authentication, an unknown, ambiguous or offline device, an agent that didn't answer, consent refused, … |
| `closed` | the device or the server ended a working session, or the connection was lost |

## How it works

MeshCentral's protocol is the one its own web pages speak; the plugin
follows the server's `control.ashx` and `meshrelay.ashx` handlers and the
web client's `agent-redir-ws` and `agent-desktop` scripts.

1. **Login.** A WebSocket to `wss://<server>:<port>/control.ashx`, opened
   with the header `x-meshauth: *`: MeshCentral then waits for the
   credentials inside the WebSocket, so the plugin sends them (`userAuth`)
   only after the TLS handshake and the certificate check. The server
   answers with `serverinfo`, or closes the connection with a reason, which
   becomes the failure text.
2. **The device.** The plugin asks for the device groups (`meshes`), the
   devices (`nodes`) and a pair of relay cookies (`authcookie`), then looks
   the device up. A device whose agent isn't connected fails at once.
3. **The relay.** It opens `meshrelay.ashx` for a remote desktop (protocol
   2) under a random tunnel id and asks the server, over the control
   channel, to send the agent to the same tunnel. When both have joined, the
   relay says `c`; the plugin answers `2` and sends the compression settings
   (JPEG, the quality, full scale, a frame timer of 1000 / fps ms). If the
   agent doesn't join, the connection fails after at most 30 seconds.
4. **The picture.** The agent sends the screen size and JPEG tiles. Tiles
   are decoded with WIC into a copy of the screen as they arrive. Whenever
   Casement has acknowledged the previous frame, the changes since then go
   out as one frame of at most 16 rectangles. Tiles that arrive while
   Casement is still busy with a frame only update the copy, so a slow
   display costs frames rather than delay; nothing queues up in the plugin.
5. **Input.** Keys become Windows virtual-key codes, as MeshCentral's
   protocol carries them; mouse events carry absolute screen positions and
   button changes; the wheel is sent at the pointer.
6. **The cursor.** Agents report which standard cursor shows (arrow, I-beam,
   hand, resize shapes, …). The plugin shows the matching Windows cursor,
   rendered to an image once per shape.
7. **The clipboard.** When Casement's clipboard changes, the text goes to
   the device (`setclip`). The device's clipboard is read every 2 seconds
   (`getclip`, marked so that the agent doesn't add each read to the
   server's event log), and new text goes to Casement.
8. **Disconnecting** releases any keys still held down, sends the relay's
   close message and closes both WebSockets.

`ping` messages from the server and the relay are answered. Messages from
the agent (consent requests, errors) and the device's Num, Caps and Scroll
Lock state go to the log.

## Things to know

### Tested versions

The plugin was tested with **MeshCentral 1.2.5** (from npm) and the Linux
x86-64 agent that comes with it (MeshAgent 2.6.0 of February 2026), on an
X server without a desktop environment, with the plugin running under Wine.
It was not tested with Windows or macOS agents, on Windows itself, behind a
proxy, over IPv6 or over a slow link; see [Testing](#testing).

MeshCentral's protocol is undocumented and may change. The server's version
is only shown to administrators, so the plugin can't check it; it does check
that the server answers the login the way MeshCentral does. MeshCentral
1.3.0 (released in October 2026) hasn't been tested yet.

### On the server

- MeshCentral must serve its site at the root of the address. Domains that
  MeshCentral serves under a path (`https://mesh.example.com/team/`) aren't
  supported; a domain with a host name of its own should work but hasn't
  been tested.
- The account needs remote desktop rights on the device. An account with
  MeshCentral's "No Tools (MeshCmd/Router)" restriction is refused, with a
  sentence saying so.
- **Two-factor authentication**: the plugin can't ask for a code, so such an
  account fails with a sentence suggesting a **login token**. MeshCentral
  makes one on its My Account page ("Create login token"); its user name
  (`~t:…`) and password go into the connection. Login tokens work unless the
  server turns them off (`passwordRequirements.logintokens` set to `false`).
- **User consent**: when the device group asks the person at the device to
  allow remote desktop sessions, the tab stays without a picture until they
  answer. If they refuse, or nobody answers in time (30 seconds unless the
  server says otherwise), the connection fails with a sentence saying which.
- A session the server records is logged as such.

### Certificates

Without a fingerprint, Windows checks the server's certificate as it would
for any HTTPS site: it must lead to a trusted certification authority and be
for the server's name. MeshCentral's own certificate, which it makes when it
first starts, is not, so for such a server the connection fails with a
sentence that names the fingerprint the server presented.

With a fingerprint in the **Certificate SHA-256** field, the plugin accepts
exactly that certificate, whoever issued it, and no other. If the server's
certificate changes, the connection fails with the new fingerprint. Check a
fingerprint before you put it in the field, for example on the server, in
MeshCentral's data folder:

```
openssl x509 -in meshcentral-data/webserver-cert-public.crt -noout -fingerprint -sha256
```

(Behind a reverse proxy that handles TLS, it is the proxy's certificate
that counts.)

The user name and password are sent only inside the TLS connection, once
the certificate check has passed, and only to the control channel; the
relay is opened with the short-lived cookie the server hands out after the
login.

### Keyboard, mouse and cursor

- Keys are sent as Windows virtual-key codes for the US layout, as
  MeshCentral's web client does; the device's own keyboard layout then
  decides which character comes out. Windows agents get the extended-key
  flag for the keys that have it.
- Keypad keys follow the device's Num Lock, which the agent reports.
- AltGr is sent as the right Alt key (with whatever Ctrl key Casement
  passes along); Windows devices take Ctrl+Alt as AltGr. Characters typed
  with AltGr haven't been tested.
- **Ctrl+Alt+Del**: pressing Delete while Ctrl and Alt are held sends
  MeshCentral's Ctrl+Alt+Del command instead of the keys. Windows agents
  that run as a service pass it to Windows; Linux agents ignore it.
- **Wheel**: one notch is sent as 120 units, the way Windows counts it.
  (MeshCentral's web client sends 360, three notches' worth on Windows.)
  Linux agents scroll one step per notch.
- **Cursor**: until the mouse moves in the tab, agents draw the device's
  pointer into the picture and Casement shows no cursor of its own; then the
  agent reports cursor shapes and Casement shows them. Linux agents know a
  shape by its name in the X cursor theme; shapes they can't name show as
  the arrow. When the pointer moves at the device itself, agents draw it
  into the picture again.

### Clipboard

- Text only.
- The device's clipboard is read every 2 seconds while the session is
  connected. (MeshCentral's web client reads it every second when its
  automatic clipboard is on.) What it held when the session began
  isn't copied to Casement, only later changes. Line ends that differ only
  in CR LF and LF don't count as a change.
- Linux agents read and set the clipboard with `xclip` when it is installed
  on the device.

### Agent problems seen in testing

The Linux agent that comes with MeshCentral 1.2.5 crashed in testing in
two situations:

- Its desktop process crashes when asked for a display that doesn't
  exist. The plugin therefore only asks for displays from the agent's own
  list.
- Its desktop process crashes when the X server doesn't hand over the
  cursor image, which it doesn't check for (still so in MeshAgent's code
  of October 2026). The test X server (Xvfb, without a desktop environment)
  got into that state whenever the root window's cursor had changed during
  a desktop session that then ended; setting the root cursor again cleared
  it. The end-to-end tests do that before each test.

After its desktop process had crashed, the agent itself sometimes crashed
when the session ended or the next one began. An agent installed as a
service is then started again by systemd (`Restart=on-failure`).

## Building

Only Windows' own libraries and the vendored nlohmann/json are needed.

With MSVC (Visual Studio 2022, from a Developer Command Prompt for x64):

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -L unit --output-on-failure
```

With MinGW-w64 on Linux (the tests then run under Wine):

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build
WINEDEBUG=-all ctest --test-dir build -L unit --output-on-failure
```

The result is `build/casement-meshcentral.exe`, built for the Windows GUI
subsystem, so starting it never opens a console window.

The code is in `src/meshcentral/`: `session.cpp` (the connection and the
message mapping), `control.cpp` (control channel messages and device
lookup), `websocket.cpp` (WinHTTP WebSockets and the certificate check),
`desktop.cpp` (desktop commands), `tiles.cpp` (WIC decoding),
`keys.cpp`, `cursors.cpp`, `clipboard.cpp` and `options.cpp`. `src/casement/`,
`tools/sim/`, `tests/check.h` and `cmake/` are the shared plugin layer from
casement-plugin-common.

## Testing

**Unit tests** (`tests/unit`, 47 tests, label `unit`): address and option
parsing, desktop command encoding and decoding (jumbo commands and split or
malformed input included), the key table and the key translator (Pause,
Ctrl+Alt+Del, Num Lock), wheel encoding, tile decoding with WIC and
compositing, cursor rendering, control channel messages, login refusals and
device lookup, and the clipboard logic.

**End-to-end tests** (`tests/e2e`, 18 tests, label `e2e`) run the real
plugin through `tools/sim` against a real MeshCentral server and a real
Linux agent. `ci/e2e.sh` sets them up and runs them:

```
ci/e2e.sh build            # set up, run the e2e tests, stop everything
ci/e2e.sh build --keep     # leave the server and agent running afterwards
ci/e2e.sh stop             # stop what --keep left running
```

It installs MeshCentral 1.2.5 with npm into `work/` next to the repository
(`CASEMENT_E2E_DIR` to change that), starts it on `127.0.0.1:10443` with the
certificate it makes for itself, and creates an account with a password,
one with two-factor authentication, a login token, two device groups and
two agents with the same name: one connected, on the Xvfb display `:11`,
which has two screens and an `xev` window that records input; the other
registered and stopped.
The tests then check:

- the picture (root window colours set with `xsetroot`, before and after a
  change), with the device found by name and group, by node id in both
  forms, and through a login token;
- keys, Shift, extended keys, Pause, Ctrl+Alt+Del (not delivered as keys),
  Num Lock and its report, keypad digits, all as `xev` sees them;
- mouse position, the three buttons and the wheel, as `xev` and `xdotool`
  see them;
- cursor messages: hidden at first, then the shapes the agent reports for
  `xsetroot -cursor_name` with the Adwaita theme, each compared with the
  image the plugin renders for it;
- clipboard text to and from the device, with non-ASCII text, through
  `xclip`;
- consent asked on the device (the agent's `xmessage` question, answered
  with `xdotool`): allowed, then refused;
- the `display` field: the agent's second display (Xvfb's second screen,
  with its own size and colour), the display it shows anyway, and two it
  doesn't have;
- a wrong password, no password, an unknown user, a two-factor account, an
  unknown device and group, two devices of the same name, an offline device
  by name and by node id, an agent that doesn't answer (stopped with
  SIGSTOP), no fingerprint, a wrong one and one written with spaces, and a
  server restart during a session.

It needs Linux with a MinGW build and Wine, Node.js 20 or later with npm,
`Xvfb`, `xauth`, x11-utils (`xev`, `xdpyinfo`, `xmessage`),
x11-xserver-utils (`xsetroot`, `xset`), `xdotool`, `xclip`, the Adwaita
cursor theme (adwaita-icon-theme), `curl`, `openssl`, and root or
password-less `sudo`: the agent runs as root. On hosts without systemd it
gives the agent a `loginctl` that reports no sessions, without which the
agent's clipboard code stops. The script stops only the processes it
started, by the PIDs it recorded. Before each test it sets the root
window's cursor again and restarts the agent if it crashed (see
[Agent problems seen in testing](#agent-problems-seen-in-testing)); crashes
are listed at the end. A test that runs for over 7 minutes stops the run,
with its name.

The CI workflow runs the unit tests on Windows with MSVC, and the unit and
end-to-end tests on Ubuntu 24.04 with MinGW and Wine.

**Trying it by hand.** `casement-sim` plays Casement's side: it answers
surface requests, acknowledges frames, prints the other messages and saves
the last picture as a PPM file.

```
CASEMENT_MESHCENTRAL_LOG=debug wine build/casement-sim.exe build/casement-meshcentral.exe \
    "meshcentral://helpdesk@mesh.example.com/Reception-PC?group=Office" --password … \
    --seconds 20 --click 600,400 --type 0x1E,0x30 --clipboard "hello" --out .
```

The plugin logs to stderr, which Casement copies into its log.
`CASEMENT_MESHCENTRAL_LOG` sets the level: `error`, `warn`, `info` (the
default), `debug` or `trace`.

## Protocol v2

These proposals (numbered as in the plugin specs) would remove the v1
workarounds:

- **P2 (prompts)**: ask for the two-factor code instead of requiring a
  login token, and let the user confirm a certificate Windows doesn't trust
  instead of filling in the fingerprint field.
- **P1 (progress text)**: show "Waiting for user to grant access…" and
  similar agent messages in the tab while the picture hasn't come.
- **P4 (focus)**: read the device's clipboard only while the tab has focus.
- **P11 (displays)**: a display picker filled from the agent's list,
  instead of the `display` field.
- **P7 (terminal view)**: MeshCentral terminal sessions (relay protocol 1).

The device's lock key state is only logged; no proposal covers showing it.

## Licensing

The plugin is licensed under the **Apache License 2.0** ([LICENSE](LICENSE)).

- `third_party/nlohmann/`: [nlohmann/json](https://github.com/nlohmann/json)
  3.12.0, MIT licence (`third_party/nlohmann/LICENSE.MIT`).
- `src/casement/`, `tools/sim/`, `tests/check.h`, `cmake/` and
  `ci/mingw-deps.sh` are Inkbraid's shared plugin layer
  (casement-plugin-common), under this repository's licence.
- [docs/plugin-protocol.md](docs/plugin-protocol.md) is a copy of the plugin
  protocol from
  [Inkbraid-Casement/casement-plugin-protocol](https://github.com/Inkbraid-Casement/casement-plugin-protocol),
  licensed CC BY 4.0.
- WinHTTP, WIC, BCrypt, CryptoAPI and GDI are Windows components, used
  through the Windows SDK.
- MeshCentral and MeshAgent (Apache-2.0) are not part of the plugin, which
  implements their protocol on its own; the end-to-end tests download
  MeshCentral from npm, and its agent from the test server, when they run.
- The icon is the project's own artwork (`tools/make-icon.py`).
