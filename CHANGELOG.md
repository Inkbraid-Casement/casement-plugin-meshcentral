# Changelog

## 0.1.0 (2026-10-23)

- `meshcentral://` connections to the desktops of MeshCentral agents through
  the user's own MeshCentral server, on the Casement plugin protocol v1
  (capabilities `password`, `clipboard`, `cursor`).
- Login on the control channel with `x-meshauth`, credentials sent inside
  the WebSocket after the certificate check; passwords and login tokens;
  `auth_failed` for a missing or wrong password, `failed` with a sentence
  for two-factor accounts (use a login token), the "No Tools" restriction,
  email validation and other refusals.
- Server certificates checked by Windows, or pinned with a SHA-256
  fingerprint (the Certificate SHA-256 field); a failure names the
  fingerprint the server presented.
- Devices by name (exact, then ignoring case), within a device group by
  name or id, or by node id (with or without `node//`); failures for
  unknown, ambiguous and offline devices and for agents that don't answer
  within 30 seconds.
- Relay set-up (tunnel request, `c`/`cr`, protocol `2`), compression
  settings from the `quality` and `fps` fields, display choice from the
  agent's list (`display` field).
- JPEG tiles decoded with WIC into a shadow buffer and sent as frames of at
  most 16 rectangles when Casement is ready; jumbo commands.
- Keyboard as US-layout Windows virtual keys with extended-key actions for
  Windows agents, Pause, Num Lock tracking from the agent's KEYSTATE,
  Ctrl+Alt+Del as MeshCentral's command, release of held keys on
  disconnect; mouse buttons, wheel (120 per notch; one message per notch
  for other agents than Windows).
- Cursor shapes as Windows system cursors rendered to RGBA; a hidden cursor
  while the agent draws the pointer into the picture.
- Clipboard text both ways: `setclip` on changes in Casement, `getclip`
  every 2 seconds while connected.
- Consent requests and refusals from the agent: logged, and refusals end
  the connection with a sentence saying so. `ping`/`pong` on both channels.
- `plugin.json`, icon (`tools/make-icon.py`), the protocol document, unit
  tests, end-to-end tests against MeshCentral 1.2.5 and its Linux agent
  (`ci/e2e.sh`), GitHub Actions (Windows build and unit tests with MSVC,
  MinGW build with unit and end-to-end tests under Wine, releases from
  tags).
