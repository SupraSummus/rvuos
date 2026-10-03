#!/usr/bin/env python3
"""Fetch what the PHY harness of user/phyblob/ links against, and write its init data.

Espressif's libphy.a for the ESP32-C6, the ROM linker scripts of ESP-IDF it calls the ROM through,
and ESP-IDF's phy_init_data.c are fetched file by file, about 225 KB, from one commit each,
into a cache outside the tree, and checked against their SHA-256.
The init data is written as a C file of its 128 bytes,
with ESP-IDF's default maximum TX power put into the entries that take it.

With --rom-elf it writes the ESP32-C6's ROM as an ELF with its symbols, which tools/phymap.py reads.
Espressif publishes it only in a release of every chip's ROM, about 4.9 MB,
which is checked and unpacked in memory, so only the chip's 490 KB are kept.

Usage: tools/phyblob-fetch.py --cache DIR --init-data FILE
       tools/phyblob-fetch.py --rom-elf FILE
"""

import argparse
import hashlib
import io
import os
import re
import sys
import tarfile
import urllib.request

IDF_COMMIT = "4d59230ddff16327812782151ef0afef202dc6d7"
PHY_LIB = "https://raw.githubusercontent.com/espressif/esp-phy-lib/20f1db053a0e6cb9f1c09d255c43bf42483041d0/esp32c6/"
ROM_LD = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_COMMIT}/components/esp_rom/esp32c6/ld/"
ESP_PHY = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_COMMIT}/components/esp_phy/esp32c6/"

FILES = {
    PHY_LIB + "libphy.a": "5ebda577864f5e90a34d90317360a5957d6c5ba133446461d697d65297caf04e",
    ROM_LD + "esp32c6.rom.ld": "e2174360047d1df6a963d3bf49271755fd23b72a65ac85b9ed14aee16035e2f3",
    ROM_LD + "esp32c6.rom.phy.ld": "7332fd77b2aeb34d5d9881e86e26a7521f2b315b3ad4116082f92eb1b6a19f35",
    ROM_LD + "esp32c6.rom.libgcc.ld": "6f601e0d26b1c9e46160edab23fad21d73fe3a1a6e0deb616906604609a0c680",
    ROM_LD + "esp32c6.rom.libc.ld": "3c1e96e7c6796f9554cd92f8c13914c318712ea2dc91489b34e40c1dc4b91493",
    ESP_PHY + "phy_init_data.c": "16561a1b508f9ebbf9684b37d3d75dbbcfe4eb64ab5653efc9dfcbaa38bb9aa3",
}

ROM_ELFS = "https://github.com/espressif/esp-rom-elfs/releases/download/20260528/esp-rom-elfs-20260528.tar.gz"
ROM_ELFS_SHA256 = "caa463d3cbef2430a5a35847c1d9f2f152403b17a802050927ff60c8da54fe46"
ROM_ELF = "esp32c6_rev0_rom.elf"
ROM_ELF_SHA256 = "788e1d38724aeb8fd974fa10c4a7b089c02627d35342ce84b9e0b12b239f3551"

# CONFIG_ESP_PHY_MAX_WIFI_TX_POWER's default in ESP-IDF's components/esp_phy/Kconfig, in dBm.
MAX_TX_POWER = 20


def fetch(cache, url, digest):
    path = os.path.join(cache, url.rsplit("/", 1)[1])
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        print(f"phyblob-fetch: fetching {url}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    data = open(path, "rb").read()
    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f"phyblob-fetch: {path} is not the file expected; remove it to fetch it again")
    return data


def rom_elf(path):
    if not os.path.exists(path):
        print(f"phyblob-fetch: fetching {ROM_ELFS}", file=sys.stderr)
        with urllib.request.urlopen(ROM_ELFS, timeout=60) as r:
            release = r.read()
        if hashlib.sha256(release).hexdigest() != ROM_ELFS_SHA256:
            sys.exit(f"phyblob-fetch: {ROM_ELFS} is not the release expected")
        with tarfile.open(fileobj=io.BytesIO(release)) as tar:
            data = tar.extractfile(ROM_ELF).read()
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    if hashlib.sha256(open(path, "rb").read()).hexdigest() != ROM_ELF_SHA256:
        sys.exit(f"phyblob-fetch: {path} is not the ROM expected; remove it to fetch it again")


def entry(text):
    """One entry of phy_init_data's initializer: a number, or a LIMIT of the maximum TX power."""
    m = re.fullmatch(r"LIMIT\(CONFIG_ESP_PHY_MAX_TX_POWER \* 4, (\w+), (\w+)\)", text)
    if m:
        return max(int(m.group(1), 0), min(MAX_TX_POWER * 4, int(m.group(2), 0)))
    if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", text):
        return int(text, 0)
    sys.exit(f"phyblob-fetch: an init data entry this does not read: {text}")


def init_data(source):
    m = re.search(r"const esp_phy_init_data_t phy_init_data\s*=\s*\{\s*\{(.*?)\}\s*\};", source, re.S)
    if not m:
        sys.exit("phyblob-fetch: phy_init_data.c holds no phy_init_data")
    # The commas between entries, not those inside a LIMIT's parentheses.
    values = [entry(e.strip()) for e in re.split(r",(?![^(]*\))", m.group(1)) if e.strip()]
    if len(values) != 128:
        sys.exit(f"phyblob-fetch: phy_init_data has {len(values)} entries, not 128")
    return values


def write_init_data(cache, path):
    fetched = {url: fetch(cache, url, digest) for url, digest in FILES.items()}
    values = init_data(fetched[ESP_PHY + "phy_init_data.c"].decode())
    rows = ",\n".join("    " + ", ".join(f"0x{v:02x}" for v in values[i:i + 8]) for i in range(0, 128, 8))
    with open(path + ".tmp", "w") as f:
        f.write(f"/* Written by tools/phyblob-fetch.py from ESP-IDF {IDF_COMMIT[:8]}'s phy_init_data.c "
                f"with a maximum TX power of {MAX_TX_POWER} dBm. */\n\n"
                "#include <stdint.h>\n\n"
                f"const struct {{\n    uint8_t params[128];\n}} phy_init_data = {{{{\n{rows}\n}}}};\n")
    os.replace(path + ".tmp", path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache")
    ap.add_argument("--init-data")
    ap.add_argument("--rom-elf")
    args = ap.parse_args()
    if args.init_data and not args.cache or not (args.init_data or args.rom_elf):
        ap.error("--init-data with --cache, or --rom-elf")
    if args.rom_elf:
        rom_elf(args.rom_elf)
    if args.init_data:
        write_init_data(args.cache, args.init_data)


if __name__ == "__main__":
    main()
