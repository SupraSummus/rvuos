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
  if curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o "$key"; then
    echo "deb [signed-by=$key] http://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-$LLVM main" \
      >/etc/apt/sources.list.d/llvm-$LLVM.list
    apt-get update -q
    apt-get install -y -q --no-install-recommends "clang-$LLVM" "lld-$LLVM" "llvm-$LLVM" "libclang-rt-$LLVM-dev"
  else
    echo "apt.llvm.org is out of reach, so this session has no clang $LLVM:" \
      "make bench misses the ARM boards' records until the environment's network lets it in" >&2
  fi
fi
if links; then
  ln -sf "$bin"/{clang,clang++,ld.lld,lld,llvm-*} /usr/local/bin/
fi

# Lean, for the model in model/ that make model-test builds and holds the host build to:
# elan, and the toolchain model/lean-toolchain names, taken from the release on GitHub and linked into elan,
# since elan's own download, from release.lean-lang.org, is not let in everywhere GitHub is.
if ! command -v lake >/dev/null; then
  if ! curl -fsSL https://raw.githubusercontent.com/leanprover/elan/master/elan-init.sh -o /tmp/elan-init.sh; then
    echo "elan is out of reach, so this session has no Lean: make model-test cannot run" >&2
    exit 0
  fi
  sh /tmp/elan-init.sh -y --default-toolchain none
  ln -sf "$HOME"/.elan/bin/{elan,lake,lean} /usr/local/bin/
fi
model=$(cd "$(dirname "$0")/../../model" && pwd)
toolchain=$(cat "$model/lean-toolchain")
version=${toolchain#*:v}
if ! elan toolchain list | grep -qx "$toolchain"; then
  command -v zstd >/dev/null || apt-get install -y -q --no-install-recommends zstd
  dir=/opt/lean/lean-$version-linux
  mkdir -p /opt/lean
  curl -fsSL "https://github.com/leanprover/lean4/releases/download/v$version/lean-$version-linux.tar.zst" |
    tar --zstd -x -C /opt/lean
  elan toolchain link "$toolchain" "$dir"
fi
(cd "$model" && lake --version >/dev/null)
