#!/usr/bin/env python3
"""Pack the CYW43439's firmware, CLM and NVRAM into the blob the Wi-Fi system loads.

The files are Infineon's, under its Permissive Binary License, as embassy carries them
in its cyw43-firmware directory; nothing of them is kept in this repository.
This fetches them from one commit of embassy into a cache, checks each against its SHA-256,
and lays them out as user/wifi/wifi.h's struct blob_header says:
a header of 64 bytes, then the firmware, the CLM and the NVRAM, each at a multiple of four, zero padded.
tools/rp2350-run.py --ram places the blob at the start of free RAM, where the root task looks for it.

Usage: tools/cyw43-blob.py --cache DIR --out FILE
"""

import argparse
import hashlib
import os
import struct
import sys
import urllib.request

COMMIT = "77d73229cd5eabb4d50276f96dc25704d79586d3"
URL = "https://raw.githubusercontent.com/embassy-rs/embassy/{commit}/cyw43-firmware/{name}"
FILES = {
    "43439A0.bin": "5555e0261da2610a500d68c18d895cace0152bbefbf76f4aa683ebce77e3d7eb",
    "43439A0_clm.bin": "e712b3d218e8b1e2747b092e03b8b0afcb8c8c8e355d2a4a0d47b493800f3f89",
    "nvram_rp2040.bin": "4904bdbb0c937bd0ac2eb2a1d62f2da4dd90e32082384e02874e8d671b0f330d",
    "LICENSE-permissive-binary-license-1.0.txt": None,
}
MAGIC = 0x33345943  # "CY43"
HEADER = 64


def fetch(cache, name, digest):
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        url = URL.format(commit=COMMIT, name=name)
        print(f"cyw43-blob: fetching {url}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    data = open(path, "rb").read()
    if digest is not None and hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f"cyw43-blob: {path} is not the file expected; remove it to fetch it again")
    return data


def pad4(data):
    return data + b"\0" * (-len(data) % 4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    files = {name: fetch(args.cache, name, digest) for name, digest in FILES.items()}
    fw, clm, nvram = files["43439A0.bin"], files["43439A0_clm.bin"], files["nvram_rp2040.bin"]
    fw_off = HEADER
    clm_off = fw_off + len(pad4(fw))
    nvram_off = clm_off + len(pad4(clm))
    size = nvram_off + len(pad4(nvram))
    header = struct.pack("<8I", MAGIC, size, fw_off, len(fw), clm_off, len(clm), nvram_off, len(nvram))
    blob = header.ljust(HEADER, b"\0") + pad4(fw) + pad4(clm) + pad4(nvram)
    assert len(blob) == size
    with open(args.out + ".tmp", "wb") as f:
        f.write(blob)
    os.replace(args.out + ".tmp", args.out)


if __name__ == "__main__":
    main()
