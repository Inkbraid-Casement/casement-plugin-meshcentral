#!/usr/bin/env bash
# End-to-end tests against a real MeshCentral server and a real Linux
# MeshAgent.
#
#   ci/e2e.sh <build-dir> [--keep]    set everything up, run the e2e tests,
#                                     stop what was started (unless --keep)
#   ci/e2e.sh stop                    stop what a --keep run left running
#   ci/e2e.sh control <state-dir> ensure-agent|stop-agent|continue-agent|restart-server
#   ci/e2e.sh control <state-dir> set-consent <flags>
#                                     used by the tests
#
# It installs MeshCentral (the pinned version below) with npm, starts it on
# 127.0.0.1:10443 with the certificate it makes for itself, creates the
# accounts the tests use (a plain one, one with two-factor authentication,
# a login token), two device groups and two agents: one runs on the Xvfb
# display :11, which has two screens and an xev window the tests watch, the
# other is registered and stopped, so that it is offline. Both get the same
# device name.
#
# CASEMENT_E2E_DIR (default: work/ next to the repository) holds the
# MeshCentral installation and the state of the last run. Processes are
# stopped by the PIDs recorded there; nothing else is touched. Xvfb and the
# agents run as root (through sudo when needed): an agent running as root
# finds an X display that is owned by root and has an -auth file.
set -euo pipefail

MESHCENTRAL_VERSION=1.2.5
PORT=10443
DISPLAY_NUMBER=11

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${CASEMENT_E2E_DIR:-$(cd "$REPO/.." && pwd)/work}
MC=$WORK/meshcentral
SUDO=
if [ "$(id -u)" != 0 ]; then SUDO="sudo -n"; fi

log() { echo "e2e: $*" >&2; }
die() { log "$*"; exit 1; }

# --- Processes ----------------------------------------------------------

# Whether process $1 runs (zombies don't count).
running() {
    local stat
    stat=$(ps -o stat= -p "$1" 2> /dev/null || true)
    [ -n "$stat" ] && [ "${stat#Z}" = "$stat" ]
}

# Whether any process of group $1 runs.
group_running() {
    ps -eo pgid=,stat= | awk -v g="$1" '$1 == g && $2 !~ /^Z/ { found = 1 } END { exit !found }'
}

# Stops, as root, the agent whose PID is in file $1, and what it started:
# it leads its own process group, and the xclip it starts for the
# clipboard outlives it otherwise. xclip blocks SIGTERM there.
stop_agent() {
    local file=$1 pid
    [ -f "$file" ] || return 0
    pid=$(cat "$file")
    rm -f "$file"
    [ -n "$pid" ] || return 0
    $SUDO kill -CONT -- "-$pid" 2>/dev/null || true
    $SUDO kill -- "-$pid" 2>/dev/null || true
    for _ in $(seq 50); do
        group_running "$pid" || return 0
        sleep 0.1
    done
    $SUDO kill -9 -- "-$pid" 2>/dev/null || true
}

# Stops the process whose PID is in file $1 (as root when $2 is "root").
stop_pid() {
    local file=$1 as=${2:-} pid
    [ -f "$file" ] || return 0
    pid=$(cat "$file")
    rm -f "$file"
    [ -n "$pid" ] || return 0
    if [ "$as" = root ]; then
        $SUDO kill -CONT "$pid" 2>/dev/null || true
        $SUDO kill "$pid" 2>/dev/null || true
    else
        kill "$pid" 2>/dev/null || true
    fi
    for _ in $(seq 50); do
        running "$pid" || return 0
        sleep 0.1
    done
    if [ "$as" = root ]; then $SUDO kill -9 "$pid" 2>/dev/null || true; else kill -9 "$pid" 2>/dev/null || true; fi
}

# The server runs in its own process group (MeshCentral starts a child).
stop_server() {
    local state=$1 pid
    [ -f "$state/server.pid" ] || return 0
    pid=$(cat "$state/server.pid")
    rm -f "$state/server.pid"
    kill -- "-$pid" 2>/dev/null || true
    for _ in $(seq 100); do
        group_running "$pid" || return 0
        sleep 0.1
    done
    kill -9 -- "-$pid" 2>/dev/null || true
}

stop_all() {
    local state=$1
    [ -d "$state" ] || return 0
    stop_pid "$state/xev.pid"
    stop_pid "$state/xclip.pid"
    stop_agent "$state/agent-office/agent.pid"
    stop_agent "$state/agent-lab/agent.pid"
    stop_pid "$state/xvfb.pid" root
    stop_server "$state"
}

start_server() {
    local state=$1
    # setsid execs node, so $! is the server, and its process group holds
    # MeshCentral's child processes too.
    setsid node "$MC/node_modules/meshcentral" --datapath "$state/data" >> "$state/server.log" 2>&1 < /dev/null &
    echo $! > "$state/server.pid"
    for _ in $(seq 120); do
        if curl -sk --max-time 2 "https://127.0.0.1:$PORT/health.ashx" 2>/dev/null | grep -q ok; then return 0; fi
        sleep 1
    done
    tail -n 30 "$state/server.log" >&2
    die "MeshCentral didn't start"
}

# --- MeshCentral and agents ----------------------------------------------------

meshctrl() {
    node "$MC/node_modules/meshcentral/meshctrl.js" --url "wss://127.0.0.1:$PORT" --loginuser admin \
        --loginpass "$ADMIN_PASSWORD" "$@"
}

# The node id of the device in group $1 whose agent is connected (or not,
# with $2 = 0).
device_id() {
    meshctrl listdevices --json 2>/dev/null | node -e '
        let s = ""; process.stdin.on("data", (d) => s += d).on("end", () => {
            const list = JSON.parse(s).filter((d) => d.meshid === process.argv[1] && ((d.conn & 1) ? 1 : 0) == process.argv[2]);
            if (list.length) console.log(list[0]._id);
        });' "$1" "${2:-1}" || true
}

wait_device() {  # group online
    local id
    for _ in $(seq 120); do
        id=$(device_id "$1" "$2")
        if [ -n "$id" ]; then echo "$id"; return 0; fi
        sleep 1
    done
    die "the agent in $1 didn't show up"
}

# Starts the agent installed in directory $1 (as root, in a session of its
# own); its run.sh writes its PID to agent.pid.
launch_agent() {
    rm -f "$1/agent.pid"
    $SUDO setsid "$1/run.sh" >> "$1/agent.log" 2>&1 < /dev/null &
    for _ in $(seq 50); do
        [ -s "$1/agent.pid" ] && return 0
        sleep 0.1
    done
    die "the agent in $1 didn't start"
}

if [ "${1:-}" = control ]; then
    STATE=$2
    # ADMIN_PASSWORD and OFFICE, from the setup below.
    . "$STATE/control.env"
    office=$STATE/agent-office
    case "${3:-}" in
        ensure-agent)
            # Run before each test. The agent that comes with MeshCentral
            # 1.2.5 crashes now and then; the tests after such a crash get
            # a new one.
            if ! { [ -f "$office/agent.pid" ] && running "$(cat "$office/agent.pid")"; }; then
                log "the agent in Office isn't running any more; starting it again"
                echo "$(date '+%F %T') the agent in Office was restarted" >> "$STATE/agent-restarts.log"
                stop_agent "$office/agent.pid"
                wait_device "$OFFICE" 0 > /dev/null
                launch_agent "$office"
                wait_device "$OFFICE" 1 > /dev/null
            fi
            # When the root window's cursor changed during a session, Xvfb
            # stops reporting the cursor image once the agent's desktop
            # process for that session has gone, and the next desktop
            # process crashes on that. Setting the cursor again clears it,
            # so each test starts with the arrow, after the last session's
            # desktop process (a child of the agent) has ended.
            agent=$(cat "$office/agent.pid")
            for _ in $(seq 100); do
                ps -o stat=,comm= --ppid "$agent" 2> /dev/null |
                    awk '$1 !~ /^Z/ && $2 == "meshagent" { found = 1 } END { exit !found }' || break
                sleep 0.1
            done
            DISPLAY=:$DISPLAY_NUMBER XAUTHORITY=$STATE/xauth XCURSOR_THEME=Adwaita xsetroot -cursor_name left_ptr
            ;;
        stop-agent) $SUDO kill -STOP "$(cat "$office/agent.pid")" ;;
        continue-agent) $SUDO kill -CONT "$(cat "$office/agent.pid")" ;;
        restart-server) stop_server "$STATE"; start_server "$STATE" ;;
        # The Office group's user consent flags (8: ask before the desktop).
        set-consent) meshctrl editdevicegroup --id "$OFFICE" --consent "${4:?flags}" > /dev/null ;;
        *) die "unknown control command ${3:-}" ;;
    esac
    exit 0
fi

STATE=$WORK/meshcentral-e2e
if [ "${1:-}" = stop ]; then
    stop_all "$STATE"
    exit 0
fi

BUILD=$(cd "${1:?usage: ci/e2e.sh <build-dir> [--keep]}" && pwd)
KEEP=${2:-}

for tool in node npm curl openssl Xvfb xauth xev xsetroot xdotool xclip xdpyinfo stdbuf; do
    command -v "$tool" > /dev/null || die "$tool is missing"
done
# The cursor test needs a cursor theme whose cursors have names.
[ -e /usr/share/icons/Adwaita/cursors/xterm ] || die "the Adwaita cursor theme is missing (package adwaita-icon-theme)"
[ "$(node -p 'process.versions.node.split(".")[0]')" -ge 20 ] || die "MeshCentral $MESHCENTRAL_VERSION needs Node.js 20 or later"

# --- MeshCentral ------------------------------------------------------------

mkdir -p "$MC"
installed=$(cd "$MC" && node -p "require('meshcentral/package.json').version" 2>/dev/null || true)
if [ "$installed" != "$MESHCENTRAL_VERSION" ]; then
    log "installing MeshCentral $MESHCENTRAL_VERSION"
    (cd "$MC" && npm install --no-audit --no-fund --loglevel=error "meshcentral@$MESHCENTRAL_VERSION")
fi

stop_all "$STATE"
rm -rf "$STATE"
mkdir -p "$STATE/data"
if [ -z "$KEEP" ]; then trap 'stop_all "$STATE"' EXIT; fi

if [ -e "/tmp/.X$DISPLAY_NUMBER-lock" ]; then die "display :$DISPLAY_NUMBER is in use"; fi

password() { openssl rand -hex 12; }
ADMIN_PASSWORD=$(password)
USER_PASSWORD=$(password)
TWO_FACTOR_PASSWORD=$(password)

# The certificate name has a dot, or MeshCentral runs in LAN mode, where
# agents look for the server by multicast instead of connecting to it.
cat > "$STATE/data/config.json" <<EOF
{
  "settings": {
    "cert": "127.0.0.1",
    "port": $PORT,
    "portBind": "127.0.0.1",
    "redirPort": 0,
    "mpsPort": 0,
    "exactPorts": true,
    "WANonly": true,
    "selfUpdate": false,
    "noAgentUpdate": 1,
    "autoBackup": false,
    "amtManager": false
  },
  "domains": { "": { "title": "Casement end-to-end tests", "newAccounts": false } }
}
EOF

node "$MC/node_modules/meshcentral" --datapath "$STATE/data" --createaccount admin --pass "$ADMIN_PASSWORD" > "$STATE/setup.log" 2>&1
node "$MC/node_modules/meshcentral" --datapath "$STATE/data" --adminaccount admin >> "$STATE/setup.log" 2>&1
log "starting MeshCentral on port $PORT"
start_server "$STATE"

setup_js() { NODE_PATH="$MC/node_modules" node "$REPO/ci/meshcentral-setup.js" "$@"; }

meshctrl adduser --user alice --pass "$USER_PASSWORD" > /dev/null
meshctrl adduser --user bob --pass "$TWO_FACTOR_PASSWORD" > /dev/null
OFFICE=$(meshctrl adddevicegroup --name Office | sed -n 's/^ok //p')
LAB=$(meshctrl adddevicegroup --name Lab | sed -n 's/^ok //p')
[ -n "$OFFICE" ] && [ -n "$LAB" ] || die "can't create the device groups"
(umask 077 && printf 'ADMIN_PASSWORD=%q\nOFFICE=%q\n' "$ADMIN_PASSWORD" "$OFFICE" > "$STATE/control.env")
for group in Office Lab; do
    for user in alice bob; do meshctrl addusertodevicegroup --group "$group" --userid "$user" --fullrights > /dev/null; done
done
setup_js totp "wss://127.0.0.1:$PORT" bob "$TWO_FACTOR_PASSWORD"
TOKEN=$(setup_js token "wss://127.0.0.1:$PORT" alice "$USER_PASSWORD" casement-e2e)
THUMBPRINT=$(openssl s_client -connect "127.0.0.1:$PORT" < /dev/null 2>/dev/null |
    openssl x509 -noout -fingerprint -sha256 | cut -d= -f2)
[ -n "$THUMBPRINT" ] || die "can't read the server certificate"

# --- X display --------------------------------------------------------------

XAUTH=$STATE/xauth
xauth -f "$XAUTH" add ":$DISPLAY_NUMBER" . "$(openssl rand -hex 16)" 2>/dev/null
chmod 644 "$XAUTH"
# Started through a script that execs Xvfb, so that no other root process
# has "-auth" on its command line: the agent picks the X server by that.
# The second screen is a second display for the agent.
cat > "$STATE/display.sh" <<'EOF'
#!/bin/sh
echo $$ > "$1"
exec Xvfb ":$2" -auth "$3" -noreset -nolisten tcp -screen 0 1280x800x24 -screen 1 1024x768x24
EOF
chmod 755 "$STATE/display.sh"
$SUDO setsid "$STATE/display.sh" "$STATE/xvfb.pid" "$DISPLAY_NUMBER" "$XAUTH" > "$STATE/xvfb.log" 2>&1 &
export DISPLAY=:$DISPLAY_NUMBER XAUTHORITY=$XAUTH XCURSOR_THEME=Adwaita
for _ in $(seq 100); do xdpyinfo > /dev/null 2>&1 && break; sleep 0.1; done
xdpyinfo > /dev/null 2>&1 || die "Xvfb didn't start"
xsetroot -cursor_name left_ptr
xsetroot -solid '#3366cc'
setsid stdbuf -oL xev -geometry 600x400+0+0 > "$STATE/xev.log" 2>&1 &
echo $! > "$STATE/xev.pid"
unset DISPLAY XAUTHORITY XCURSOR_THEME

# --- Agents -------------------------------------------------------------------

# Without systemd (containers), loginctl fails, and the agent's search for
# the console user ends in an error that switches off its clipboard. On
# such hosts it gets a loginctl that reports no sessions, as on a server
# nobody is logged in to.
mkdir -p "$STATE/bin"
if ! loginctl list-sessions > /dev/null 2>&1; then
    printf '#!/bin/sh\necho "SESSION UID USER SEAT TTY"\n' > "$STATE/bin/loginctl"
    chmod 755 "$STATE/bin/loginctl"
fi
curl -sfk -o "$STATE/meshagent" "https://127.0.0.1:$PORT/meshagents?id=6"
chmod 755 "$STATE/meshagent"
urlencode() { node -p 'encodeURIComponent(process.argv[1])' "$1"; }

start_agent() {  # directory group-id
    local dir=$STATE/$1
    mkdir -p "$dir"
    cp "$STATE/meshagent" "$dir/"
    curl -sfk -o "$dir/meshagent.msh" "https://127.0.0.1:$PORT/meshsettings?id=$(urlencode "${2#mesh//}")"
    cat > "$dir/run.sh" <<EOF
#!/bin/sh
cd "$dir"
echo \$\$ > agent.pid
PATH=$STATE/bin:\$PATH
export PATH
exec ./meshagent run
EOF
    chmod 755 "$dir/run.sh"
    launch_agent "$dir"
}

log "registering an agent in Lab and stopping it"
start_agent agent-lab "$LAB"
OFFLINE_ID=$(wait_device "$LAB" 1)
stop_agent "$STATE/agent-lab/agent.pid"
wait_device "$LAB" 0 > /dev/null
log "starting the agent in Office on display :$DISPLAY_NUMBER"
start_agent agent-office "$OFFICE"
NODE_ID=$(wait_device "$OFFICE" 1)
meshctrl editdevice --id "$NODE_ID" --name e2e-desktop > /dev/null
meshctrl editdevice --id "$OFFLINE_ID" --name e2e-desktop > /dev/null

# --- Tests ----------------------------------------------------------------------

node -e '
    const [file, ...v] = process.argv.slice(1);
    const token = JSON.parse(v[3]);
    require("fs").writeFileSync(file, JSON.stringify({
        server: "127.0.0.1:" + v[0], thumbprint: v[1], user: "alice", password: v[2],
        token_user: token.user, token_password: token.pass, two_factor_user: "bob", two_factor_password: v[4],
        device: "e2e-desktop", group: "Office", node_id: v[5], offline_group: "Lab", offline_node_id: v[6],
        state: v[7], x_env: "DISPLAY=:" + v[8] + " XAUTHORITY=" + v[9] + " XCURSOR_THEME=Adwaita", xev_log: v[7] + "/xev.log",
        xev: { x: 0, y: 0, width: 600, height: 400 }, root: { x: 700, y: 450, width: 500, height: 300 },
        second_screen: { display: ":" + v[8] + ".1", width: 1024, height: 768 },
        control: v[10] + " control " + v[7] }, null, 2));
' "$STATE/e2e.json" "$PORT" "$THUMBPRINT" "$USER_PASSWORD" "$TOKEN" "$TWO_FACTOR_PASSWORD" "$NODE_ID" "$OFFLINE_ID" \
    "$STATE" "$DISPLAY_NUMBER" "$XAUTH" "$REPO/ci/e2e.sh"

log "running the end-to-end tests"
status=0
CASEMENT_E2E=1 CASEMENT_MESHCENTRAL_E2E=$STATE/e2e.json ctest --test-dir "$BUILD" -L e2e --output-on-failure || status=$?
if [ "$status" != 0 ]; then
    log "server log:"; tail -n 40 "$STATE/server.log" >&2 || true
    log "agent log:"; tail -n 40 "$STATE/agent-office/agent.log" >&2 || true
fi
# The agent writes its crashes to meshagent.log, its desktop processes'
# crashes too; they are worth knowing about even when the tests passed.
crashes=$(grep -c 'CRASH' "$STATE/agent-office/meshagent.log" 2> /dev/null || true)
if [ "${crashes:-0}" != 0 ]; then
    log "MeshAgent crashed $crashes time(s) during the tests:"
    cat "$STATE/agent-office/meshagent.log" >&2
    cat "$STATE/agent-restarts.log" >&2 2> /dev/null || true
fi
exit "$status"
