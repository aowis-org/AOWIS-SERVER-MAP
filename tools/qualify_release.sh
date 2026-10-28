#!/usr/bin/env bash
# Validate release builds, shutdown/cancellation regressions, and optional sanitizers.
# No server listener is started. Logs are retained in a build directory.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
mode=release
quick=false

usage()
{
    cat <<'USAGE'
Usage: ./tools/qualify_release.sh [--release | --sanitizers | --all] [--quick]

  --release     Release build, full suite, lifecycle stress and config checks (default)
  --sanitizers  Separate Debug build with AddressSanitizer and UndefinedBehaviorSanitizer
  --all         Run Release qualification followed by sanitizer qualification
  --quick       Reduce lifecycle repetitions for a smoke run (not full qualification)

The script never launches the HTTP listener. Logs remain under
build-linux-qualification/. Sanitizer runs need compiler and runtime support.
USAGE
}

while (( $# > 0 )); do
    case "$1" in
        --release) mode=release ;;
        --sanitizers) mode=sanitizers ;;
        --all) mode=all ;;
        --quick) quick=true ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

for command_name in cmake ctest ninja c++; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        printf 'Required tool not found: %s\n' "$command_name" >&2
        exit 1
    fi
done

report_root="$project_dir/build-linux-qualification"
mkdir -p -- "$report_root"
report_dir="$report_root/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p -- "$report_dir"
summary="$report_dir/summary.txt"
printf 'Mode: %s\nQuick smoke: %s\nStarted (UTC): %s\n' \
    "$mode" "$quick" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$summary"
printf 'Qualification logs: %s\n' "$report_dir"

finish()
{
    local result=$?
    trap - EXIT
    if (( result == 0 )); then
        printf 'Result: PASS\n' >> "$summary"
        printf 'Release qualification PASSED. Logs: %s\n' "$report_dir"
    else
        printf 'Result: FAIL (exit %d)\n' "$result" >> "$summary"
        printf 'Release qualification FAILED (exit %d). Logs: %s\n' \
            "$result" "$report_dir" >&2
    fi
    exit "$result"
}
trap finish EXIT

run_logged()
{
    local name="$1"
    shift
    printf '\n== %s ==\n' "$name"
    "$@" 2>&1 | tee "$report_dir/$name.log"
    printf 'PASS: %s\n' "$name" >> "$summary"
}

check_lifecycle_test()
{
    local build_dir="$1"
    local registration
    registration="$(ctest --test-dir "$build_dir" -N \
        -R '^aowis-server-map-terrain-singleflight-http$')"
    if ! grep -q 'Total Tests: 1' <<< "$registration"; then
        printf 'Expected exactly one terrain lifecycle test in %s:\n%s\n' \
            "$build_dir" "$registration" >&2
        return 1
    fi
}

check_config_cli()
{
    local binary="$1"
    local variant="$2"
    local test_dir
    local config_file
    test_dir="$(mktemp -d "$report_dir/config-${variant}-XXXXXXXX")"
    config_file="$test_dir/map-server.ini"
    printf '\n== %s configuration smoke ==\n' "$variant"
    # Isolate the smoke test from a real user/service's provider-state catalog.
    local environment=(env -u CACHE_DIRECTORY -u STATE_DIRECTORY
        "XDG_DATA_HOME=$test_dir/data")
    "${environment[@]}" "$binary" --write-default-config "$config_file" \
        > "$report_dir/$variant-config.log" 2>&1
    "${environment[@]}" "$binary" --print-default-config \
        > "$test_dir/printed.ini"
    if ! cmp -s "$config_file" "$test_dir/printed.ini"; then
        printf 'Generated and printed default configurations differ.\n' >&2
        return 1
    fi
    "${environment[@]}" "$binary" --check-config --config "$config_file" \
        >> "$report_dir/$variant-config.log" 2>&1
    # Default configuration generation must refuse to overwrite an existing file.
    if "${environment[@]}" "$binary" --write-default-config "$config_file" \
        >> "$report_dir/$variant-config.log" 2>&1; then
        printf 'Default configuration generation overwrote an existing file.\n' >&2
        return 1
    fi
    if ! cmp -s "$config_file" "$test_dir/printed.ini"; then
        printf 'The rejected overwrite changed the existing configuration.\n' >&2
        return 1
    fi
    rm -rf -- "$test_dir"
    printf 'PASS: %s config CLI (no listener started)\n' "$variant"
    printf 'PASS: %s config CLI\n' "$variant" >> "$summary"
}

qualify_lifecycle()
{
    local build_dir="$1"
    local variant="$2"
    local cycles="$3"
    local repeats="$4"
    check_lifecycle_test "$build_dir"
    run_logged "$variant-lifecycle-cycles" \
        env "AOWIS_TERRAIN_LIFECYCLE_REPETITIONS=$cycles" \
        ctest --test-dir "$build_dir" \
        -R '^aowis-server-map-terrain-singleflight-http$' --output-on-failure
    run_logged "$variant-lifecycle-repeat" \
        env -u AOWIS_TERRAIN_LIFECYCLE_REPETITIONS \
        ctest --test-dir "$build_dir" \
        -R '^aowis-server-map-terrain-singleflight-http$' \
        --repeat "until-fail:$repeats" --output-on-failure
}

qualify_release()
{
    local build_dir="$project_dir/build-linux-tests-release"
    local cycles=20 repeats=100
    if [[ "$quick" == true ]]; then cycles=3; repeats=5; fi
    run_logged release-suite "$project_dir/compile_linux_tests.sh" --release
    check_config_cli "$build_dir/aowis-server-map" release
    qualify_lifecycle "$build_dir" release "$cycles" "$repeats"
}

qualify_sanitizers()
{
    local build_dir="$project_dir/build-linux-tests-sanitizers"
    local cycles=5 repeats=10
    if [[ "$quick" == true ]]; then cycles=2; repeats=2; fi
    local compile_flags='-fsanitize=address,undefined -fno-omit-frame-pointer -O1'
    local link_flags='-fsanitize=address,undefined'
    local sanitizer_environment=(env
        "ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}"
        "UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}")
    run_logged sanitizer-configure cmake \
        -S "$project_dir" -B "$build_dir" -G Ninja \
        -DBUILD_TESTING=ON -DAOWIS_SERVER_MAP_BUILD_SERVER=ON \
        -DCMAKE_BUILD_TYPE=Debug \
        "-DCMAKE_CXX_FLAGS=$compile_flags" \
        "-DCMAKE_EXE_LINKER_FLAGS=$link_flags" \
        "-DCMAKE_SHARED_LINKER_FLAGS=$link_flags"
    run_logged sanitizer-build cmake --build "$build_dir" \
        --target aowis-server-map-tests --parallel
    local registration
    registration="$(ctest --test-dir "$build_dir" -N -L '^aowis$')"
    if ! grep -Eq 'Total Tests: [1-9][0-9]*' <<< "$registration"; then
        printf 'No AOWIS sanitizer tests registered:\n%s\n' "$registration" >&2
        return 1
    fi
    run_logged sanitizer-suite "${sanitizer_environment[@]}" \
        ctest --test-dir "$build_dir" -L '^aowis$' --output-on-failure
    check_config_cli "$build_dir/aowis-server-map" sanitizers
    # Keep ASan/UBSan active for the repeated subprocess tests as well.
    check_lifecycle_test "$build_dir"
    run_logged sanitizer-lifecycle-cycles "${sanitizer_environment[@]}" \
        "AOWIS_TERRAIN_LIFECYCLE_REPETITIONS=$cycles" \
        ctest --test-dir "$build_dir" \
        -R '^aowis-server-map-terrain-singleflight-http$' --output-on-failure
    run_logged sanitizer-lifecycle-repeat "${sanitizer_environment[@]}" \
        env -u AOWIS_TERRAIN_LIFECYCLE_REPETITIONS \
        ctest --test-dir "$build_dir" \
        -R '^aowis-server-map-terrain-singleflight-http$' \
        --repeat "until-fail:$repeats" --output-on-failure
}

case "$mode" in
    release) qualify_release ;;
    sanitizers) qualify_sanitizers ;;
    all) qualify_release; qualify_sanitizers ;;
esac
