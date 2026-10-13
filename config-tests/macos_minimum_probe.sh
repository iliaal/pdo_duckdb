#!/usr/bin/env bash
# Exercise the real workflow probe statements without building or releasing.
set -euo pipefail
repo="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/temp dir"
cat > "$work/bin/otool" <<'OTOOL'
#!/usr/bin/env bash
set -euo pipefail
[ "$1" = -l ]
[ "$2" = "$EXPECTED_MODULE" ]
cat "$PROBE_INPUT"
exit "$PRODUCER_STATUS"
OTOOL
chmod +x "$work/bin/otool"
# Generate the oversized fixture once, outside the producer pipeline.
awk 'BEGIN { for (i = 0; i < 65536; i++) print "      cmd LC_SEGMENT_64" }' > "$work/tail"

check() {
    local label="$1" expected="$2" producer_status="$3" status=0
    PATH="$work/bin:$PATH" RUNNER_TEMP="$work/temp dir" \
        PROBE_INPUT="$work/input" PRODUCER_STATUS="$producer_status" \
        EXPECTED_MODULE="$module" \
        bash -euo pipefail -c "$probe"$'\nprintf "%s\\n" "$min_macos"' \
        > "$work/output" 2> "$work/error" || status=$?
    if [ "$status" -ne "$producer_status" ] ||
        { [ "$status" -eq 0 ] && [ "$(cat "$work/output")" != "$expected" ]; } ||
        { [ "$status" -ne 0 ] && [ -s "$work/output" ]; }; then
        printf 'FAIL: %s: %s (expected status %s, got %s)\n' "$workflow" "$label" "$producer_status" "$status" >&2
        cat "$work/output" "$work/error" >&2
        exit 1
    fi
    printf 'PASS: %s: %s\n' "$workflow" "$label"
}

for workflow in release-linux.yml binary-validate.yml; do
    # Run exactly the production producer/assignment, including the old combined
    # assignment when testing this suite against the base checkout.
    probe=$(sed -n '/^[[:space:]]*otool -l /p; /^[[:space:]]*min_macos=/p' "$repo/.github/workflows/$workflow")
    [ "$(printf '%s\n' "$probe" | grep -c 'min_macos=')" -eq 1 ]
    module=modules/pdo_duckdb.so
    [ "$workflow" != release-linux.yml ] || module=release-stage/pdo_duckdb.so

    printf '    minos 11.0\n' > "$work/input"
    check 'ordinary minimum' 11.0 0
    printf '    minos 11.0.0\n' > "$work/input"
    check 'three-part minimum' 11.0.0 0
    printf '    minos 11.0\n' > "$work/input"
    cat "$work/tail" >> "$work/input"
    check 'early minimum with large trailing output' 11.0 0
    printf '    minos 12.0\n' >> "$work/input"
    check 'first minimum retained' 11.0 0
    cp "$work/tail" "$work/input"
    printf '    minos 11.0\n' >> "$work/input"
    check 'late minimum' 11.0 0
    printf '    minos_version 11.0\n' > "$work/input"
    check 'exact field matching and absent minimum' '' 0
    printf '    minos 12.0\n' > "$work/input"
    check 'unsupported minimum preserved for caller rejection' 12.0 0
    printf '    minos 11.0\n' > "$work/input"
    check 'failed producer with plausible output' '' 42
    : > "$work/input"
    check 'failed producer without output' '' 42
    check 'interrupted producer' '' 143
done
