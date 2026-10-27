#!/usr/bin/env python3
"""Stage and verify exact DuckDB C++ headers for the optional native bridge."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


class SourceError(Exception):
    pass


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def identity(version, source_id):
    match = re.fullmatch(r"v([12])\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?", version) if isinstance(version, str) else None
    if not match:
        raise SourceError("version must be an explicit v1.x.y or v2.x.y engine version")
    if not isinstance(source_id, str) or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", source_id):
        raise SourceError("source ID must be a full lowercase Git commit hash")
    return {"version": version, "source_id": source_id, "engine_major": int(match[1])}


def build_header(info):
    return (
        "#ifndef PDO_DUCKDB_BRIDGE_BUILD_H\n#define PDO_DUCKDB_BRIDGE_BUILD_H\n"
        f'#define PDO_DUCKDB_BRIDGE_EXPECTED_VERSION "{info["version"]}"\n'
        f'#define PDO_DUCKDB_BRIDGE_EXPECTED_SOURCE_ID "{info["source_id"][:10]}"\n'
        f'#define PDO_DUCKDB_BRIDGE_ENGINE_MAJOR {info["engine_major"]}\n'
        "#endif\n"
    ).encode("ascii")


def safe_parts(name):
    parts = name.rstrip("/").split("/")
    if not name or name.startswith("/") or any(
        part in ("", ".", "..") or part.endswith((".", " "))
        or any(character in '\\:<>"|?*' or ord(character) < 32 for character in part)
        or re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³]", part.split(".")[0].rstrip(" "), re.IGNORECASE)
        for part in parts
    ):
        raise SourceError(f"unsafe archive/cache path: {name!r}")
    return tuple(parts)


def validate_link(parts, target, root, hardlink=False):
    if not target or target.startswith("/") or "\\" in target or ":" in target or "\0" in target:
        raise SourceError(f"unsafe archive link: {'/'.join(parts)!r}")
    resolved = [] if hardlink else list(parts[:-1])
    for part in target.split("/"):
        if part == "..":
            if not resolved:
                raise SourceError("archive link escapes its source root")
            resolved.pop()
        elif part not in ("", "."):
            resolved.append(part)
    if tuple(resolved[:len(root)]) != root:
        raise SourceError("archive link escapes its source root")


def source_destination(parts):
    if parts[:2] == ("src", "include") and len(parts) > 2:
        return ("include",) + parts[2:]
    if parts[:3] == ("third_party", "fmt", "include") and len(parts) > 3:
        return ("include",) + parts[3:]
    if parts == ("third_party", "fmt", "LICENSE"):
        return ("licenses", "fmt", "LICENSE")
    if parts == ("LICENSE",):
        return parts
    return None


def archive_headers(archive_path, target, expected_id):
    is_zip = zipfile.is_zipfile(archive_path)
    opener = zipfile.ZipFile if is_zip else tarfile.open
    with opener(archive_path) as archive:
        commit = archive.comment.decode("ascii", "strict") if is_zip else archive.pax_headers.get("comment", "")
        if commit != expected_id:
            raise SourceError("archive Git commit metadata does not match the expected full source ID")
        members = archive.infolist() if is_zip else archive.getmembers()
        entries = [(member, safe_parts(member.filename if is_zip else member.name)) for member in members]
        roots = {parts[:-3] for _, parts in entries if parts[-3:] == ("src", "include", "duckdb.hpp")}
        if len(roots) != 1:
            raise SourceError("archive must contain exactly one src/include/duckdb.hpp tree")
        root = roots.pop()
        seen = set()
        for member, parts in entries:
            mode = member.external_attr >> 16 if is_zip else member.mode
            symlink = stat.S_ISLNK(mode) if is_zip else member.issym()
            hardlink = False if is_zip else member.islnk()
            directory = member.is_dir() if is_zip else member.isdir()
            regular = not stat.S_IFMT(mode) or stat.S_ISREG(mode) if is_zip else member.isfile()
            relative = source_destination(parts[len(root):]) if parts[:len(root)] == root else None
            if symlink or hardlink:
                link = archive.read(member).decode("utf-8", "strict") if is_zip else member.linkname
                validate_link(parts, link, root, hardlink)
                if relative is not None:
                    raise SourceError("source headers and LICENSE must not contain links")
                continue
            if not regular and not directory:
                raise SourceError("archive contains an unsupported special file")
            if directory or relative is None:
                continue
            key = "/".join(relative).casefold()
            if key in seen:
                raise SourceError("archive contains duplicate or case-colliding output files")
            seen.add(key)
            destination = target.joinpath(*relative)
            destination.parent.mkdir(parents=True, exist_ok=True)
            source = archive.open(member) if is_zip else archive.extractfile(member)
            with source, destination.open("xb") as output:
                shutil.copyfileobj(source, output)
        if not (target / "LICENSE").is_file() or not (target / "include/duckdb.h").is_file():
            raise SourceError("source archive is missing LICENSE or src/include/duckdb.h")


def git(checkout, *arguments, input_data=None):
    result = subprocess.run(
        ["git", "--no-replace-objects", "-C", str(checkout), *arguments], input=input_data, check=False, capture_output=True
    )
    if result.returncode:
        raise SourceError(result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def checkout_identity(checkout, version, source_id):
    if Path(os.fsdecode(git(checkout, "rev-parse", "--show-toplevel").rstrip(b"\n"))).resolve() != checkout.resolve():
        raise SourceError("checkout must name the Git repository root")
    if git(checkout, "status", "--porcelain=v1", "--untracked-files=all"):
        raise SourceError("DuckDB source checkout must be clean, including untracked files")
    actual_id = git(checkout, "rev-parse", "HEAD").decode("ascii").strip()
    if source_id is not None and source_id != actual_id:
        raise SourceError("checkout HEAD does not match the expected full source ID")
    tags = git(checkout, "tag", "--points-at", "HEAD").decode("utf-8").splitlines()
    versions = [tag for tag in tags if re.fullmatch(r"v[12]\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?", tag)]
    if version is None:
        if len(versions) != 1:
            raise SourceError("checkout needs one exact release tag or an explicit --version")
        version = versions[0]
    elif versions and version not in versions:
        raise SourceError("expected version does not match the checkout's exact release tag")
    return identity(version, actual_id)


def checkout_headers(checkout, source_id, target):
    tree = git(checkout, "ls-tree", "-rz", "--full-tree", source_id, "--", "src/include", "LICENSE",
               "third_party/fmt/include", "third_party/fmt/LICENSE")
    files = []
    seen = set()
    for entry in tree.split(b"\0"):
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        mode, kind, object_id = metadata.split(b" ")
        parts = safe_parts(name.decode("utf-8"))
        if mode not in (b"100644", b"100755") or kind != b"blob":
            raise SourceError("source headers and LICENSE must be regular Git blobs")
        relative = source_destination(parts)
        if relative is None:
            raise SourceError("Git returned a path outside the selected source headers")
        key = "/".join(relative).casefold()
        if key in seen:
            raise SourceError("checkout contains case-colliding output files")
        seen.add(key)
        files.append((object_id, target.joinpath(*relative)))
    blobs = git(checkout, "cat-file", "--batch", input_data=b"".join(object_id + b"\n" for object_id, _ in files))
    offset = 0
    for object_id, destination in files:
        end = blobs.index(b"\n", offset)
        actual_id, kind, size = blobs[offset:end].split(b" ")
        if actual_id != object_id or kind != b"blob":
            raise SourceError("Git returned an unexpected object while reading source headers")
        offset = end + 1
        end = offset + int(size)
        if end >= len(blobs) or blobs[end:end + 1] != b"\n":
            raise SourceError("Git returned incomplete source header contents")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(blobs[offset:end])
        offset = end + 1
    if not all((target / name).is_file() for name in ("LICENSE", "include/duckdb.h", "include/duckdb.hpp")):
        raise SourceError("source checkout is missing LICENSE or required DuckDB headers")


def files_in(path):
    files = {}
    for entry in sorted(path.rglob("*")):
        relative = entry.relative_to(path).as_posix()
        safe_parts(relative)
        if entry.is_symlink() or not (entry.is_dir() or entry.is_file()):
            raise SourceError(f"cache contains a link or special file: {relative}")
        if entry.is_file() and relative != "manifest.json":
            files[relative] = digest(entry)
    return files


def tree_digest(files):
    entries = "".join(f"{name}\0{checksum}\n" for name, checksum in sorted(files.items()))
    return hashlib.sha256(entries.encode("utf-8")).hexdigest()


def verify(path, version=None, source_id=None):
    if path.is_symlink():
        raise SourceError("cache root must not be a symlink")
    manifest_path = path / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise SourceError("cache manifest must be a regular file")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise SourceError("unsupported source cache manifest")
    info = identity(manifest.get("version"), manifest.get("source_id"))
    if manifest.get("engine_major") != info["engine_major"]:
        raise SourceError("cache engine major does not match its version")
    if (version is not None and version != info["version"]) or (source_id is not None and source_id != info["source_id"]):
        raise SourceError("cached source identity does not match the requested identity")
    files = files_in(path)
    if files != manifest.get("files") or tree_digest(files) != manifest.get("tree_sha256"):
        raise SourceError("source cache contents changed; remove and re-stage the cache")
    required = {"LICENSE", "include/duckdb.h", "include/duckdb.hpp", "include/pdo_duckdb_bridge_build.h",
                "include/fmt/core.h", "include/fmt/format.h", "include/fmt/format-inl.h",
                "include/fmt/ostream.h", "include/fmt/printf.h", "licenses/fmt/LICENSE"}
    if not required.issubset(files):
        raise SourceError("source cache is missing required headers or license")
    if (path / "include/pdo_duckdb_bridge_build.h").read_bytes() != build_header(info):
        raise SourceError("generated bridge identity does not match the source cache manifest")
    return manifest


def stage(arguments):
    output = Path(arguments.output).absolute()
    if arguments.checkout:
        checkout = Path(arguments.checkout)
        info = checkout_identity(checkout, arguments.version, arguments.source_id)
    else:
        info = identity(arguments.version, arguments.source_id)
        if not re.fullmatch(r"[0-9a-f]{64}", arguments.sha256 or ""):
            raise SourceError("archives and downloads require a pinned lowercase --sha256")
    if arguments.checkout and arguments.sha256:
        raise SourceError("--sha256 applies only to archives and downloads")
    if output.exists() or output.is_symlink():
        existing = verify(output, info["version"], info["source_id"])
        if arguments.sha256 and existing.get("archive_sha256") != arguments.sha256:
            raise SourceError("cached archive checksum does not match the requested archive")
        return existing
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".duckdb-bridge-", dir=output.parent) as temporary:
        temporary = Path(temporary)
        staged = temporary / "staged"
        staged.mkdir()
        if arguments.checkout:
            checkout_headers(checkout, info["source_id"], staged)
        elif arguments.url:
            if not arguments.url.startswith("https://"):
                raise SourceError("downloads require an explicit HTTPS URL")
            archive = temporary / "source.archive"
            with urllib.request.urlopen(arguments.url, timeout=60) as source, archive.open("wb") as stream:
                shutil.copyfileobj(source, stream)
        else:
            archive = temporary / "source.archive"
            shutil.copyfile(arguments.archive, archive)
        if not arguments.checkout and digest(archive) != arguments.sha256:
            raise SourceError("source archive SHA256 does not match the pinned checksum")
        if not arguments.checkout:
            archive_headers(archive, staged, info["source_id"])
        if arguments.checkout and checkout_identity(checkout, info["version"], info["source_id"]) != info:
            raise SourceError("source checkout changed while staging")
        (staged / "include/pdo_duckdb_bridge_build.h").write_bytes(build_header(info))
        files = files_in(staged)
        manifest = {"format": 1, **info, "files": files, "tree_sha256": tree_digest(files)}
        if not arguments.checkout:
            manifest["archive_sha256"] = arguments.sha256
        (staged / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        verify(staged, info["version"], info["source_id"])
        staged.rename(output)
        return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    staging = commands.add_parser("stage", help="stage immutable source headers into a new cache")
    sources = staging.add_mutually_exclusive_group(required=True)
    sources.add_argument("--checkout")
    sources.add_argument("--archive")
    sources.add_argument("--url")
    staging.add_argument("--sha256")
    staging.add_argument("--output", required=True)
    checking = commands.add_parser("verify", help="reject changed or mismatched cached headers")
    checking.add_argument("cache")
    for command in (staging, checking):
        command.add_argument("--version")
        command.add_argument("--source-id")
    arguments = parser.parse_args()
    try:
        manifest = stage(arguments) if arguments.command == "stage" else verify(Path(arguments.cache), arguments.version, arguments.source_id)
        print(json.dumps({key: manifest[key] for key in ("version", "source_id", "engine_major", "tree_sha256")}, sort_keys=True))
    except (SourceError, OSError, ValueError, tarfile.TarError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"duckdb-bridge-source: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
