#!/usr/bin/env bash
# Check argument forwarding without requiring an ASan PHP or DuckDB build.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
harness="${1:-$repo/scripts/asan-test.sh}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/Duck DB/modules" "$work/plain/modules"

# Supply library names to the wrapper; the fake PHP clears LD_PRELOAD before
# launching any subprocess, so no ASan runtime is required for this test.
cat > "$work/bin/ldd" <<'LDD'
#!/usr/bin/env bash
printf 'libasan.so => %s (0x0)\n' "$FAKE_LIBRARY"
printf 'libstdc++.so => %s (0x0)\n' "$FAKE_LIBRARY"
LDD
cat > "$work/bin/php" <<'PHP'
#!/usr/bin/env bash
set -euo pipefail
unset LD_PRELOAD
[ "$1" = -d ]
[ "$2" = "extension=$EXPECTED_EXT" ]
[ "$3" = "$RUN_TESTS" ]
shift 3
# Model run-tests.php's literal-space split of TEST_PHP_ARGS. Quoting inside
# that environment string does not protect a path with spaces.
IFS=' ' read -r -a extra <<< "${TEST_PHP_ARGS-}"
set -- "$@" "${extra[@]}"
extension_dir=
caller_option=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -d)
            case "$2" in
                extension_dir=*) extension_dir="${2#extension_dir=}" ;;
                memory_limit=*) caller_option="-d $2" ;;
                *) exit 1 ;;
            esac
            shift 2
            ;;
        -p) [ "$2" = "$PHP" ]; shift 2 ;;
        tests/) shift ;;
        *) printf 'unexpected runner argument: <%s>\n' "$1" >&2; exit 1 ;;
    esac
done
[ "$extension_dir" = "$EXPECTED_DIR" ]
[ "$caller_option" = "$EXPECTED_ARGS" ]
[ "$TEST_PHP_EXECUTABLE" = "$PHP" ]
[ "$USE_ZEND_ALLOC" = 0 ]
exit "${FAKE_STATUS:-0}"
PHP
chmod +x "$work/bin/ldd" "$work/bin/php"
# Use a real, harmless shared library as the preload stand-in. This test runs
# on Linux, just like the ldd-based ASan helper it exercises.
library=$(ldd /bin/sh | awk '/libc\.so/ {print $3}')
[ -f "$library" ]

check() {
    local label="$1" directory="$2" caller_args="$3" expected_status="$4" status=0
    PATH="$work/bin:$PATH" PHP="$work/bin/php" RUN_TESTS="$work/run tests.php" \
        DUCKDB_PREFIX="$work/Duck DB" EXT="$directory/pdo_duckdb.so" \
        EXPECTED_EXT="$directory/pdo_duckdb.so" EXPECTED_DIR="$directory" \
        EXPECTED_ARGS="$caller_args" TEST_PHP_ARGS="$caller_args" \
        FAKE_LIBRARY="$library" FAKE_STATUS="$expected_status" \
        bash "$harness" > "$work/output" 2>&1 || status=$?
    if [ "$status" -ne "$expected_status" ]; then
        echo "FAIL: $label (expected $expected_status, got $status)" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "PASS: $label"
}

check 'plain module directory' "$work/plain/modules" '' 0
check 'module directory with spaces' "$work/Duck DB/modules" '' 0
check 'preserve caller PHP options' "$work/Duck DB/modules" '-d memory_limit=256M' 0
check 'preserve runner failure status' "$work/Duck DB/modules" '' 42
