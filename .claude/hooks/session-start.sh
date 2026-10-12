#!/bin/bash
# Install what `make check` needs in a Claude Code cloud session:
# QEMU for RISC-V and for ARM, and clang 22 with lld, LLVM's tools and the fuzzer and sanitizer runtimes.
# The link checks and `make bench` read what clang makes, and the records were written with 22,
# which the distribution's own clang is not, so it comes from apt.llvm.org
# and its tools go first on the PATH, through /usr/local/bin.
# It installs only what is missing, so a cached container starts at once.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

LLVM=22
llvm_bin=/usr/lib/llvm-$LLVM/bin

packages=()
command -v qemu-system-riscv32 >/dev/null || packages+=(qemu-system-misc)
command -v qemu-system-arm >/dev/null || packages+=(qemu-system-arm)
[ -x "$llvm_bin/clang" ] || packages+=("clang-$LLVM" "lld-$LLVM" "llvm-$LLVM")
[ -e "$("$llvm_bin/clang" --print-runtime-dir 2>/dev/null)/libclang_rt.fuzzer-$(uname -m).a" ] \
  || packages+=("libclang-rt-$LLVM-dev")

if [ ${#packages[@]} -gt 0 ]; then
  export DEBIAN_FRONTEND=noninteractive
  if [[ " ${packages[*]} " == *"-$LLVM"* ]] && [ ! -e /etc/apt/sources.list.d/llvm-$LLVM.list ]; then
    . /etc/os-release
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc
    echo "deb http://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-$LLVM main" \
      >/etc/apt/sources.list.d/llvm-$LLVM.list
  fi
  apt-get update -q
  apt-get install -y -q --no-install-recommends "${packages[@]}"
fi

for tool in "$llvm_bin"/clang "$llvm_bin"/clang++ "$llvm_bin"/ld.lld "$llvm_bin"/lld "$llvm_bin"/llvm-*; do
  ln -sf "$tool" /usr/local/bin/
done
