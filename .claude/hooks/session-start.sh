#!/bin/bash
# Install what `make check` needs in a Claude Code cloud session:
# QEMU for RISC-V, and clang's fuzzer and sanitizer runtimes for the host build.
# It installs only what is missing, so a cached container starts at once.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

packages=()
command -v qemu-system-riscv32 >/dev/null || packages+=(qemu-system-misc)
[ -e "$(clang --print-runtime-dir)/libclang_rt.fuzzer-$(uname -m).a" ] \
  || packages+=("libclang-rt-$(clang -dumpversion | cut -d. -f1)-dev")

if [ ${#packages[@]} -eq 0 ]; then
  exit 0
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends "${packages[@]}"
