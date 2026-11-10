#!/bin/sh
set -eu

control_url="${GLUETUN_CONTROL_URL:-http://127.0.0.1:8000}"
poll_seconds="${GLUETUN_POLL_SECONDS:-15}"
relay_pid=""
active_endpoint=""

case "$poll_seconds" in
    ''|*[!0-9]*|0)
        echo "GLUETUN_POLL_SECONDS must be a positive integer" >&2
        exit 2
        ;;
esac

stop_relay() {
    if [ -n "$relay_pid" ] && kill -0 "$relay_pid" 2>/dev/null; then
        kill "$relay_pid"
        wait "$relay_pid" 2>/dev/null || true
    fi
    relay_pid=""
}

trap 'stop_relay; exit 0' INT TERM

read_endpoint() {
    port_json="$(curl --fail --silent --show-error --max-time 5 \
        "$control_url/v1/portforward" 2>/dev/null)" || return 1
    ip_json="$(curl --fail --silent --show-error --max-time 5 \
        "$control_url/v1/publicip/ip" 2>/dev/null)" || return 1

    port="$(printf '%s' "$port_json" | jq -er \
        '.port | select(type == "number" and . >= 1 and . <= 65535)' 2>/dev/null)" \
        || return 1
    public_ip="$(printf '%s' "$ip_json" | jq -er \
        '.public_ip | select(type == "string" and length > 0)' 2>/dev/null)" \
        || return 1

    printf '%s:%s\n' "$public_ip" "$port"
}

start_relay() {
    public_ip="${1%:*}"
    port="${1##*:}"

    echo "VPN endpoint ready: $public_ip:$port/udp"
    echo "Starting Dusklight Online relay; copy the Relay code line below."

    if [ "${RELAY_VERBOSE:-off}" = "on" ]; then
        dusklight_online_relay \
            --host 0.0.0.0 --port "$port" \
            --public-host "$public_ip" --public-port "$port" --verbose &
    else
        dusklight_online_relay \
            --host 0.0.0.0 --port "$port" \
            --public-host "$public_ip" --public-port "$port" &
    fi
    relay_pid="$!"
}

echo "Waiting for Gluetun to establish Proton VPN port forwarding..."

while :; do
    endpoint="$(read_endpoint || true)"

    if [ -n "$endpoint" ] && [ "$endpoint" != "$active_endpoint" ]; then
        if [ -n "$active_endpoint" ]; then
            echo "VPN endpoint changed from $active_endpoint to $endpoint; restarting relay."
        fi
        stop_relay
        active_endpoint="$endpoint"
        start_relay "$active_endpoint"
    elif [ -n "$endpoint" ] && { [ -z "$relay_pid" ] || ! kill -0 "$relay_pid" 2>/dev/null; }; then
        if [ -n "$relay_pid" ]; then
            wait "$relay_pid" 2>/dev/null || true
            echo "Relay stopped unexpectedly; restarting it."
        fi
        start_relay "$active_endpoint"
    fi

    sleep "$poll_seconds" &
    wait "$!"
done
