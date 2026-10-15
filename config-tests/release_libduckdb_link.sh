#!/usr/bin/env bash
# Exercise the real release libduckdb link checks without building or releasing.
set -euo pipefail
repo="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
workflow="$repo/.github/workflows/release-linux.yml"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/temp dir"
for inspector in ldd otool; do
    cat > "$work/bin/$inspector" <<'INSPECTOR'
#!/usr/bin/env bash
set -euo pipefail
[ "$(basename "$0")" = "$EXPECTED_INSPECTOR" ]
[ "$#" -eq "$EXPECTED_ARGC" ]
[ "${!#}" = release-stage/pdo_duckdb.so ]
[ "$#" -eq 1 ] || [ "$1" = -L ]
cat "$PROBE_INPUT"
exit "$PRODUCER_STATUS"
INSPECTOR
    chmod +x "$work/bin/$inspector"
done
# Generate the oversized fixture once, outside the producer pipeline.
awk 'BEGIN { for (i = 0; i < 65536; i++) print "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)" }' > "$work/tail"

check() {
    local label="$1" expected="$2" producer_status="$3" status=0
    PATH="$work/bin:$PATH" RUNNER_TEMP="$work/temp dir" \
        PROBE_INPUT="$work/input" PRODUCER_STATUS="$producer_status" \
        EXPECTED_INSPECTOR="$inspector" EXPECTED_ARGC="$argc" \
        bash -euo pipefail -c "$probe" > "$work/output" 2> "$work/error" || status=$?
    local ok=1
    case "$expected" in
        pass) [ "$status" -eq 0 ] && [ ! -s "$work/output" ] || ok=0 ;;
        reject) [ "$status" -eq 1 ] &&
            [ "$(cat "$work/output")" = "release archive still depends on libduckdb" ] || ok=0 ;;
        producer) [ "$status" -eq "$producer_status" ] && [ ! -s "$work/output" ] || ok=0 ;;
    esac
    if [ "$ok" -ne 1 ]; then
        printf 'FAIL: %s: %s (expected %s, got status %s)\n' "$inspector" "$label" "$expected" "$status" >&2
        cat "$work/output" "$work/error" >&2
        exit 1
    fi
    printf 'PASS: %s: %s\n' "$inspector" "$label"
}

for inspector in ldd otool; do
    argc=1
    [ "$inspector" = ldd ] || argc=2
    # Run exactly the production statements from the inspector call through the
    # end of its libduckdb check, including the old piped form on a base checkout.
    probe=$(awk -v re="^[[:space:]]*(if )?$inspector (-L )?release-stage/pdo_duckdb[.]so" '
        $0 ~ re { on = 1 }
        on { print }
        on && /^[[:space:]]*fi$/ { exit }
    ' "$workflow")
    [ "$(printf '%s\n' "$probe" | grep -c libduckdb)" -ge 1 ]
    [ "$(printf '%s\n' "$probe" | grep -c '^[[:space:]]*fi$')" -eq 1 ]

    cp "$work/tail" "$work/input"
    check 'clean long output' pass 0
    printf '\tlibduckdb.dylib (compatibility version 0.0.0)\n' > "$work/input"
    cat "$work/tail" >> "$work/input"
    check 'libduckdb early in long output' reject 0
    cp "$work/tail" "$work/input"
    printf '\tlibduckdb.so => /usr/local/lib/libduckdb.so (0x00007f0000000000)\n' >> "$work/input"
    check 'libduckdb late in long output' reject 0
    printf '\tLIBDUCKDB.so\n' > "$work/input"
    check 'case-insensitive match' reject 0
    : > "$work/input"
    check 'failed inspector without output' producer 42
    head -100 "$work/tail" > "$work/input"
    check 'failed inspector with partial clean output' producer 42
    printf '\tlibduckdb.so\n' > "$work/input"
    check 'failed inspector with libduckdb output' producer 42
    check 'interrupted inspector' producer 143
done
