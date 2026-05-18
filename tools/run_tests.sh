#!/usr/bin/env bash
set -euo pipefail

kind="${1:?test kind required}"
build_dir="${BUILD_DIR:-build}"
case "$kind" in
  unit) pattern="$build_dir/tests/unit/test_*" ;;
  integration) pattern="$build_dir/tests/integration/test_*" ;;
  sanitize) pattern="$build_dir/tests/unit/test_*" ;;
  *) echo "unknown test kind: $kind" >&2; exit 2 ;;
esac

shopt -s nullglob
tests=( $pattern )
if ((${#tests[@]} == 0)); then
  printf 'No %s tests exist yet; scaffold check passed.\n' "$kind"
  exit 0
fi

timeout_seconds=30
if [[ "$build_dir" == */sanitize ]]; then
  timeout_seconds=90
fi
for test_bin in "${tests[@]}"; do
  timeout "${timeout_seconds}s" "$test_bin"
done
