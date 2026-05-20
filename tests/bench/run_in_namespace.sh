#!/usr/bin/env bash
# Launch a measurement endpoint without topology privileges. This accepts only
# one of the four namespace names exported by topology.py.
set -euo pipefail

namespace="${1:?namespace required}"
shift
if (($# == 0)); then
  echo "endpoint command required" >&2
  exit 2
fi

allowed=0
for owned in "${RUDP_SENDER_NS:-}" "${RUDP_SHAPER_NS:-}" "${RUDP_DELAY_NS:-}" "${RUDP_RECEIVER_NS:-}"; do
  if [[ -n "$owned" && "$namespace" == "$owned" ]]; then
    allowed=1
  fi
done
if ((allowed == 0)); then
  echo "refusing unowned namespace: $namespace" >&2
  exit 2
fi

exec ip netns exec "$namespace" setpriv \
  --reuid="${RUDP_RUN_AS_UID:?}" --regid="${RUDP_RUN_AS_GID:?}" --clear-groups -- "$@"
