#!/usr/bin/env python3
"""Generate the supported binary targets and check complete release sets."""

import argparse
import hashlib
import json
import re
from pathlib import Path


PHP_IMAGES = {
    "8.2": ("8.2.35", "ce09ca1d4b532edf9b0347b44a1968a80d9175addb581c450ba0c9a43fa0c5ea", "025d4148dcdc12a5def5fa947066be9b802111d52ddb2eae323247b3c1d51313", "e61209a54196578562e251cb01c3b154a399e9d37a85462a07f528c8fc85d431"),
    "8.3": ("8.3.36", "232331112d3a7c5b98edeeddc3b85f610172f99352c0635fe6b3e166e47062cb", "47d3c81c4f256ce9070e129648fb210d5ba2c968aa89d264024b70acdab569a1", "76408934b0d5238f5f80ad7f7a9f0c5daa6aac822110bb6096bd1e3cb39cc0c9"),
    "8.4": ("8.4.27", "b3e907a1108629c562c4bb313ce3449e34d49991f6bb2487913f6146f47200fe", "dc0b99cf3abb6c9dfae4efa4cfe9a089449e43f1800b1e29b45746d47e8f9cbc", "396f7daf51a4902af9e72e9e017727b33dc2bd96d42d94e9acf6c8729bddb40b"),
    "8.5": ("8.5.12", "050713ce008e2360ea9dc96e1f9a075690132388d917fc3552806f7a7be6f2a0", "c6a2606c831a21992c2cdd0ae5a4e87af02348c4c3f66621bfb8b87702977ee3", "c135cf34aed94ecaa22e3592af4511fe19254b1fe39b99c94125915fee6a7449"),
}
# Each series builds on the oldest patch that exports every engine symbol the
# module imports (the f-variant allocators and hash updaters), because the
# module refuses to load on a runtime older than its build headers.
# setup-php only offers the newest patch on macOS, so macOS builds these
# releases from source; the checksums are php.net's published values.
PHP_SOURCES = {
    "8.2.35": "5737b98601fe5ab1b8c5db0c82ed3158acb9283ed3081ffe2faffb9ac81455e6",
    "8.3.36": "dd54693aa334c5c4d72e9aee96b082c2473750a18a6c1d105789c93a3b1692fd",
    "8.4.27": "dcf93923b0e38e0d5252b25b796520b5a47761a08b3488b6d0b5d1115764d3f0",
    "8.5.12": "08bc2eba2a588654ade1357174aa646b761556c862dda0678345ee29ed481209",
    "8.6.0": "5f84beaa3d763aee1e4feed99bf54ec692c383b322e25626807b589238c3a087",
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
                source_build = php == "8.6" or os_name == "darwin"
                image_php = "8.5" if php == "8.6" else php
                version, nts_hash, zts_hash, musl_hash = PHP_IMAGES[image_php]
                runtime_version = "8.6.0" if php == "8.6" else version
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
                    "runtime-version": runtime_version,
                    "source-build": source_build, "image": image,
                    "source-url": f"https://www.php.net/distributions/php-{runtime_version}.tar.xz" if source_build else "",
                    "source-sha256": PHP_SOURCES[runtime_version] if source_build else "",
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
