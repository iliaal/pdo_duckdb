#!/usr/bin/env python3
"""Exercise source staging with real Git archives and disposable repositories."""

import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
import zipfile


HELPER = Path(__file__).with_name("duckdb-bridge-source.py")


class BridgeSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="duckdb-bridge-source-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "source"
        self.checkout.mkdir()
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        includes = self.checkout / "src/include"
        includes.mkdir(parents=True)
        (includes / "duckdb.h").write_bytes(b"#define DUCKDB_API_VERSION \"1.5.6\"\n")
        (includes / "duckdb.hpp").write_bytes(b"#include \"duckdb/nested.hpp\"\n")
        (includes / "duckdb").mkdir()
        (includes / "duckdb/nested.hpp").write_bytes(b"// exact header bytes\r\n")
        (includes / "duckdb/LICENSE").write_text("nested license\n", encoding="utf-8")
        (self.checkout / "LICENSE").write_text("source license\n", encoding="utf-8")
        vendor = self.checkout / "third_party/fmt"
        (vendor / "include/fmt").mkdir(parents=True)
        for name in ("core.h", "format.h", "format-inl.h", "ostream.h", "printf.h"):
            (vendor / "include/fmt" / name).write_bytes(b"// exact vendored header bytes\r\n")
        (vendor / "LICENSE").write_text("fmt license\n", encoding="utf-8")
        self.git("add", "src", "LICENSE", "third_party")
        self.git("-c", "user.name=Bridge source tests", "-c", "user.email=bridge@example.invalid", "commit", "-qm", "headers")
        self.source_id = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("update-ref", "refs/tags/v2.0.0", "HEAD")
        self.cache = self.root / "cache"

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.checkout), *arguments], capture_output=True, text=True, check=True)

    def run_helper(self, *arguments, succeeds=True):
        result = subprocess.run([sys.executable, str(HELPER), *map(str, arguments)], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
        if succeeds:
            return json.loads(result.stdout)
        self.assertTrue(result.stderr.startswith("duckdb-bridge-source:"), result.stderr)
        return result.stderr

    def checkout_stage(self, *arguments, succeeds=True):
        return self.run_helper("stage", "--checkout", self.checkout, "--output", self.cache, *arguments, succeeds=succeeds)

    def archive(self, kind="tar"):
        archive = self.root / ("unrelated-name." + kind)
        with archive.open("wb") as stream:
            subprocess.run(["git", "-C", str(self.checkout), "archive", "--format=" + kind, "--prefix=arbitrary-root/", "HEAD"], stdout=stream, check=True)
        return archive

    def archive_stage(self, archive, *arguments, succeeds=True):
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        return self.run_helper("stage", "--archive", archive, "--sha256", checksum,
                               "--version", "v2.0.0", "--source-id", self.source_id,
                               "--output", self.cache, *arguments, succeeds=succeeds)

    def add_tar(self, archive, name, data=b"bad", link=None):
        with tarfile.open(archive, "a") as stream:
            member = tarfile.TarInfo(name)
            if link is not None:
                member.type = tarfile.SYMTYPE
                member.linkname = link
                stream.addfile(member)
            else:
                member.size = len(data)
                stream.addfile(member, io.BytesIO(data))

    def assert_no_cache(self):
        self.assertFalse(self.cache.exists())
        self.assertFalse(list(self.root.glob(".duckdb-bridge-*")))

    def test_checkout_infers_exact_release_identity_and_preserves_all_header_bytes(self):
        info = self.checkout_stage()
        self.assertEqual(info["version"], "v2.0.0")
        self.assertEqual(info["source_id"], self.source_id)
        self.assertEqual(info["engine_major"], 2)
        for source in (self.checkout / "src/include").rglob("*"):
            if source.is_file():
                self.assertEqual(source.read_bytes(), (self.cache / "include" / source.relative_to(self.checkout / "src/include")).read_bytes())
        header = (self.cache / "include/pdo_duckdb_bridge_build.h").read_text()
        for source in (self.checkout / "third_party/fmt/include/fmt").iterdir():
            self.assertEqual(source.read_bytes(), (self.cache / "include/fmt" / source.name).read_bytes())
        self.assertEqual((self.cache / "licenses/fmt/LICENSE").read_bytes(), (self.checkout / "third_party/fmt/LICENSE").read_bytes())
        self.assertIn('#define PDO_DUCKDB_BRIDGE_EXPECTED_VERSION "v2.0.0"', header)
        self.assertIn(f'#define PDO_DUCKDB_BRIDGE_EXPECTED_SOURCE_ID "{self.source_id[:10]}"', header)
        self.assertIn("#define PDO_DUCKDB_BRIDGE_ENGINE_MAJOR 2", header)
        self.assertEqual(self.checkout_stage(), info)
        self.assertEqual(self.run_helper("verify", self.cache), info)
        self.assertEqual(self.git("status", "--porcelain=v1").stdout, "")

    def test_dirty_tracked_and_untracked_checkout_rejected(self):
        header = self.checkout / "src/include/duckdb.h"
        original = header.read_bytes()
        header.write_bytes(original + b"changed\n")
        self.assertIn("clean", self.checkout_stage(succeeds=False))
        self.assert_no_cache()
        header.write_bytes(original)
        (self.checkout / "untracked").write_bytes(b"data")
        self.assertIn("clean", self.checkout_stage(succeeds=False))
        self.assert_no_cache()

    def test_checkout_staging_ignores_git_export_attributes(self):
        header = self.checkout / "src/include/duckdb/nested.hpp"
        header.write_bytes(b"// $Format:%H$\n")
        self.git("add", "src/include/duckdb/nested.hpp")
        self.git("-c", "user.name=Bridge source tests", "-c", "user.email=bridge@example.invalid", "commit", "-qm", "export marker")
        self.git("update-ref", "refs/tags/v2.0.0", "HEAD")
        self.source_id = self.git("rev-parse", "HEAD").stdout.strip()
        attributes = self.checkout / ".git/info/attributes"
        attributes.write_text("src/include/duckdb.h export-ignore\nsrc/include/duckdb/nested.hpp export-subst\n", encoding="utf-8")
        self.assertEqual(self.git("status", "--porcelain=v1").stdout, "")
        self.checkout_stage()
        self.assertEqual((self.cache / "include/duckdb.h").read_bytes(), (self.checkout / "src/include/duckdb.h").read_bytes())
        self.assertEqual((self.cache / "include/duckdb/nested.hpp").read_bytes(), header.read_bytes())
        self.run_helper("verify", self.cache)

    def test_checkout_staging_ignores_git_replacement_objects(self):
        original = self.git("rev-parse", "HEAD:src/include/duckdb.hpp").stdout.strip()
        replacement = self.root / "replacement.hpp"
        replacement.write_bytes(b"changed header\n")
        changed = self.git("hash-object", "-w", str(replacement)).stdout.strip()
        self.git("replace", original, changed)
        self.assertEqual(self.git("status", "--porcelain=v1").stdout, "")
        self.assertEqual(self.checkout_stage()["source_id"], self.source_id)
        self.assertEqual((self.cache / "include/duckdb.hpp").read_bytes(), (self.checkout / "src/include/duckdb.hpp").read_bytes())
        self.run_helper("verify", self.cache)

    def test_wrong_checkout_commit_and_tag_rejected(self):
        self.assertIn("HEAD", self.checkout_stage("--source-id", "0" * 40, succeeds=False))
        self.assert_no_cache()
        self.assertIn("tag", self.checkout_stage("--version", "v1.5.3", succeeds=False))
        self.assert_no_cache()

    def test_untagged_checkout_requires_explicit_version_and_accepts_custom_commit(self):
        self.git("tag", "-d", "v2.0.0")
        self.assertIn("explicit --version", self.checkout_stage(succeeds=False))
        self.assert_no_cache()
        self.assertEqual(self.checkout_stage("--version", "v1.5.3-dev")["version"], "v1.5.3-dev")

    def test_tar_and_zip_verify_embedded_git_commit(self):
        for kind in ("tar", "zip"):
            with self.subTest(kind=kind):
                archive = self.archive(kind)
                result = self.archive_stage(archive)
                self.assertEqual(result["source_id"], self.source_id)
                self.assertEqual((self.cache / "LICENSE").read_bytes(), (self.checkout / "LICENSE").read_bytes())
                self.assertEqual((self.cache / "include/fmt/format.h").read_bytes(), (self.checkout / "third_party/fmt/include/fmt/format.h").read_bytes())
                self.assertEqual((self.cache / "licenses/fmt/LICENSE").read_bytes(), (self.checkout / "third_party/fmt/LICENSE").read_bytes())
                shutil.rmtree(self.cache)
                self.assertIn("commit metadata", self.archive_stage(archive, "--source-id", "0" * 40, succeeds=False))
                self.assert_no_cache()

    def test_wrong_archive_checksum_rejected(self):
        self.assertIn("SHA256", self.archive_stage(self.archive(), "--sha256", "0" * 64, succeeds=False))
        self.assert_no_cache()

    def test_vendored_header_changes_and_omissions_are_rejected(self):
        self.checkout_stage()
        (self.cache / "include/fmt/format.h").write_bytes(b"changed\n")
        self.assertIn("changed", self.run_helper("verify", self.cache, succeeds=False))
        shutil.rmtree(self.cache)
        self.git("rm", "third_party/fmt/include/fmt/format.h")
        self.git("-c", "user.name=Bridge source tests", "-c", "user.email=bridge@example.invalid", "commit", "-qm", "missing vendor header")
        self.git("update-ref", "refs/tags/v2.0.0", "HEAD")
        self.source_id = self.git("rev-parse", "HEAD").stdout.strip()
        self.assertIn("missing required", self.checkout_stage(succeeds=False))
        self.assert_no_cache()

    def test_vendor_headers_cannot_override_engine_headers(self):
        archive = self.archive()
        self.add_tar(archive, "arbitrary-root/src/include/fmt/format.h", b"shadowed\n")
        self.assertIn("duplicate", self.archive_stage(archive, succeeds=False))
        self.assert_no_cache()

    @unittest.skipUnless(Path("/proc/self/fd").is_dir(), "open-file race observation requires Linux procfs")
    def test_archive_path_replacement_cannot_bypass_checksum(self):
        archive = self.archive()
        replacement = self.root / "replacement.tar"
        shutil.copyfile(archive, replacement)
        with tarfile.open(replacement, "r") as stream:
            member = stream.getmember("arbitrary-root/src/include/duckdb.hpp")
            offset, size = member.offset_data, member.size
        with replacement.open("r+b") as stream:
            stream.seek(offset)
            stream.write(b"X" * size)
        with archive.open("r+b") as stream:
            stream.truncate(128 * 1024 * 1024)
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        command = [sys.executable, str(HELPER), "stage", "--archive", str(archive),
                   "--sha256", checksum, "--version", "v2.0.0", "--source-id", self.source_id,
                   "--output", str(self.cache)]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
            replaced = False
            deadline = time.monotonic() + 15
            while process.poll() is None and time.monotonic() < deadline:
                for descriptor in Path(f"/proc/{process.pid}/fd").glob("*"):
                    try:
                        opened = descriptor.readlink()
                    except FileNotFoundError:
                        continue
                    if opened == archive:
                        replacement.replace(archive)
                        replaced = True
                        break
                if replaced:
                    break
                time.sleep(0.001)
            stdout, stderr = process.communicate(timeout=15)
            self.assertTrue(replaced, "did not exercise replacement while the original archive was open")
            self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual((self.cache / "include/duckdb.hpp").read_bytes(), (self.checkout / "src/include/duckdb.hpp").read_bytes())
        self.run_helper("verify", self.cache)

    def test_missing_commit_metadata_rejected(self):
        archive = self.root / "no-identity.tar"
        with tarfile.open(archive, "w") as stream:
            stream.add(self.checkout / "src", arcname="src")
            stream.add(self.checkout / "LICENSE", arcname="LICENSE")
        self.assertIn("commit metadata", self.archive_stage(archive, succeeds=False))
        self.assert_no_cache()

    def test_traversal_outside_selected_headers_rejected(self):
        for name in ("arbitrary-root/../escape", "/absolute", "C:/windows", "arbitrary-root\\bad"):
            with self.subTest(name=name):
                archive = self.archive()
                self.add_tar(archive, name)
                self.assertIn("unsafe", self.archive_stage(archive, succeeds=False))
                self.assert_no_cache()
        self.assertFalse((self.root / "escape").exists())

    def test_safe_unselected_links_allowed_but_selected_or_escaping_links_rejected(self):
        archive = self.archive()
        self.add_tar(archive, "arbitrary-root/tests/link", link="../LICENSE")
        self.archive_stage(archive)
        shutil.rmtree(self.cache)
        for name, target in (("arbitrary-root/src/include/linked.h", "../../../LICENSE"),
                             ("arbitrary-root/tests/link", "../../escape")):
            with self.subTest(name=name):
                archive = self.archive()
                self.add_tar(archive, name, link=target)
                self.archive_stage(archive, succeeds=False)
                self.assert_no_cache()

    def test_zip_selected_symlink_rejected(self):
        archive = self.archive("zip")
        with zipfile.ZipFile(archive, "a") as stream:
            member = zipfile.ZipInfo("arbitrary-root/src/include/link.h")
            member.create_system = 3
            member.external_attr = 0o120777 << 16
            stream.writestr(member, "../duckdb.h")
        self.assertIn("must not contain links", self.archive_stage(archive, succeeds=False))
        self.assert_no_cache()

    def test_case_colliding_headers_rejected(self):
        archive = self.archive()
        self.add_tar(archive, "arbitrary-root/src/include/DuckDB.h")
        self.assertIn("case-colliding", self.archive_stage(archive, succeeds=False))
        self.assert_no_cache()

    def test_windows_reserved_header_names_rejected_on_every_host(self):
        for name in ("NUL", "aux.hpp", "duckdb.h.", "space ", "CON .h", "question?.hpp"):
            with self.subTest(name=name):
                archive = self.archive()
                self.add_tar(archive, "arbitrary-root/src/include/" + name)
                self.assertIn("unsafe", self.archive_stage(archive, succeeds=False))
                self.assert_no_cache()

    def test_cache_changes_are_rejected_instead_of_silently_repaired(self):
        for alteration in ("modify", "remove", "add"):
            with self.subTest(alteration=alteration):
                self.checkout_stage()
                header = self.cache / "include/duckdb.h"
                if alteration == "modify":
                    header.write_bytes(b"changed")
                elif alteration == "remove":
                    header.unlink()
                else:
                    (self.cache / "include/extra.h").write_bytes(b"extra")
                self.assertIn("changed", self.run_helper("verify", self.cache, succeeds=False))
                self.assertIn("changed", self.checkout_stage(succeeds=False))
                shutil.rmtree(self.cache)

    def test_cache_expected_identity_must_match(self):
        self.checkout_stage()
        self.assertIn("identity", self.run_helper("verify", self.cache, "--source-id", "0" * 40, succeeds=False))
        self.assertIn("identity", self.run_helper("verify", self.cache, "--version", "v1.5.6", succeeds=False))


if __name__ == "__main__":
    unittest.main(verbosity=2)
