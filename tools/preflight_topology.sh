#!/usr/bin/env bash
# Validate the disposable benchmark topology described in BENCHMARKING.md.
# --check only inspects prerequisites. --run creates resources named from a
# unique prefix, records them in results/_work, and removes only those names.
set -euo pipefail

mode="${1:---check}"
if [[ "$mode" != "--check" && "$mode" != "--run" ]]; then
  echo "usage: $0 [--check|--run]" >&2
  exit 2
fi

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required command: $1" >&2
    return 1
  }
}

for command_name in ip tc ethtool ping; do
  require_command "$command_name"
done

netem_help="$(tc qdisc add dev lo root netem help 2>&1 || true)"
if ! grep -q netem <<<"$netem_help"; then
  echo "netem support is unavailable in tc" >&2
  exit 1
fi

htb_help="$(tc qdisc add dev lo root htb help 2>&1 || true)"
if ! grep -q htb <<<"$htb_help"; then
  echo "HTB support is unavailable in tc" >&2
  exit 1
fi

controls="$(sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null || true)"
if [[ " $controls " != *" cubic "* ]]; then
  echo "TCP CUBIC is unavailable" >&2
  exit 1
fi
if [[ " $controls " != *" bbr "* ]]; then
  echo "TCP BBR is unavailable; benchmark gate remains closed" >&2
  exit 1
fi

if [[ "$mode" == "--check" ]]; then
  echo "commands, netem, HTB, CUBIC, and BBR are available"
  echo "run with --run as root to validate namespaces, veth, qdiscs, and offloads"
  exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
  echo "--run requires root or equivalent CAP_NET_ADMIN" >&2
  exit 1
fi

prefix="rudp$$_$RANDOM"
prefix="${prefix:0:11}"
sender_ns="${prefix}s"
shaper_ns="${prefix}h"
delay_ns="${prefix}d"
receiver_ns="${prefix}r"
manifest_dir="results/_work"
manifest="$manifest_dir/${prefix}.topology"

mkdir -p "$manifest_dir"
printf '%s\n' "$sender_ns" "$shaper_ns" "$delay_ns" "$receiver_ns" >"$manifest"

cleanup() {
  local namespace
  if [[ -f "$manifest" ]]; then
    while IFS= read -r namespace; do
      ip netns del "$namespace" 2>/dev/null || true
    done <"$manifest"
    rm -f "$manifest"
  fi
}
trap cleanup EXIT HUP INT TERM

for namespace in "$sender_ns" "$shaper_ns" "$delay_ns" "$receiver_ns"; do
  ip netns add "$namespace"
  ip -n "$namespace" link set lo up
done

ip link add s0 type veth peer name h0
ip link set s0 netns "$sender_ns"
ip link set h0 netns "$shaper_ns"
ip link add h1 type veth peer name d0
ip link set h1 netns "$shaper_ns"
ip link set d0 netns "$delay_ns"
ip link add d1 type veth peer name r0
ip link set d1 netns "$delay_ns"
ip link set r0 netns "$receiver_ns"

ip -n "$sender_ns" addr add 192.0.2.1/30 dev s0
ip -n "$shaper_ns" addr add 192.0.2.2/30 dev h0
ip -n "$shaper_ns" addr add 198.51.100.1/30 dev h1
ip -n "$delay_ns" addr add 198.51.100.2/30 dev d0
ip -n "$delay_ns" addr add 203.0.113.1/30 dev d1
ip -n "$receiver_ns" addr add 203.0.113.2/30 dev r0
for pair in "$sender_ns:s0" "$shaper_ns:h0" "$shaper_ns:h1" "$delay_ns:d0" "$delay_ns:d1" "$receiver_ns:r0"; do
  namespace="${pair%%:*}"
  interface="${pair##*:}"
  ip -n "$namespace" link set "$interface" mtu 1500
  ip -n "$namespace" link set "$interface" up
  ip netns exec "$namespace" ethtool -K "$interface" tso off gso off gro off >/dev/null 2>&1 || true
done

ip -n "$sender_ns" route add default via 192.0.2.2
ip -n "$receiver_ns" route add default via 203.0.113.1
ip -n "$shaper_ns" route add 203.0.113.0/30 via 198.51.100.2
ip -n "$delay_ns" route add 192.0.2.0/30 via 198.51.100.1
ip netns exec "$shaper_ns" sysctl -q -w net.ipv4.ip_forward=1
ip netns exec "$delay_ns" sysctl -q -w net.ipv4.ip_forward=1

# HTB is the directional bottleneck. The downstream netem queues are separate
# so propagation delay is not silently converted into bottleneck buffering.
for pair in "$shaper_ns:h1" "$delay_ns:d0"; do
  namespace="${pair%%:*}"
  interface="${pair##*:}"
  ip -n "$namespace" qdisc add dev "$interface" root handle 1: htb default 10
  ip -n "$namespace" class add dev "$interface" parent 1: classid 1:10 htb rate 20mbit ceil 20mbit burst 16k cburst 16k quantum 1514
  ip -n "$namespace" qdisc add dev "$interface" parent 1:10 handle 10: pfifo limit 84
done
for pair in "$delay_ns:d1" "$shaper_ns:h0"; do
  namespace="${pair%%:*}"
  interface="${pair##*:}"
  ip -n "$namespace" qdisc add dev "$interface" root netem delay 5ms limit 65536
done

ip netns exec "$sender_ns" ping -c 1 -W 1 203.0.113.2 >/dev/null
for namespace in "$sender_ns" "$shaper_ns" "$delay_ns" "$receiver_ns"; do
  ip -n "$namespace" link show
done
echo "topology preflight passed; cleanup will now remove $prefix resources"
