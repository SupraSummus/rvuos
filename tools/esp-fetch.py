#!/usr/bin/env python3
"""Fetch Espressif's closed libraries for the ESP32-C6 and what of ESP-IDF they need, never into the tree.

The PHY's libphy.a, the ROM linker scripts of ESP-IDF it calls the ROM through,
and ESP-IDF's phy_init_data.c are fetched file by file, about 225 KB, from one commit each,
into a cache outside the tree, and checked against their SHA-256.
The init data is written as a C file of its 128 bytes,
with ESP-IDF's default maximum TX power put into the entries that take it;
user/phyblob/ links against these.

With --wifi, the two Wi-Fi libraries are fetched too, about 2.5 MB,
with the ROM linker scripts for the Wi-Fi functions the ROM holds
and ESP-IDF's tables of the channels each country allows, esp_wifi_regulatory.c,
and the BT baseband's libbtbb.a from the PHY's repository, about 20 KB, which brings the radio up for IEEE 802.15.4;
the Wi-Fi system's driver for the ESP32-C6 links against these.
It runs WPA with hostap's supplicant, upstream's own and not ESP-IDF's fork of it,
so --wifi also fetches wpa_supplicant's release, about 4.2 MB, checks it,
and unpacks into hostap/ of the cache only the parts of src/ the driver builds, about 5 MB,
with the license, BSD, beside them.
WPA3's SAE needs elliptic curves, which hostap's own crypto lacks, so --wifi also fetches Mbed TLS, about 5.5 MB,
of the 3.6 branch, the last whose big numbers and curves are public, and unpacks its include/ and library/ into mbedtls/;
the driver takes it under Apache-2.0 rather than the GPL, which the closed libraries beside it would not allow.

With --rom-elf it writes the ESP32-C6's ROM as an ELF with its symbols, which tools/phymap.py reads.
Espressif publishes it only in a release of every chip's ROM, about 4.9 MB,
which is checked and unpacked in memory, so only the chip's 490 KB are kept.

With --chip esp32c5 it fetches the ESP32-C5's instead, from the commits ESP-IDF v5.5.4 pins:
libphy.a, the ROM's linker scripts and the init data, and with --wifi the Wi-Fi libraries, libcoexist.a and libbtbb.a,
which the radio's programs of user/radio/esp32c5/ link while the radio's code there is not yet its own,
or read as the libraries' half of what the code does; no hostap nor Mbed TLS, which the ESP32-C6's fetch keeps.
Its ROM is the revision the boards have, v1.0's, esp32c5_rev100_rom.elf.

Usage: tools/esp-fetch.py [--chip esp32c6|esp32c5] --cache DIR --init-data FILE [--wifi]
       tools/esp-fetch.py [--chip esp32c6|esp32c5] --rom-elf FILE
"""

import argparse
import hashlib
import io
import os
import re
import shutil
import sys
import tarfile
import urllib.request

IDF_COMMIT = "4d59230ddff16327812782151ef0afef202dc6d7"
PHY_LIB = "https://raw.githubusercontent.com/espressif/esp-phy-lib/20f1db053a0e6cb9f1c09d255c43bf42483041d0/esp32c6/"
ROM_LD = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_COMMIT}/components/esp_rom/esp32c6/ld/"
ESP_PHY = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_COMMIT}/components/esp_phy/esp32c6/"
ESP_WIFI = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_COMMIT}/components/esp_wifi/"

WIFI_LIB = "https://raw.githubusercontent.com/espressif/esp32-wifi-lib/af55a0ca258ce9d791d1661d7c2bbb65f08c0c21/esp32c6/"

FILES = {
    PHY_LIB + "libphy.a": "5ebda577864f5e90a34d90317360a5957d6c5ba133446461d697d65297caf04e",
    ROM_LD + "esp32c6.rom.ld": "e2174360047d1df6a963d3bf49271755fd23b72a65ac85b9ed14aee16035e2f3",
    ROM_LD + "esp32c6.rom.phy.ld": "7332fd77b2aeb34d5d9881e86e26a7521f2b315b3ad4116082f92eb1b6a19f35",
    ROM_LD + "esp32c6.rom.libgcc.ld": "6f601e0d26b1c9e46160edab23fad21d73fe3a1a6e0deb616906604609a0c680",
    ROM_LD + "esp32c6.rom.libc.ld": "3c1e96e7c6796f9554cd92f8c13914c318712ea2dc91489b34e40c1dc4b91493",
    ESP_PHY + "phy_init_data.c": "16561a1b508f9ebbf9684b37d3d75dbbcfe4eb64ab5653efc9dfcbaa38bb9aa3",
}

WIFI_FILES = {
    WIFI_LIB + "libnet80211.a": "40c03728cf922d5ee70d0bd78c3573da3ee06b448458f6357117c2df24d0c113",
    WIFI_LIB + "libpp.a": "c155f4bf97fda9f2f1c4f72e827d39f490a3aa39a26aaab54b6bd587af64380b",
    PHY_LIB + "libbtbb.a": "1843c31dbd6abfabe33439882bf1ad4ffa5ac60d885da98040c0d909872187e2",
    ROM_LD + "esp32c6.rom.pp.ld": "421e8f9a3f0d3dd11d351398f6e48f0c66ad5b6a350583df400098f44f59ec3b",
    ROM_LD + "esp32c6.rom.net80211.ld": "4acdeceed6d2229367cd18ecf122ec29c257be50340b1777f589426f088fa337",
    ESP_WIFI + "regulatory/esp_wifi_regulatory.c": "54a664aa696e583352c3865bc441a837828b8b0ce26f09015583f43d2f24ff51",
}

HOSTAP = "https://w1.fi/releases/wpa_supplicant-2.12.tar.gz"
HOSTAP_SHA256 = "08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6"
HOSTAP_TOP = "wpa_supplicant-2.12/"
# The directories of src/ whose files the driver builds or includes, and the few headers it includes from others.
HOSTAP_PARTS = ("src/utils/", "src/common/", "src/crypto/", "src/rsn_supp/",
                "src/drivers/driver.h", "src/eapol_supp/eapol_supp_sm.h", "src/eap_common/eap_defs.h",
                "COPYING", "README")

MBEDTLS = "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
MBEDTLS_TOP = "mbedtls-3.6.7/"
MBEDTLS_PARTS = ("include/mbedtls/", "include/psa/", "library/", "LICENSE", "README.md")

ROM_ELFS = "https://github.com/espressif/esp-rom-elfs/releases/download/20260528/esp-rom-elfs-20260528.tar.gz"
ROM_ELFS_SHA256 = "caa463d3cbef2430a5a35847c1d9f2f152403b17a802050927ff60c8da54fe46"
ROM_ELF = "esp32c6_rev0_rom.elf"
ROM_ELF_SHA256 = "788e1d38724aeb8fd974fa10c4a7b089c02627d35342ce84b9e0b12b239f3551"

IDF_C5_COMMIT = "735507283d5b2f9fb363a1901172dbd9e847945d"  # v5.5.4
PHY_LIB_C5 = "https://raw.githubusercontent.com/espressif/esp-phy-lib/3d57415af6e4c92eff2c4c3463e20a51d7340aba/esp32c5/"
WIFI_LIB_C5 = "https://raw.githubusercontent.com/espressif/esp32-wifi-lib/b87cce81803b85c3639fb14bac6c24cff6d0fbad/esp32c5/"
COEX_LIB_C5 = "https://raw.githubusercontent.com/espressif/esp-coex-lib/ee5cd79583c02f23e43e62931ffb55f5a4992d0f/esp32c5/"
ROM_LD_C5 = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_C5_COMMIT}/components/esp_rom/esp32c5/ld/"
ESP_PHY_C5 = f"https://raw.githubusercontent.com/espressif/esp-idf/{IDF_C5_COMMIT}/components/esp_phy/esp32c5/"

FILES_C5 = {
    PHY_LIB_C5 + "libphy.a": "694c5df5679ae7bf503fe2d0a7e889873b65a37b459e4ce177904847a47ef80f",
    ROM_LD_C5 + "esp32c5.rom.ld": "637024ecbece1dd8c8d7650f6f70e80a187fef8b06d8b6338a0a028979c47513",
    ROM_LD_C5 + "esp32c5.rom.phy.ld": "58feb442aa4b1cc48377feaada8de39baaca6dcbfe3112ceed95af4905716bae",
    ROM_LD_C5 + "esp32c5.rom.libgcc.ld": "009d22abadc18ff4e7483038ae197bd2cdb240e16a099616cbdb94a829017de6",
    ROM_LD_C5 + "esp32c5.rom.libc.ld": "cb6a67941f5cd62066827bdff89f9646f2a39b5f3ecd5ba70f26dbc2e4c598e8",
    ROM_LD_C5 + "esp32c5.rom.version.ld": "23cd17dd0fd04233ab90032374de646e265d76122f4b6afbcbea75963de12b88",
    ESP_PHY_C5 + "phy_init_data.c": "60466e0d24d125e06d5fd7352dc28d30310abbcfdeb0a7eca3d8baf545caa80e",
}

WIFI_FILES_C5 = {
    WIFI_LIB_C5 + "libnet80211.a": "607edde25d05e62d9b18de24994bf5cb510637de0591ec9bd7ddd9b6e27d44a9",
    WIFI_LIB_C5 + "libpp.a": "de09094403c30902db13b0b244297aceba4ce0cc188377bdea09c5516c5c9c96",
    COEX_LIB_C5 + "libcoexist.a": "2159c4932970566ced6984a8012a775767cac0e6609563dd4f183b7172b86fa7",
    PHY_LIB_C5 + "libbtbb.a": "622115b666882a0179037aeeb62f4f4b9dd327edd0d9dfba9f784c30553f39fe",
    ROM_LD_C5 + "esp32c5.rom.pp.ld": "d0e252f19ea67db95babca89dc9992bb21d43ea8ab3081a192d71e21fea3dc30",
    ROM_LD_C5 + "esp32c5.rom.net80211.ld": "d90a707ee09856cbdf2af472114be66186964300fe82a3a1a3748418352767cb",
    ROM_LD_C5 + "esp32c5.rom.coexist.ld": "e0848384d75e0de97dd9c29ee2ccc2723dfdf8779c4b6bbb6f8d3e3eb383fd05",
}

ROM_ELF_C5 = "esp32c5_rev100_rom.elf"
ROM_ELF_C5_SHA256 = "49693d745eb4b4e6e6c77be4f5213765d545d8c7a1d9742a8a619bf596c4e27a"

# What each chip fetches: its files, its Wi-Fi's, its init data's source and commit, its ROM's ELF, and whether
# hostap and Mbed TLS come with its Wi-Fi.
CHIPS = {
    "esp32c6": (lambda: (FILES, WIFI_FILES, ESP_PHY, IDF_COMMIT, ROM_ELF, ROM_ELF_SHA256, True)),
    "esp32c5": (lambda: (FILES_C5, WIFI_FILES_C5, ESP_PHY_C5, IDF_C5_COMMIT, ROM_ELF_C5, ROM_ELF_C5_SHA256, False)),
}

# CONFIG_ESP_PHY_MAX_WIFI_TX_POWER's default in ESP-IDF's components/esp_phy/Kconfig, in dBm.
MAX_TX_POWER = 20


def fetch(cache, url, digest):
    path = os.path.join(cache, url.rsplit("/", 1)[1])
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        print(f"esp-fetch: fetching {url}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    data = open(path, "rb").read()
    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f"esp-fetch: {path} is not the file expected; remove it to fetch it again")
    return data


def rom_elf(path, name=ROM_ELF, digest=ROM_ELF_SHA256):
    if not os.path.exists(path):
        print(f"esp-fetch: fetching {ROM_ELFS}", file=sys.stderr)
        with urllib.request.urlopen(ROM_ELFS, timeout=60) as r:
            release = r.read()
        if hashlib.sha256(release).hexdigest() != ROM_ELFS_SHA256:
            sys.exit(f"esp-fetch: {ROM_ELFS} is not the release expected")
        with tarfile.open(fileobj=io.BytesIO(release)) as tar:
            data = tar.extractfile(name).read()
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
    if hashlib.sha256(open(path, "rb").read()).hexdigest() != digest:
        sys.exit(f"esp-fetch: {path} is not the ROM expected; remove it to fetch it again")


def unpack(cache, subdir, url, digest, top, parts):
    """Unpacks the parts of a release's top directory into cache/subdir/, unless it is there."""
    dest = os.path.join(cache, subdir)
    if os.path.isdir(dest):
        return
    print(f"esp-fetch: fetching {url}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=60) as r:
        release = r.read()
    if hashlib.sha256(release).hexdigest() != digest:
        sys.exit(f"esp-fetch: {url} is not the release expected")
    # Unpacked aside and moved in whole, so that a directory there is a whole one.
    shutil.rmtree(dest + ".tmp", ignore_errors=True)
    with tarfile.open(fileobj=io.BytesIO(release)) as tar:
        for m in tar.getmembers():
            name = m.name[len(top):] if m.name.startswith(top) else None
            if not m.isfile() or not name or not any(name.startswith(p) for p in parts):
                continue
            path = os.path.join(dest + ".tmp", name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(tar.extractfile(m).read())
    os.replace(dest + ".tmp", dest)


def entry(text):
    """One entry of phy_init_data's initializer: a number, or a LIMIT of the maximum TX power."""
    m = re.fullmatch(r"LIMIT\(CONFIG_ESP_PHY_MAX_TX_POWER \* 4, (\w+), (\w+)\)", text)
    if m:
        return max(int(m.group(1), 0), min(MAX_TX_POWER * 4, int(m.group(2), 0)))
    if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", text):
        return int(text, 0)
    sys.exit(f"esp-fetch: an init data entry this does not read: {text}")


def init_data(source, count=128):
    m = re.search(r"const esp_phy_init_data_t phy_init_data\s*=\s*\{\s*\{(.*?)\}\s*\};", source, re.S)
    if not m:
        sys.exit("esp-fetch: phy_init_data.c holds no phy_init_data")
    # The commas between entries, not those inside a LIMIT's parentheses.
    values = [entry(e.strip()) for e in re.split(r",(?![^(]*\))", m.group(1)) if e.strip()]
    if len(values) != count:
        sys.exit(f"esp-fetch: phy_init_data has {len(values)} entries, not {count}")
    return values


def write_init_data(cache, path, wifi, chip="esp32c6"):
    files, wifi_files, esp_phy, commit, _, _, supplicant = CHIPS[chip]()
    fetched = {url: fetch(cache, url, digest) for url, digest in (files | wifi_files if wifi else files).items()}
    if wifi and supplicant:
        unpack(cache, "hostap", HOSTAP, HOSTAP_SHA256, HOSTAP_TOP, HOSTAP_PARTS)
        unpack(cache, "mbedtls", MBEDTLS, MBEDTLS_SHA256, MBEDTLS_TOP, MBEDTLS_PARTS)
    # ESP-IDF's esp_phy_init_data_t: 256 bytes on the ESP32-C5, which has a second band's powers, 128 elsewhere.
    count = 256 if chip == "esp32c5" else 128
    values = init_data(fetched[esp_phy + "phy_init_data.c"].decode(), count)
    rows = ",\n".join("    " + ", ".join(f"0x{v:02x}" for v in values[i:i + 8]) for i in range(0, count, 8))
    with open(path + ".tmp", "w") as f:
        f.write(f"/* Written by tools/esp-fetch.py from ESP-IDF {commit[:8]}'s phy_init_data.c "
                f"with a maximum TX power of {MAX_TX_POWER} dBm. */\n\n"
                "#include <stdint.h>\n\n"
                f"const struct {{\n    uint8_t params[{count}];\n}} phy_init_data = {{{{\n{rows}\n}}}};\n")
    os.replace(path + ".tmp", path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache")
    ap.add_argument("--init-data")
    ap.add_argument("--rom-elf")
    ap.add_argument("--wifi", action="store_true")
    ap.add_argument("--chip", choices=sorted(CHIPS), default="esp32c6")
    args = ap.parse_args()
    if args.init_data and not args.cache or not (args.init_data or args.rom_elf):
        ap.error("--init-data with --cache, or --rom-elf")
    if args.rom_elf:
        _, _, _, _, name, digest, _ = CHIPS[args.chip]()
        rom_elf(args.rom_elf, name, digest)
    if args.init_data:
        write_init_data(args.cache, args.init_data, args.wifi, args.chip)


if __name__ == "__main__":
    main()
