#!/usr/bin/env python3
"""Compare selected public C headers with an already verified source cache."""

import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        raise ValueError("usage: check-duckdb-headers.py VERIFIED_INCLUDE SELECTED_INCLUDE")
    expected, selected = map(Path, sys.argv[1:])
    names = ["duckdb.h"]
    if (expected / "duckdb_v2.h").is_file():
        names += ["duckdb_v2.h", "duckdb_extension.h", "duckdb_extension_v2.h", "duckdb_static_extension.h"]
    elif (selected / "duckdb_extension.h").exists():
        names.append("duckdb_extension.h")
    for name in names:
        source = (expected / name).read_bytes().replace(b"\r\n", b"\n")
        header = (selected / name).read_bytes().replace(b"\r\n", b"\n")
        if header != source:
            raise ValueError(f"selected DuckDB C header differs from the verified source: {name}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"DuckDB C header verification failed: {error}", file=sys.stderr)
        sys.exit(1)
