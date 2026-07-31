#!/usr/bin/env bash
# Toggle/status helper for a Hyprland keybind + Waybar indicator that starts
# and stops the dictation daemon on demand, instead of it running (and
# holding GPU memory) all the time. Mirrors ml4w's own custom/hypridle
# toggle-module pattern (~/.config/hypr/scripts/hypridle.sh), including its
# pgrep/pkill-by-name approach -- capturing $! across a "cd && nohup ... &"
# background job is unreliable (it can capture an intermediate shell PID
# instead of the exec'd binary's), so process identity is tracked by name
# instead of a PID file.
set -uo pipefail

DICT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DICT_BIN="$DICT_DIR/dictation"
DICT_CONF="$DICT_DIR/configs/example.conf"
LOG_FILE="${XDG_RUNTIME_DIR:-/tmp}/dictation.log"
WAYBAR_SIGNAL=8   # must match the "signal" set on custom/dictation in modules.json

notify_waybar() {
    pkill -RTMIN+"$WAYBAR_SIGNAL" waybar 2>/dev/null || true
}

is_running() {
    pgrep -x dictation >/dev/null
}

is_ready() {
    is_running && grep -q "dictation: ready" "$LOG_FILE" 2>/dev/null
}

print_status() {
    if is_ready; then
        echo '{"text":"","class":"active","tooltip":"Dictation: models loaded on GPU, ready\nSUPER+D or click to stop"}'
    elif is_running; then
        echo '{"text":"","class":"loading","tooltip":"Dictation: loading models onto GPU..."}'
    else
        echo '{"text":"","class":"notactive","tooltip":"Dictation: not loaded\nSUPER+D or click to start"}'
    fi
}

start() {
    is_running && return
    : > "$LOG_FILE"
    ( cd "$DICT_DIR" && exec nohup "$DICT_BIN" --config "$DICT_CONF" >"$LOG_FILE" 2>&1 & )
    notify_waybar
    # Watch for the daemon's "ready" log line in the background so the
    # indicator flips to "active" the moment models finish loading, without
    # Waybar itself having to poll on a tight interval.
    (
        for _ in $(seq 1 300); do
            is_running || exit 0
            grep -q "dictation: ready" "$LOG_FILE" 2>/dev/null && { notify_waybar; exit 0; }
            sleep 0.2
        done
    ) &
    disown
}

stop() {
    is_running || return
    pkill -INT -x dictation   # same as Ctrl-C: clean shutdown (handled in main.c)
    for _ in $(seq 1 50); do
        is_running || break
        sleep 0.1
    done
    notify_waybar
}

case "${1:-toggle}" in
    status) print_status ;;
    start)  start ;;
    stop)   stop ;;
    toggle) if is_running; then stop; else start; fi ;;
    *) echo "Usage: $0 {status|start|stop|toggle}" >&2; exit 1 ;;
esac
