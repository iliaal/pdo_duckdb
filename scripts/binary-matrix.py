#!/usr/bin/env python3
"""Generate the supported binary targets and check complete release sets."""

import argparse
import hashlib
import json
import re
from pathlib import Path


PHP_IMAGES = {
    "8.2": ("8.2.35", "58bd7cb60b690c01106084ce4a0a73eb01094ceb00904ebb4b8514516765292a", "0c0214799ed508e54fcfce246aec540f8d7a5754b0d4b1e935ccff310fcbab6b", "198ed9e076a5c6ad8c17b64c3ff874c276a5e86527a9888f07eee837bc862040"),
    "8.3": ("8.3.36", "cdb77e306d6bb99db40e513fbe036ecc6c53b575cbcb7e581387845fef26d755", "adb3809d01b8b25030248b8ee456cf35fc6266b7faec1d156d59cea9dea3767d", "882668bff8ba882b01e7b2834565323ca24e4b97bec1ff47a948eb226dea0c32"),
    "8.4": ("8.4.27", "496b5e02fe9fe44fcfb715023b931048111dc4e0bc719789d83c906be9f04b05", "4955d421a747523e09ca1b51d05b517f6ae8451ca8a3b9ee9296f28b7c0582d3", "05aa2edde7c3f828fef15b2508edfbac334dda7e1070de56d86a8b2c40b116a6"),
    "8.5": ("8.5.12", "d4f6e10eb393de7e3a7a88e8b88e139072dd3ae297f6a5dada8257e6fe32c585", "be3a5190f1d9c4c1956619b263ce471b7987487038f03fdbddcfc0b99647916c", "dc60e039a84bc0e3d79335497f40b886d4084693b4b2544ff0736972f2f984d6"),
}
PLATFORMS = (
    ("linux", "x86_64", "glibc", "ubuntu-24.04", "linux-amd64", ("nts", "zts")),
    ("linux", "arm64", "glibc", "ubuntu-24.04-arm", "linux-arm64", ("nts", "zts")),
    ("linux", "x86_64", "musl", "ubuntu-24.04", "linux-amd64-musl", ("nts",)),
    ("linux", "arm64", "musl", "ubuntu-24.04-arm", "linux-arm64-musl", ("nts",)),
    ("darwin", "arm64", "bsdlibc", "macos-15", "osx-arm64", ("nts", "zts")),
)


def targets():
    result = []
    for os_name, arch, libc, runner, asset_suffix, flavors in PLATFORMS:
        for php in (*PHP_IMAGES, "8.6"):
            for ts in flavors:
                source_build = php == "8.6"
                image_php = "8.5" if source_build else php
                version, nts_hash, zts_hash, musl_hash = PHP_IMAGES[image_php]
                if libc == "musl":
                    image = f"php:{version}-cli-alpine3.23@sha256:{musl_hash}"
                elif libc == "glibc":
                    image_flavor = "cli" if ts == "nts" or source_build else "zts"
                    image_hash = nts_hash if image_flavor == "cli" else zts_hash
                    image = f"php:{version}-{image_flavor}-bookworm@sha256:{image_hash}"
                else:
                    image = ""
                result.append({
                    "php": php, "ts": ts, "os": os_name, "arch": arch,
                    "libc": libc, "runner": runner,
                    "platform": f"{os_name}-{arch}-{libc}",
                    "asset": f"duckdb-static-libs-{asset_suffix}.tar.gz",
                    "channel": "development" if source_build else "stable",
                    "runtime-version": "8.6.0RC4" if source_build else version,
                    "source-build": source_build, "image": image,
                    "image-key": hashlib.sha256(image.encode()).hexdigest()[:16],
                })
    return result


def package_name(target, version):
    suffix = "-zts" if target["ts"] == "zts" else ""
    return (f"php_pdo_duckdb-{version}_php{target['php']}-{target['arch']}-"
            f"{target['os']}-{target['libc']}{suffix}.zip")


def verify_archives(directory, version):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", version):
        raise ValueError("unsupported package version")
    expected = {package_name(target, version) for target in targets()}
    for php, vs in (("8.2", "16"), ("8.3", "16"), ("8.4", "17"), ("8.5", "17"), ("8.6", "18")):
        for ts in ("nts", "ts"):
            expected.add(f"php_pdo_duckdb-{version}-{php}-{ts}-vs{vs}-x86_64.zip")
    paths = list(directory.rglob("*.zip"))
    actual = {path.name for path in paths}
    if len(paths) != len(actual):
        raise ValueError("duplicate archive names across uploaded artifacts")
    if actual != expected:
        raise ValueError(f"release archive mismatch; missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}")
    print(f"Verified all {len(expected)} requested binary archive names")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("matrix")
    commands.add_parser("engines")
    verify = commands.add_parser("verify-archives")
    verify.add_argument("--directory", required=True, type=Path)
    verify.add_argument("--version", required=True)
    arguments = parser.parse_args()
    if arguments.command == "verify-archives":
        try:
            verify_archives(arguments.directory, arguments.version)
        except ValueError as error:
            parser.exit(1, f"{error}\n")
    elif arguments.command == "engines":
        print(json.dumps({"asset": sorted({target["asset"] for target in targets()})}, separators=(",", ":")))
    else:
        print(json.dumps({"include": targets()}, separators=(",", ":")))


if __name__ == "__main__":
    main()
