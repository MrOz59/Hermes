#!/usr/bin/env bash
# Entrypoint for the Hermes (Sunshine-compatible) streaming host.
# Brings up a headless Wayland session (sway) + audio, then launches hermes.
set -euo pipefail

log()  { printf '\033[0;36m[hermes]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[0;33m[hermes][warn]\033[0m %s\n' "$*" >&2; }

# Hermes keeps its configuration in its own directory instead of sharing
# ~/.config/sunshine with Apollo and Sunshine, and HOME is /config here. The
# config file is handed to hermes explicitly, so it stays flat in the volume;
# apps.json is resolved by Hermes itself and so has to match its default.
HERMES_CONFIG="${HERMES_CONFIG:-/config/hermes.conf}"
APPS_JSON="${APPS_JSON:-/config/.config/hermes/apps.json}"
LEGACY_HERMES_CONFIG="/config/sunshine.conf"
STEAM_SESSION_CMD="/usr/local/bin/hermes-steam-session"
SWAY_TEMPLATE="/etc/sway/config"
SWAY_CONFIG="/run/hermes/sway.config"
PIDS=()

cleanup() {
    log "shutting down..."
    for pid in "${PIDS[@]:-}"; do
        [ -n "${pid}" ] && kill "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

# --- timezone ---------------------------------------------------------------
# Precedence: an explicit TZ wins; otherwise inherit whatever /etc/localtime the
# host bind-mounted (it reflects the user's location). glibc reads the TZ env
# var directly, so exporting it is what actually matters; rewriting the files is
# best-effort since they may be read-only bind mounts.
if [ -n "${TZ:-}" ] && [ -f "/usr/share/zoneinfo/${TZ}" ]; then
    export TZ
    ln -sf "/usr/share/zoneinfo/${TZ}" /etc/localtime 2>/dev/null || true
    echo "${TZ}" > /etc/timezone 2>/dev/null || true
    log "timezone: ${TZ} (explicit)"
elif [ -n "${TZ:-}" ]; then
    warn "TZ='${TZ}' is not a known zone (missing /usr/share/zoneinfo/${TZ}); ignoring"
elif [ -e /etc/localtime ]; then
    log "timezone: inherited from host /etc/localtime"
else
    log "timezone: UTC (default; set TZ or mount /etc/localtime to change)"
fi

# --- base directories -------------------------------------------------------
mkdir -p /config /config/sway.d /run/hermes "${XDG_RUNTIME_DIR}"
chmod 700 "${XDG_RUNTIME_DIR}"

# --- capabilities -----------------------------------------------------------
# The image sets file capabilities on sway (cap_sys_admin+ep) and hermes
# (cap_sys_admin+p), for Steam's pressure-vessel sandbox. The effective bit on
# sway is not a preference: execve() of a file whose permitted set holds a
# capability the process cannot receive fails outright with EPERM, so under any
# runtime that does not grant SYS_ADMIN - rootless Podman by default - sway never
# starts, no Wayland socket appears, and Hermes comes up on a host that looks
# like it has no display at all, every encoder probe failing. The capability is
# out of reach in that case either way, so the file capability buys nothing and
# costs the whole session. Drop it and let sway run without.
reconcile_file_capabilities() {
    local bnd
    bnd="$(awk '/^CapBnd:/ {print $2}' /proc/self/status 2>/dev/null || true)"
    [ -n "${bnd}" ] || return 0

    # CAP_SYS_ADMIN is capability 21.
    if (( 0x${bnd} & (1 << 21) )); then
        return 0
    fi

    log "SYS_ADMIN is not in this container's bounding set; dropping the file capabilities that need it"
    local dropped=0
    for binary in /usr/bin/sway /usr/bin/hermes; do
        [ -e "${binary}" ] || continue
        if setcap -r "${binary}" 2>/dev/null; then
            dropped=1
        fi
    done
    if [ "${dropped}" = "0" ]; then
        warn "could not drop them (no setcap, or a read-only image); sway will fail to exec with 'Operation not permitted'"
        warn "grant the capability instead: --cap-add=SYS_ADMIN, or AddCapability=SYS_ADMIN in a Quadlet unit"
    else
        warn "Steam's pressure-vessel sandbox needs SYS_ADMIN and will not work; desktop streaming is unaffected"
    fi
}
reconcile_file_capabilities

# --- system dbus (needed by avahi) -----------------------------------------
start_dbus() {
    dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null || true
    mkdir -p /run/dbus
    if [ ! -S /run/dbus/system_bus_socket ]; then
        dbus-daemon --system --fork 2>/dev/null \
            && log "dbus system bus started" \
            || warn "dbus failed to start"
    fi
}

# --- mDNS discovery (Moonlight auto-discovery) ------------------------------
start_avahi() {
    avahi-daemon --no-chroot -D 2>/dev/null \
        && log "avahi-daemon started" \
        || warn "avahi-daemon failed to start (discovery via manual IP still works)"
}

# --- device detection: Hermes-KMS virtual display + real render GPU ---------
HERMES_KMS_CARD=""   # DRM card node backed by the host hermes_kms.ko module
RENDER_NODE=""       # real GPU render node used for rendering + encoding
KMS_ZEROCOPY="false" # true once sway is actually up on the Hermes-KMS DRM card

drm_driver() {
    # Print the kernel driver name for a /dev/dri/<node> (card* or renderD*).
    # Prefer the bound-driver symlink, then fall back to the uevent DRIVER=
    # line — virtual DRM devices don't always expose the driver symlink.
    local base sys drv
    base="$(basename "$1")"
    sys="/sys/class/drm/${base}/device"
    drv="$(readlink -f "${sys}/driver" 2>/dev/null || true)"
    drv="${drv##*/}"
    if [ -z "${drv}" ]; then
        drv="$(sed -n 's/^DRIVER=//p' "${sys}/uevent" 2>/dev/null | head -n1 || true)"
    fi
    printf '%s' "${drv}"
}

is_hermes_kms() {
    # The DRM driver reports "hermes-kms" (hyphen) while the platform driver may
    # use "hermes_kms" (underscore); match either, case-insensitively.
    case "$(printf '%s' "${1:-}" | tr 'A-Z-' 'a-z_')" in
        *hermes_kms*) return 0 ;;
        *)            return 1 ;;
    esac
}

detect_devices() {
    local node drv
    # The virtual display card is the DRM card driven by the host hermes_kms.ko
    # module (loaded with initial_enabled=1 and exposed via /dev/dri).
    for node in /dev/dri/card[0-9]*; do
        [ -e "${node}" ] || continue
        drv="$(drm_driver "${node}")"
        log "DRM ${node}: driver=${drv:-unknown}"
        if [ -z "${HERMES_KMS_CARD}" ] && is_hermes_kms "${drv}"; then
            HERMES_KMS_CARD="${node}"
        fi
    done

    # Real render node for GPU rendering + hardware encoding: the first
    # renderD* that is NOT provided by hermes_kms (which is not a render GPU).
    for node in /dev/dri/renderD*; do
        [ -e "${node}" ] || continue
        is_hermes_kms "$(drm_driver "${node}")" && continue
        RENDER_NODE="${node}"
        break
    done

    if [ -n "${HERMES_KMS_CARD}" ]; then
        log "Hermes-KMS virtual display: ${HERMES_KMS_CARD}"
    else
        warn "no hermes_kms DRM card found — is the host module loaded and /dev/dri passed in?"
    fi
    if [ -n "${RENDER_NODE}" ]; then
        log "render/encode GPU node: ${RENDER_NODE}"
    else
        warn "no real render node — GPU rendering/encoding will not work."
    fi
}

# --- seat management (needed by the DRM backend to open the GPU/KMS) --------
start_seat() {
    # wlroots' DRM backend needs a libseat session. This libseat build has no
    # "builtin" backend, so run seatd (works as root, no logind needed) and let
    # libseat talk to it over /run/seatd.sock.
    #
    # SEATD_VTBOUND=0 makes seatd create a seat that is NOT tied to a virtual
    # terminal. A container has no VT (/dev/tty0), so the default VT-bound seat
    # can never be activated — seatd logs "Could not open tty0" and the session
    # stays inactive, making wlroots time out ("waiting session to become
    # active"). A VT-free seat is activated immediately, so the DRM/KMS path can
    # actually come up inside the container.
    # A leftover /run/seatd.sock from a previous container run (whose seatd died
    # on restart) makes libseat's client get ECONNREFUSED, so wlroots' DRM backend
    # aborts within milliseconds and sway never brings up a Wayland socket. Only
    # reuse the socket when a live seatd actually owns it; otherwise drop the stale
    # socket and respawn.
    if pgrep -x seatd >/dev/null 2>&1 && [ -S /run/seatd.sock ]; then
        return 0
    fi
    rm -f /run/seatd.sock
    SEATD_VTBOUND=0 seatd > /var/log/seatd.log 2>&1 &
    PIDS+=("$!")
    local _
    for _ in $(seq 1 40); do
        [ -S /run/seatd.sock ] && break
        sleep 0.1
    done
    if [ -S /run/seatd.sock ]; then
        log "seatd started"
    else
        warn "seatd did not create /run/seatd.sock; see /var/log/seatd.log"
    fi
}

# --- udev (so wlroots sees the Hermes-KMS virtual-connector hotplug) --------
# The Hermes-KMS virtual output is a DRM connector ("Virtual-1") that is hotplugged
# onto card1 only when a stream starts. wlroots' DRM backend learns about hotplugs
# through a libudev monitor on the "udev" netlink group, which is populated by
# udevd. With no udevd running, sway never adopts the connector, exposes zero
# outputs, and Hermes reports "The compositor did not activate the Hermes-KMS
# output". Run udevd so the monitor is live before the first stream. (network_mode:
# host puts us in the host net namespace, so the host kernel's uevents reach us.)
start_udev() {
    if pgrep -x systemd-udevd >/dev/null 2>&1 || pgrep -x udevd >/dev/null 2>&1; then
        return 0
    fi
    local udevd="" cand
    for cand in /usr/lib/systemd/systemd-udevd /usr/lib/udev/udevd /sbin/udevd /usr/bin/udevd; do
        [ -x "${cand}" ] && { udevd="${cand}"; break; }
    done
    if [ -z "${udevd}" ]; then
        warn "udevd not found; wlroots may not see the Hermes-KMS hotplug (install systemd/udev)"
        return 1
    fi
    "${udevd}" --daemon 2>/var/log/udevd.log \
        && log "udevd started (${udevd})" \
        || { warn "udevd failed to start (see /var/log/udevd.log)"; return 1; }
    # Prime the DRM subsystem so any already-present connector is tagged, then wait
    # for the queue to drain before sway enumerates outputs.
    udevadm trigger --subsystem-match=drm --action=add >/dev/null 2>&1 || true
    udevadm settle --timeout=5 >/dev/null 2>&1 || true
}

# --- Wayland compositor (sway): KMS output with headless fallback -----------

# Start sway in the background with the currently exported WLR_* environment and
# wait for it to publish a Wayland socket. On success records the PID, exports
# WAYLAND_DISPLAY and returns 0; otherwise kills the failed instance and returns
# 1. wlroots gives up within ~10s when it cannot bring up a backend (e.g. the
# DRM session never becomes active in a container with no foreground VT), so we
# watch the process while polling — no point waiting once it has exited.
launch_sway() {
    sway -c "${SWAY_CONFIG}" > /var/log/sway.log 2>&1 &
    local pid=$! sock="" _
    for _ in $(seq 1 60); do
        kill -0 "${pid}" 2>/dev/null || break
        sock="$(find "${XDG_RUNTIME_DIR}" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' 2>/dev/null | head -n1 || true)"
        [ -n "${sock}" ] && break
        sleep 0.25
    done
    if [ -n "${sock}" ] && kill -0 "${pid}" 2>/dev/null; then
        PIDS+=("${pid}")
        export WAYLAND_DISPLAY="$(basename "${sock}")"
        # swaymsg locates sway's IPC socket via SWAYSOCK; sway only exports it to
        # its own children, so publish it here (it appears alongside the Wayland
        # socket) — without it swaymsg cannot query/set outputs.
        for _ in $(seq 1 20); do
            SWAYSOCK="$(find "${XDG_RUNTIME_DIR}" -maxdepth 1 -name 'sway-ipc.*.sock' 2>/dev/null | head -n1 || true)"
            [ -n "${SWAYSOCK}" ] && break
            sleep 0.1
        done
        [ -n "${SWAYSOCK}" ] && export SWAYSOCK
        return 0
    fi
    kill "${pid}" 2>/dev/null || true
    return 1
}

# Environment for the DRM (KMS) backend: drive the Hermes-KMS card, render on the
# real GPU, and open the device through a seatd session.
#
# We run a "drm,headless" multi-backend on purpose. The DRM backend on the
# Hermes-KMS card adopts the per-session "Virtual-1" connector that Hermes
# hotplugs, while a single headless output stays present so the compositor never
# exposes an empty output layout. That placeholder is load-bearing: Hermes'
# wl::configure_virtual_output() returns immediately when the layout is empty and
# only enters its hotplug-wait retry loop once at least one head exists. With zero
# idle outputs it queried wlr-output-management in the same millisecond it created
# the connector — before wlroots had processed the DRM hotplug — and always lost
# the race ("output-management returned no complete output layout"). The
# placeholder lives on the headless backend, not on card1, so it never drives the
# DRM_IOCTL_MODE_CREATE_DUMB path; Hermes selects "Virtual-1" by name for capture
# and reads it zero-copy, ignoring the placeholder.
setup_kms_env() {
    start_seat
    export LIBSEAT_BACKEND=seatd
    # drm = the Hermes-KMS scanout, headless = the idle placeholder output (see
    # above), libinput = the input backend. libinput is NOT autodetected once
    # WLR_BACKENDS is set explicitly, so it must be listed or sway reads zero
    # input devices (get_inputs == []) and the stream has video but no keyboard/
    # mouse/gamepad — the uinput devices Hermes injects never reach the seat.
    # WLR_LIBINPUT_NO_DEVICES=1 lets it start before any device exists (they are
    # hotplugged when a client connects); it needs the same seatd session as DRM.
    export WLR_BACKENDS=drm,headless,libinput
    export WLR_HEADLESS_OUTPUTS=1
    export WLR_DRM_DEVICES="${HERMES_KMS_CARD}"
    export WLR_RENDERER="${WLR_RENDERER:-gles2}"
    [ -n "${RENDER_NODE}" ] && \
        export WLR_RENDER_DRM_DEVICE="${WLR_RENDER_DRM_DEVICE:-${RENDER_NODE}}"
    export WLR_LIBINPUT_NO_DEVICES=1
}

# Environment for the headless backend: a software/GPU-rendered virtual output
# that needs no seat or session — the reliable path inside a container. NOTE:
# the backend selector is WLR_BACKENDS (plural); the singular form is silently
# ignored, which makes wlroots autodetect DRM and fail for lack of a seat.
setup_headless_env() {
    unset LIBSEAT_BACKEND WLR_DRM_DEVICES
    export WLR_BACKENDS=headless
    export WLR_HEADLESS_OUTPUTS=1
    export WLR_LIBINPUT_NO_DEVICES=1
    if [ -n "${RENDER_NODE}" ]; then
        export WLR_RENDERER=gles2
        export WLR_RENDER_DRM_DEVICE="${WLR_RENDER_DRM_DEVICE:-${RENDER_NODE}}"
    else
        export WLR_RENDERER=pixman
    fi
}

start_compositor() {
    # Build the effective sway config from the template (resolution is applied
    # afterwards via swaymsg, since the output name depends on the backend).
    local xw="xwayland disable"
    [ "${ENABLE_XWAYLAND}" = "true" ] && xw="xwayland enable"
    sed -e "s/__XWAYLAND__/${xw}/g" "${SWAY_TEMPLATE}" > "${SWAY_CONFIG}"

    # Prefer the Hermes-KMS DRM output; fall back to the wlroots headless backend.
    local use_kms="false"
    case "${HERMES_KMS:-auto}" in
        on)  use_kms="true" ;;
        off) use_kms="false" ;;
        *)   [ -n "${HERMES_KMS_CARD}" ] && use_kms="true" ;;
    esac

    export XDG_SESSION_TYPE=wayland
    export XDG_CURRENT_DESKTOP=sway

    local started="false"
    local compositor_kind=""
    if [ "${use_kms}" = "true" ] && [ -n "${HERMES_KMS_CARD}" ]; then
        log "starting sway on the Hermes-KMS DRM output (${HERMES_KMS_CARD})"
        setup_kms_env
        if launch_sway; then
            started="true"
            compositor_kind="kms"
            KMS_ZEROCOPY="true"
            log "Wayland session up on the Hermes-KMS output (WAYLAND_DISPLAY=${WAYLAND_DISPLAY})"
        else
            warn "sway could not start on the Hermes-KMS DRM output; see /var/log/sway.log"
            tail -n 20 /var/log/sway.log >&2 || true
            warn "the container has no active DRM session (a seat cannot be activated"
            warn "without a foreground VT); falling back to the wlroots headless backend."
        fi
    fi

    if [ "${started}" != "true" ]; then
        if [ "${use_kms}" != "true" ]; then
            warn "using the wlroots HEADLESS backend (software virtual display)."
            warn "For the low-latency KMS path, load hermes_kms.ko on the host and pass /dev/dri."
        fi
        setup_headless_env
        if launch_sway; then
            started="true"
            compositor_kind="headless"
            log "Wayland session up on a headless output (WAYLAND_DISPLAY=${WAYLAND_DISPLAY})"
        fi
    fi

    if [ "${started}" != "true" ]; then
        warn "sway did not create a Wayland socket; see /var/log/sway.log"
        tail -n 40 /var/log/sway.log >&2 || true
        return 1
    fi

    if [ "${compositor_kind}" = "kms" ]; then
        # sway is on a drm,headless multi-backend: the DRM backend adopts the
        # per-session "Virtual-1" connector Hermes hotplugs on this card (enabled
        # by headless_mode in hermes.conf) and Hermes reads that connector
        # zero-copy; the headless backend contributes one idle placeholder output.
        # The placeholder is what lets Hermes' virtual-output activation succeed:
        # it bails instantly on an empty output layout and only waits for the
        # hotplug once a head exists (see setup_kms_env). We must NOT force a mode
        # onto card1 here — Hermes owns "Virtual-1" per session, and capture picks
        # the output by connector name, so the placeholder is harmless.
        # Some wlroots builds bring the headless backend up with zero outputs, which
        # would defeat the placeholder, so create one explicitly if the layout is
        # still empty.
        local heads
        heads="$(swaymsg -t get_outputs 2>/dev/null | grep -c '"name"' || true)"
        if [ "${heads:-0}" -eq 0 ]; then
            swaymsg create_output >/dev/null 2>&1 || true
            sleep 0.3
        fi
        log "sway idling on ${HERMES_KMS_CARD} with a headless placeholder; Hermes will hotplug Virtual-1 per session"
    else
        # Headless fallback: Hermes captures sway's own output directly, so ensure
        # one exists and carries the requested mode.
        local out
        out="$(swaymsg -t get_outputs 2>/dev/null \
            | grep -oE '"name"[[:space:]]*:[[:space:]]*"[^"]+"' | head -n1 \
            | sed -E 's/.*"([^"]+)"$/\1/')"
        if [ -z "${out}" ]; then
            # Some wlroots builds start the headless backend with zero outputs;
            # create one explicitly so there is a surface to capture.
            swaymsg create_output >/dev/null 2>&1 || true
            sleep 0.3
            out="$(swaymsg -t get_outputs 2>/dev/null \
                | grep -oE '"name"[[:space:]]*:[[:space:]]*"[^"]+"' | head -n1 \
                | sed -E 's/.*"([^"]+)"$/\1/')"
        fi
        if [ -n "${out}" ]; then
            log "compositor output: ${out} -> ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}@${DISPLAY_REFRESH}Hz"
            swaymsg output "${out}" mode "${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}@${DISPLAY_REFRESH}Hz" >/dev/null 2>&1 \
                || swaymsg output "${out}" resolution "${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}@${DISPLAY_REFRESH}Hz" >/dev/null 2>&1 \
                || warn "could not set mode ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}@${DISPLAY_REFRESH}Hz on ${out}"
        else
            warn "could not determine the compositor output name (see /var/log/sway.log)"
        fi
    fi

    # Surface the XWayland DISPLAY for X11-only apps launched by the host.
    if [ "${ENABLE_XWAYLAND}" = "true" ]; then
        export DISPLAY="${DISPLAY:-:0}"
        log "XWayland enabled (DISPLAY=${DISPLAY}) for X11 application compatibility"
    fi
}

# --- audio (PipeWire) -------------------------------------------------------
# Run the whole PipeWire stack (pipewire + wireplumber + pipewire-pulse) as root
# under one shared, world-usable runtime dir so Hermes (root) and the non-root
# steam user reach the same server. PipeWire warns under root but works; it has
# no PulseAudio-style "system mode", so a single fixed-path server is the
# container equivalent. Hermes creates its own loopback sinks (sink-sunshine-*)
# at stream time and captures them, so we define no sink here — the watchdog only
# relaunches daemons and reopens the socket.
PIPEWIRE_RUNTIME_DIR=/run/pipewire
PULSE_SOCKET="${PIPEWIRE_RUNTIME_DIR}/pulse/native"

pw_alive() {
    # Lightweight check: socket exists and pipewire-pulse process is running.
    # Avoids the heavy `pactl info` process spawn on every watchdog iteration.
    [ -S "${PULSE_SOCKET}" ] && pgrep -x pipewire-pulse >/dev/null 2>&1
}

pw_ready() {
    # Runs once the pulse socket answers. The non-root steam user connects to the
    # root-owned socket, so open it up. We do NOT pin a default sink: Hermes
    # creates its own loopback sinks (sink-sunshine-stereo / -surround51 /
    # -surround71) at stream start and sets the one matching the negotiated
    # channel layout as the default itself, then captures that same sink.
    chmod 0666 "${PULSE_SOCKET}" 2>/dev/null || true
}

_pw_spawn() {
    pipewire       >>/var/log/pipewire.log       2>&1 & PIDS+=("$!")
    wireplumber    >>/var/log/wireplumber.log    2>&1 & PIDS+=("$!")
    pipewire-pulse >>/var/log/pipewire-pulse.log 2>&1 & PIDS+=("$!")
}

start_pipewire() {
    log "starting PipeWire (pipewire + wireplumber + pipewire-pulse)"
    mkdir -p "${PIPEWIRE_RUNTIME_DIR}/pulse"
    chmod 0777 "${PIPEWIRE_RUNTIME_DIR}" "${PIPEWIRE_RUNTIME_DIR}/pulse"
    export PIPEWIRE_RUNTIME_DIR PULSE_RUNTIME_PATH="${PIPEWIRE_RUNTIME_DIR}/pulse"
    _pw_spawn
    # Hermes reaches PipeWire through the pulse-compat socket.
    export PULSE_SERVER="unix:${PULSE_SOCKET}"
    local _
    for _ in $(seq 1 40); do pw_alive && break; sleep 0.3; done
    if pw_alive; then
        pw_ready
        log "PipeWire up (pulse socket ${PULSE_SOCKET}); Hermes manages its own loopback sink"
    else
        warn "PipeWire pulse socket not reachable (see /var/log/pipewire*.log)"
        return 1
    fi
}

# PipeWire has no supervisor here and the entrypoint execs into hermes, so watch
# it in the background: when the control socket stops answering, relaunch the
# trio and reopen the socket. Hermes recreates its own loopback sink on the next
# stream, so there is no default to re-pin here.
start_pipewire_watchdog() {
    (
        set +e
        while :; do
            sleep 5
            pw_alive && continue
            warn "PipeWire is unreachable; restarting it"
            pkill -x pipewire-pulse 2>/dev/null
            pkill -x wireplumber    2>/dev/null
            pkill -x pipewire       2>/dev/null
            sleep 1
            _pw_spawn
            for _ in $(seq 1 40); do pw_alive && break; sleep 0.3; done
            if pw_alive; then
                pw_ready
                log "PipeWire restarted"
            else
                warn "PipeWire restart failed (see /var/log/pipewire*.log)"
            fi
        done
    ) &
    PIDS+=("$!")
    log "pipewire watchdog started"
}

# --- GPU access for the non-root steam user ---------------------------------
# The passed-through /dev/dri nodes keep the HOST's group IDs (e.g. render=105,
# card/video=44), which almost never match the container's own render/video
# groups. root owns the nodes so Hermes is unaffected, but the steam user then
# can't open renderD128 — gamescope's Vulkan device enumeration fails ("failed
# to find physical device"), it aborts, and the streamed Virtual-1 stays black.
# Grant access by making steam a member of a group whose GID equals each node's
# actual owner GID (creating that group when the host GID is unknown in the
# container). Must run before we spawn gamescope; the build-time `usermod -aG
# render` can't cover a GID that only exists at runtime.
grant_gpu_access() {
    local user="$1" node gid gname
    id "${user}" >/dev/null 2>&1 || return 0
    # Cache user's current groups to avoid repeated id -G calls
    local user_groups
    user_groups="$(id -G "${user}" 2>/dev/null)" || user_groups=""
    for node in /dev/dri/card[0-9]* /dev/dri/renderD[0-9]*; do
        [ -e "${node}" ] || continue
        # Use bash parameter expansion to extract GID (avoid stat fork)
        gid="$(stat -c '%g' "${node}" 2>/dev/null)" || continue
        [ -n "${gid}" ] || continue
        # Check if user already has this group using bash string matching
        case " ${user_groups} " in
            *" ${gid} "*) continue ;;
        esac
        # Use bash parameter expansion instead of cut
        gname="$(getent group "${gid}" 2>/dev/null)"
        gname="${gname%%:*}"
        if [ -z "${gname}" ]; then
            gname="gpu-${gid}"
            groupadd -g "${gid}" "${gname}" 2>/dev/null || true
        fi
        [ -n "${gname}" ] && usermod -aG "${gname}" "${user}" 2>/dev/null \
            && log "granted ${user} access to ${node} (gid ${gid} via group ${gname})" \
            && user_groups="${user_groups} ${gid}"
    done
}

# --- Steam Big Picture -------------------------------------------------------
# Steam is NOT autostarted. It is registered as a per-session Hermes app: when a
# client streams "Steam Big Picture", Hermes runs ${STEAM_SESSION_CMD} at the
# client's negotiated resolution (native, no upscaling) as the non-root "steam"
# user (Steam refuses to run as root). gamescope is a nested Wayland client of
# the root-owned sway session, so its fullscreen window is scanned out to the
# captured Virtual-1 output. When the client disconnects Hermes tears the app
# down, so Steam is relaunched fresh per connection (its data persists under
# /config/steam, so only the very first bootstrap is slow).
#
# This one-time boot step does the root-side setup the per-session launcher
# relies on: grant the steam user GPU access, create its runtime dir, and hand
# it traverse+rw on the root-owned sway Wayland socket (mode 700 dir) by absolute
# path, plus install the sway rule that fullscreens the gamescope window.
prepare_steam() {
    command -v steam >/dev/null 2>&1 || return 0
    [ "${AUTOSTART_STEAM:-true}" = "true" ] || { log "AUTOSTART_STEAM=false; skipping Steam prep (streamed app shows the desktop)"; return 0; }
    command -v gamescope >/dev/null 2>&1 || { warn "Steam present but gamescope missing; Steam app disabled"; return 0; }
    id steam >/dev/null 2>&1 || { warn "no 'steam' user; Steam app disabled"; return 0; }
    [ -n "${WAYLAND_DISPLAY:-}" ] || { warn "no Wayland session up; Steam app will not work"; return 0; }

    # Ensure the steam user can actually open the GPU render node.
    grant_gpu_access steam

    # Grant access to /dev/uhid for virtual gamepad creation
    [ -c /dev/uhid ] && chgrp input /dev/uhid && chmod 660 /dev/uhid

    # Grant access to input devices whose GID doesn't match container's input group
    for dev in /dev/input/event* /dev/input/js*; do
        [ -c "${dev}" ] || continue
        gid="$(stat -c '%g' "${dev}" 2>/dev/null)" || continue
        [ "${gid}" = "992" ] && continue
        gname="$(getent group "${gid}" | cut -d: -f1 2>/dev/null)"
        [ -z "${gname}" ] && gname="input-${gid}" && groupadd -g "${gid}" "${gname}" 2>/dev/null || true
        usermod -aG "${gname}" steam 2>/dev/null && log "granted steam access to ${dev} (gid ${gid} via group ${gname})"
    done

    local uid runtime sockpath
    uid="$(id -u steam)"
    runtime="/run/user/${uid}"
    install -d -o steam -g steam -m 700 "${runtime}"
    # Steam data (client bootstrap, library, config) persists in the /config
    # volume via HOME so it survives container recreation.
    install -d -o steam -g steam /config/steam

    sockpath="${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}"
    chmod o+x "${XDG_RUNTIME_DIR}" 2>/dev/null || true
    chmod o+rw "${sockpath}" 2>/dev/null || true

    # Pin the gamescope window to the output Hermes actually captures and
    # fullscreen it there. On the KMS path sway runs a drm,headless multi-backend
    # (see setup_kms_env): Hermes captures the per-session "Virtual-1" connector,
    # but sway keeps focus on the idle "HEADLESS-1" placeholder, so a bare
    # "fullscreen enable" fullscreens gamescope on the WRONG output and the
    # captured one stays black. Route it to Virtual-1 by name (the rule fires
    # when the window maps, after Hermes has hotplugged the connector) and focus
    # it so input lands there too. On the headless fallback there is only one
    # output, so no move is needed.
    if [ "${KMS_ZEROCOPY}" = "true" ]; then
        swaymsg 'for_window [app_id="gamescope"] move container to output Virtual-1, fullscreen enable, focus, border none' >/dev/null 2>&1 || true
    else
        swaymsg 'for_window [app_id="gamescope"] fullscreen enable, focus, border none' >/dev/null 2>&1 || true
    fi

    log "Steam Big Picture ready as a per-session app (launches at the client's resolution when streamed)"
}

# --- seed the required config keys ------------------------------------------
# Hermes rewrites this file itself whenever you change settings in the web UI,
# so merge idempotently: add each required key only when it is absent, and never
# overwrite a value the user (or Hermes) has already written. These keys enable
# the Hermes-KMS virtual display backend, dual-stack networking and UPnP.
ensure_conf_key() {
    local key="$1" val="$2"
    if ! grep -Eq "^[[:space:]]*${key}[[:space:]]*=" "${HERMES_CONFIG}" 2>/dev/null; then
        printf '%s = %s\n' "${key}" "${val}" >> "${HERMES_CONFIG}"
        log "config: added ${key} = ${val}"
    fi
}

set_conf_key() {
    local key="$1" val="$2"
    if grep -Eq "^[[:space:]]*${key}[[:space:]]*=" "${HERMES_CONFIG}" 2>/dev/null; then
        sed -i -E "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*.*/${key} = ${val}/" "${HERMES_CONFIG}"
        log "config: updated ${key} = ${val}"
    else
        printf '%s = %s\n' "${key}" "${val}" >> "${HERMES_CONFIG}"
        log "config: added ${key} = ${val}"
    fi
}

seed_config() {
    mkdir -p "$(dirname "${HERMES_CONFIG}")"
    # Volumes created before Hermes stopped sharing ~/.config/sunshine with
    # Apollo and Sunshine carry the config as /config/sunshine.conf. Hermes
    # migrates its own directory (/config/.config/sunshine -> .../hermes) on
    # start, but this flat path is ours, so rename it here or the container would
    # come up on an empty config and lose the user's settings.
    if [ -f "${LEGACY_HERMES_CONFIG}" ] && [ ! -f "${HERMES_CONFIG}" ]; then
        mv "${LEGACY_HERMES_CONFIG}" "${HERMES_CONFIG}" \
            && log "migrated ${LEGACY_HERMES_CONFIG} -> ${HERMES_CONFIG}" \
            || warn "could not migrate ${LEGACY_HERMES_CONFIG}"
    fi
    touch "${HERMES_CONFIG}"
    ensure_conf_key address_family both
    ensure_conf_key upnp enabled
    ensure_conf_key system_tray disabled
    # Name the GPU explicitly. Both of Hermes' fallbacks are wrong on a hybrid
    # host: VAAPI defaults to /dev/dri/renderD128, and capture takes the first
    # render node that opens - and node numbers follow module load order, so on a
    # laptop whose discrete card loads first that is the GPU nothing renders on.
    # detect_devices already worked out which node is the encode GPU; the guess
    # is only ever needed where nobody said.
    if [ -n "${RENDER_NODE}" ]; then
        ensure_conf_key adapter_name "${RENDER_NODE}"
    fi
    if [ "${KMS_ZEROCOPY}" = "true" ]; then
        set_conf_key virtual_display_backend hermes_kms
        set_conf_key headless_mode enabled
    else
        # "none" is the value Hermes actually parses. This wrote "headless" for a
        # while, which no version ever accepted: it was dropped on load, the
        # default EVDI backend stayed in place, and the whole install was then
        # judged - and warned about - as a broken EVDI host that had never wanted
        # EVDI in the first place.
        set_conf_key virtual_display_backend none
        set_conf_key headless_mode disabled
    fi
    # Audio: let Hermes fully own its loopback sinks. It creates
    # sink-sunshine-stereo / -surround51 / -surround71, sets the one matching the
    # stream's channel layout as the PipeWire default (so Steam/games play into
    # it), and captures that same sink — capture and playback stay aligned.
    # Do NOT pin audio_sink: an earlier build pinned it to a custom "hermes" null
    # sink, but Hermes still made sink-sunshine-stereo the default, so games
    # played into sink-sunshine-stereo while Hermes recorded the silent
    # hermes.monitor. Strip that stale pin from existing configs so sound returns.
    if [ "${START_PIPEWIRE}" = "true" ]; then
        sed -i -E '/^[[:space:]]*audio_sink[[:space:]]*=[[:space:]]*hermes[[:space:]]*$/d' "${HERMES_CONFIG}" 2>/dev/null || true
    fi
}

# --- sanitize the app list --------------------------------------------------
# Hermes ships a default app list (copied to $APPS_JSON on first run) tailored to
# a physical X11 desktop and a non-root Steam, neither of which fits this image:
#   • "Low Res Desktop" runs `xrandr --output HDMI-1 ...` to switch resolution —
#     that fails on our virtual Wayland output, and resolution is negotiated by
#     the Moonlight client anyway, so once its xrandr command is stripped it is
#     just a duplicate of "Desktop". We drop the whole entry.
#   • "Steam Big Picture" runs `setsid steam steam://...` as the Hermes process
#     user (root here); Steam refuses to run as root, so it silently no-ops.
#   • "Gamescope Steam Session" runs `hermes-gamescope-launch`, which likewise
#     starts Steam as root and cannot work here.
# We instead register our own per-session launcher as the
# "Steam Big Picture" cmd (see configure_steam_app), so streaming that app brings
# Steam up correctly at the client's resolution. Strip the broken commands, drop
# the redundant "Low Res Desktop" and gamescope-launch apps in place, idempotently,
# so existing configs get fixed too. Fresh configs get the cleaned
# /usr/share/hermes/apps.json template.
sanitize_apps() {
    [ -f "${APPS_JSON}" ] || return 0
    grep -Eq 'xrandr|steam://|hermes-gamescope-launch|Low Res Desktop' "${APPS_JSON}" 2>/dev/null || return 0
    command -v jq >/dev/null 2>&1 || { warn "jq missing; leaving apps.json unchanged"; return 0; }
    local tmp; tmp="$(mktemp)"
    # Step 1: Remove apps with hermes-gamescope-launch command or "Low Res Desktop" name
    # Step 2: Clean prep-cmd (remove xrandr commands, clean up empty entries)
    # Step 3: Clean detached (remove steam:// commands)
    if jq '
          # Remove xrandr from prep-cmd.do, remove xrandr/steam:// from prep-cmd.undo
          def clean_prep_cmd:
            if has("prep-cmd") then
              .["prep-cmd"] = [
                .["prep-cmd"][] |
                (.do //= "") | (.undo //= "") |
                select(.do | test("xrandr") | not) |
                if (.undo | test("xrandr|steam://")) then .undo = "" else . end
              ] | [.[] | select(.do != "" or .undo != "")] |
              if length == 0 then empty else . end;
            else . end;

          # Remove steam:// from detached commands
          def clean_detached:
            if has("detached") then
              .detached = [.detached[] | select(test("steam://") | not)] |
              if length == 0 then empty else . end;
            else . end;

          .apps |= [
            .[] |
            select((.cmd // "") != "hermes-gamescope-launch") |
            select(.name != "Low Res Desktop") |
            clean_prep_cmd |
            clean_detached
          ]
        ' "${APPS_JSON}" > "${tmp}" 2>/dev/null && [ -s "${tmp}" ]; then
        cat "${tmp}" > "${APPS_JSON}"   # keep the mounted file's inode/ownership
        log "sanitized apps.json (removed Low Res Desktop / xrandr / root steam:// / gamescope-launch)"
    else
        warn "could not sanitize apps.json (left unchanged)"
    fi
    rm -f "${tmp}"
}

# --- register the per-session Steam launcher --------------------------------
# Point the "Steam Big Picture" app's cmd at our per-session
# launcher so Hermes runs it (at the client's negotiated resolution) when the app
# is streamed. Fresh /config volumes already carry this in the apps.json template;
# this patches an existing /config idempotently — updating the cmd, or appending
# the app when it is missing. Only touches the file when steam + gamescope exist.
configure_steam_app() {
    command -v steam >/dev/null 2>&1 || return 0
    command -v gamescope >/dev/null 2>&1 || return 0
    [ -f "${APPS_JSON}" ] || return 0
    command -v jq >/dev/null 2>&1 || { warn "jq missing; cannot register the Steam app"; return 0; }
    # Already wired to the launcher? nothing to do.
    jq -e --arg cmd "${STEAM_SESSION_CMD}" \
        'any(.apps[]?; .name == "Steam Big Picture" and .cmd == $cmd)' \
        "${APPS_JSON}" >/dev/null 2>&1 && return 0
    local tmp; tmp="$(mktemp)"
    if jq --arg cmd "${STEAM_SESSION_CMD}" '
          if any(.apps[]?; .name == "Steam Big Picture")
          then .apps |= map(if .name == "Steam Big Picture" then .cmd = $cmd else . end)
          else .apps += [{
              "name": "Steam Big Picture",
              "image-path": "steam.png",
              "allow-client-commands": false,
              "cmd": $cmd
          }]
          end
        ' "${APPS_JSON}" > "${tmp}" 2>/dev/null && [ -s "${tmp}" ]; then
        cat "${tmp}" > "${APPS_JSON}"   # keep the mounted file's inode/ownership
        log "registered Steam Big Picture as a per-session app (cmd=${STEAM_SESSION_CMD})"
    else
        warn "could not register the Steam app in apps.json (left unchanged)"
    fi
    rm -f "${tmp}"
}

# --- bring up the session ---------------------------------------------------
start_dbus
[ "${START_AVAHI}" = "true" ] && start_avahi || true
if [ "${START_COMPOSITOR}" = "true" ]; then
    detect_devices
    start_udev || warn "continuing without udev (Hermes-KMS hotplug may not be seen)"
    start_compositor || warn "continuing without a Wayland session (capture will fail)"
fi
[ "${START_PIPEWIRE}" = "true" ] && { start_pipewire || warn "continuing without audio"; start_pipewire_watchdog; }

# Steam Big Picture per-session prep. Needs the Wayland session and audio above.
# This does NOT start Steam — it just readies the steam user so the per-session
# launcher (run by Hermes when the app is streamed) works.
[ "${START_COMPOSITOR}" = "true" ] && { prepare_steam || warn "Steam prep failed (see /var/log/steam.log)"; }

# --- hardware encode diagnostics (non-fatal) --------------------------------
if [ -d /dev/dri ]; then
    log "GPU nodes: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
    vainfo 2>/dev/null | grep -E 'Driver version|VAProfile' | head -n 5 || \
        warn "VAAPI not usable inside the container (check /dev/dri passthrough and drivers)"
else
    warn "/dev/dri not present — no GPU. Wayland capture/hardware encoding will not work."
fi

seed_config
sanitize_apps
configure_steam_app
log "launching hermes with config: ${HERMES_CONFIG}"
log "web UI will be available at https://<host>:47990"
cd /config
exec /usr/bin/hermes "${HERMES_CONFIG}" "$@"
