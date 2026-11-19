#!/usr/bin/env python3
"""Generate the supported binary targets and check complete release sets."""

import argparse
import hashlib
import json
import re
from pathlib import Path


PHP_IMAGES = {
    "8.2": ("8.2.36", "0a5ed0098915c11e1db8729b81bdb9a1bf12a998cc5e5b2f9a640afb6104dbe1", "d2bb14b56719d178369006200c4b1433a6f02738fb62b5359ea47e39a8790edc", "3de318631b4a0f3bbd31240871fceaa4f6c7e86e7be1c29baaf7ab6bbd30af74"),
    "8.3": ("8.3.37", "a5edff11964286c30cd7dc5c29618624b6dac8b6583697ea6236a2dce44f1e2c", "2db4cff975a701e06a9d1a655101212c6c8bcf3872c205d7b099a0914e922f7e", "03c0c9c3a1cf15eac1e6097710e668a3e9414f070b740ec814a04d415e76470b"),
    "8.4": ("8.4.28", "85da896b3eeb8bcd83fe612e51a529b7d5983a1fab92ef5f63c7ac6e2842fc46", "bffe87204f1fc00507a259494c5dd2ea5cb95e828f4c6f8f24e3b6d67c8bb0a1", "0bfae5e3a982afc3a182edb4a79e9bedebf01823f57c1762c23778fa061d57e0"),
    "8.5": ("8.5.13", "d6991172c4282d2a09d6418073923a81664173bb0a0e07a16663a9a645746092", "25cea2c356f20545f828c9d987a0d0d9ce9ee597103eef699b2a05ba39466d93", "422f6b0307a9742e531c828c033f524b612548d2d9e8e67551a88b21609d0a18"),
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
                    "channel": "stable",
                    "runtime-version": "8.6.0" if source_build else version,
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
