#!/usr/bin/env bash
set -euo pipefail

kind="${1:?test kind required}"
case "$kind" in
  unit) pattern='tests/unit/test_*' ;;
  integration) pattern='tests/integration/test_*' ;;
  sanitize) pattern='tests/unit/test_*' ;;
  *) echo "unknown test kind: $kind" >&2; exit 2 ;;
esac

shopt -s nullglob
tests=( $pattern )
if ((${#tests[@]} == 0)); then
  printf 'No %s tests exist yet; scaffold check passed.\n' "$kind"
  exit 0
fi

for test_bin in "${tests[@]}"; do
  timeout 30s "$test_bin"
done
