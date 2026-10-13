#!/bin/bash
# Install what `make check` needs in a Claude Code cloud session:
# QEMU for RISC-V and for ARM, and clang 22 with lld, LLVM's tools and the fuzzer and sanitizer runtimes.
# The records of `tests/bench/` hold for the clang that wrote them, 22, and Ubuntu's is older,
# so clang comes from apt.llvm.org and its tools are linked into /usr/local/bin, ahead of /usr/bin.
# It installs only what is missing, so a cached container starts at once.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi
export DEBIAN_FRONTEND=noninteractive

qemu=()
command -v qemu-system-riscv32 >/dev/null || qemu+=(qemu-system-misc)
command -v qemu-system-arm >/dev/null || qemu+=(qemu-system-arm)
if [ ${#qemu[@]} -gt 0 ]; then
  apt-get update -q
  apt-get install -y -q --no-install-recommends "${qemu[@]}"
fi

LLVM=22
bin=/usr/lib/llvm-$LLVM/bin
key=/etc/apt/keyrings/apt.llvm.org.asc

# One link answers for clang, lld and the fuzzer's runtime, wherever this version keeps it.
links() {
  [ -x "$bin/llvm-objdump" ] &&
    echo 'int LLVMFuzzerTestOneInput(const char *d, unsigned long n) { return 0; }' |
    "$bin/clang" -fsanitize=fuzzer -fuse-ld=lld -x c - -o /dev/null 2>/dev/null
}

if ! links; then
  . /etc/os-release
  if ! curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o "$key"; then
    echo "apt.llvm.org is out of reach, so this session has no clang $LLVM:" \
      "make bench misses the ARM boards' records until the environment's network lets it in" >&2
    exit 0
  fi
  echo "deb [signed-by=$key] http://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-$LLVM main" \
    >/etc/apt/sources.list.d/llvm-$LLVM.list
  apt-get update -q
  apt-get install -y -q --no-install-recommends "clang-$LLVM" "lld-$LLVM" "llvm-$LLVM" "libclang-rt-$LLVM-dev"
fi

ln -sf "$bin"/{clang,clang++,ld.lld,lld,llvm-*} /usr/local/bin/
