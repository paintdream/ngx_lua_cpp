#!/usr/bin/env bash
# run-server.sh - ngx_lua_cpp demo center helper (Linux / OpenResty)
#
# Syncs the canonical config/UI into the web/run nginx prefix and manages the
# server process. Works from any working directory.
#
# Usage:
#   ./web/run-server.sh            # start (default)
#   ./web/run-server.sh restart    # stop, sync, start
#   ./web/run-server.sh stop       # stop
#   ./web/run-server.sh build      # cmake configure + build (needs LUA_DIR if LuaJIT is not auto-detected)
#
# Environment:
#   NGINX_BIN   path to openresty/nginx binary (default: `openresty`, then `nginx`)
#   LUA_DIR     LuaJIT install root, only used by `build`
#
# The script copies web/nginx.conf and web/index.html into web/run (mime.types
# is copied from the nginx install on first run, or a minimal set is written as
# a fallback), then starts `nginx -p web/run -c conf/nginx.conf` and probes
# http://127.0.0.1:8080/api/info.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_DIR="$SCRIPT_DIR/run"
PORT=8080
URL="http://127.0.0.1:${PORT}/api/info"

# ------------------------------------------------------------------ find nginx
if [ -z "${NGINX_BIN:-}" ]; then
    if command -v openresty >/dev/null 2>&1; then
        NGINX_BIN="$(command -v openresty)"
    elif command -v nginx >/dev/null 2>&1; then
        NGINX_BIN="$(command -v nginx)"
    else
        echo "error: neither openresty nor nginx found on PATH; set NGINX_BIN=/path/to/nginx" >&2
        exit 1
    fi
fi
NGINX_BIN="$(readlink -f "$NGINX_BIN")"
PID_FILE="$RUN_DIR/logs/nginx.pid"
# the master's argv is `nginx -p <RUN_DIR> -c conf/nginx.conf`, so matching on
# " -p $RUN_DIR" only ever touches this demo's own nginx instance, never an
# unrelated system nginx. Workers ("nginx: worker process") do not carry the
# prefix and are ignored - killing the master takes them down.
CMD_PATTERN=" -p $RUN_DIR"

nginx_running() {
    [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}

# true if any process command line contains the given literal string
proc_cmdline_has() {
    local pattern="$1"
    for p in /proc/[0-9]*; do
        [ -r "$p/cmdline" ] || continue
        if tr '\0' ' ' < "$p/cmdline" 2>/dev/null | grep -qF -- "$pattern"; then
            return 0
        fi
    done
    return 1
}

# SIGKILL every process whose command line contains the given literal string
proc_cmdline_kill() {
    local pattern="$1" killed=0
    for p in /proc/[0-9]*; do
        [ -r "$p/cmdline" ] || continue
        if tr '\0' ' ' < "$p/cmdline" 2>/dev/null | grep -qF -- "$pattern"; then
            kill -9 "${p#/proc/}" 2>/dev/null && killed=1
        fi
    done
    return "$killed"
}

# --------------------------------------------------------------- stop
stop_nginx() {
    local running=0
    if nginx_running; then
        running=1
    elif proc_cmdline_has "$CMD_PATTERN"; then
        running=1
        echo "note: nginx running but the pid file is stale"
    fi

    if [ "$running" = 1 ]; then
        echo "Stopping nginx..."
        # a stale pid file makes -s stop fail; never abort on that, fall
        # through to the force-kill below.
        "$NGINX_BIN" -p "$RUN_DIR" -c conf/nginx.conf -s stop || true
        for _ in $(seq 1 20); do
            nginx_running || proc_cmdline_has "$CMD_PATTERN" || break
            sleep 0.5
        done
        if proc_cmdline_has "$CMD_PATTERN"; then
            proc_cmdline_kill "$CMD_PATTERN" || true
            echo "nginx force-killed."
        else
            echo "nginx stopped."
        fi
    fi
    rm -f "$PID_FILE"
}

# --------------------------------------------------------------- sync files
sync_files() {
    mkdir -p "$RUN_DIR/conf" "$RUN_DIR/logs" "$RUN_DIR/html"
    cp -f "$SCRIPT_DIR/nginx.conf" "$RUN_DIR/conf/nginx.conf"
    cp -f "$SCRIPT_DIR/index.html" "$RUN_DIR/html/index.html"

    if [ ! -f "$RUN_DIR/conf/mime.types" ]; then
        local src=""

        # 1) from `nginx -V` configure arguments (--prefix=...)
        local nginx_prefix
        nginx_prefix="$("$NGINX_BIN" -V 2>&1 | sed -n 's/.*--prefix=\([^ ]*\).*/\1/p' | head -1 || true)"
        if [ -n "$nginx_prefix" ] && [ -f "$nginx_prefix/conf/mime.types" ]; then
            src="$nginx_prefix/conf/mime.types"
        fi

        # 2) relative to the binary
        if [ -z "$src" ] && [ -f "$(dirname "$NGINX_BIN")/../conf/mime.types" ]; then
            src="$(dirname "$NGINX_BIN")/../conf/mime.types"
        fi

        # 3) common locations
        if [ -z "$src" ] && [ -f /etc/nginx/mime.types ]; then
            src=/etc/nginx/mime.types
        fi
        if [ -z "$src" ] && [ -f /usr/local/openresty/nginx/conf/mime.types ]; then
            src=/usr/local/openresty/nginx/conf/mime.types
        fi

        if [ -n "$src" ]; then
            cp -f "$src" "$RUN_DIR/conf/mime.types"
            echo "Copied mime.types from $src"
        else
            cat > "$RUN_DIR/conf/mime.types" <<'EOF'
types {
    text/html html htm;
    text/css css;
    application/javascript js mjs;
    application/json json;
    image/png png;
    image/jpeg jpg jpeg;
    image/svg+xml svg;
}
EOF
            echo "Wrote minimal mime.types (nginx install not found)"
        fi
    fi
}

# --------------------------------------------------------------- probe
probe() {
    if command -v curl >/dev/null 2>&1; then
        local info
        if info="$(curl -sf --max-time 5 "$URL" 2>/dev/null)"; then
            echo "OK: demo center at http://localhost:${PORT}"
            echo "$info" | grep -o '"ngx_lua_cpp_running":[a-z]*' | sed 's/^/    /' || true
        else
            echo "warning: nginx started but $URL is not reachable; check $RUN_DIR/logs/error.log" >&2
        fi
    else
        echo "nginx started; curl not found, skipping probe"
    fi
}

# --------------------------------------------------------------- start
start_nginx() {
    if nginx_running || proc_cmdline_has "$CMD_PATTERN"; then
        echo "nginx already running (pid $(cat "$PID_FILE" 2>/dev/null || echo '?'))"
        probe || true
        return
    fi

    echo "Starting nginx (prefix: $RUN_DIR)..."
    "$NGINX_BIN" -p "$RUN_DIR" -c conf/nginx.conf
    for _ in $(seq 1 20); do
        nginx_running && break
        sleep 0.5
    done
    probe
}

# --------------------------------------------------------------- build
build_library() {
    echo "Building ngx_lua_cpp (LUA_DIR=${LUA_DIR:-unset}; set it if LuaJIT is not auto-detected)..."
    cmake -S "$REPO_DIR" -B "$REPO_DIR/build"
    cmake --build "$REPO_DIR/build" -j"$(nproc)"
    echo "Built: $REPO_DIR/build/libngx_lua_cpp.so"
}

# --------------------------------------------------------------- main
case "${1:-start}" in
    start)
        sync_files
        start_nginx
        ;;
    restart)
        stop_nginx
        sync_files
        start_nginx
        ;;
    stop)
        stop_nginx
        ;;
    build)
        build_library
        ;;
    *)
        echo "usage: $0 [start|restart|stop|build]" >&2
        exit 1
        ;;
esac
