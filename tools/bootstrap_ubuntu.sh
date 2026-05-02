#!/usr/bin/env bash
set -euo pipefail

packages=(build-essential clang clang-format clang-tidy cppcheck gdb valgrind python3 python3-venv python3-pip iproute2 ethtool tcpdump jq bc)

if [[ "${1:-}" == "--check" ]]; then
  missing=()
  for command_name in gcc make python3 tc ip clang-format cppcheck; do
    command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
  done
  if ((${#missing[@]})); then
    printf 'Missing commands: %s\n' "${missing[*]}"
    printf 'Run tools/bootstrap_ubuntu.sh in Ubuntu/WSL to install prerequisites.\n'
    exit 1
  fi
  printf 'Ubuntu toolchain prerequisites are available.\n'
  exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
  sudo apt-get update
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"
else
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"
fi

printf 'Dependencies installed. Verify with: make configure\n'
