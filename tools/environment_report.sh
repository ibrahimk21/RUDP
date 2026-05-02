#!/usr/bin/env bash
set -euo pipefail

version() {
  local label="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    printf '%s: ' "$label"
    "$@" 2>&1 | head -n 1
  else
    printf '%s: unavailable\n' "$label"
  fi
}

version 'C compiler' cc --version
version 'Make' make --version
version 'Python' python3 --version
version 'iproute2 ip' ip -Version
version 'iproute2 tc' tc -Version
version 'ethtool' ethtool --version
version 'tcpdump' tcpdump --version
version 'time' time --version

python3 - <<'PY'
for module_name in ("numpy", "matplotlib", "pandas"):
    try:
        module = __import__(module_name)
        print(f"{module_name}: {module.__version__}")
    except ImportError:
        print(f"{module_name}: unavailable")
PY

printf 'TCP congestion controls: '
sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null || echo unavailable
printf 'Default qdisc: '
sysctl -n net.core.default_qdisc 2>/dev/null || echo unavailable
