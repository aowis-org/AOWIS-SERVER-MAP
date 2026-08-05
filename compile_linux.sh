#!/usr/bin/env bash
set -euo pipefail

project_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_directory="$project_directory/build-linux"
install_dependencies=false
run_after_build=true
application_arguments=()

is_debian_family()
{
    if [[ ! -r /etc/os-release ]]; then
        return 1
    fi

    local ID=""
    local ID_LIKE=""
    # shellcheck disable=SC1091
    source /etc/os-release

    if [[ "${ID:-}" == "debian" ]]; then
        return 0
    fi

    [[ " ${ID_LIKE:-} " == *" debian "* ]]
}

install_debian_dependencies()
{
    if ! is_debian_family; then
        echo "Error: --install-dependencies is only supported on Debian and Debian-based distributions." >&2
        echo "Install a C++ compiler, CMake, Qt 6 Core/Network development files, and Qt 6 HttpServer development files manually." >&2
        exit 1
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        echo "Error: Debian was detected, but apt-get is unavailable." >&2
        exit 1
    fi

    local packages=(build-essential cmake ninja-build qt6-base-dev qt6-websockets-dev qt6-httpserver-dev)

    if (( EUID == 0 )); then
        apt-get update
        apt-get install -y "${packages[@]}"
        return
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        echo "Error: dependency installation requires root privileges or sudo." >&2
        exit 1
    fi

    sudo apt-get update
    sudo apt-get install -y "${packages[@]}"
}

check_build_tools()
{
    local missing_tools=()

    if ! command -v cmake >/dev/null 2>&1; then
        missing_tools+=(cmake)
    fi

    if ! command -v c++ >/dev/null 2>&1; then
        missing_tools+=("C++ compiler")
    fi

    if ! command -v ninja >/dev/null 2>&1; then
        missing_tools+=(ninja)
    fi

    if (( ${#missing_tools[@]} == 0 )); then
        return
    fi

    echo "Error: missing required build tools: ${missing_tools[*]}." >&2
    if is_debian_family; then
        echo "Run: ./compile_linux.sh --install-dependencies --no-run" >&2
    else
        echo "Install the missing tools using your distribution's package manager." >&2
    fi
    exit 1
}

while (( $# > 0 )); do
    case "$1" in
        --install-dependencies)
            install_dependencies=true
            ;;
        --no-run)
            run_after_build=false
            ;;
        --)
            shift
            application_arguments+=("$@")
            break
            ;;
        *)
            application_arguments+=("$1")
            ;;
    esac
    shift
done

if [[ "$install_dependencies" == true ]]; then
    install_debian_dependencies
fi

check_build_tools

if [[ -f "$build_directory/CMakeCache.txt" ]]; then
    current_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$build_directory/CMakeCache.txt" | head -n 1)"
    if [[ -n "$current_generator" && "$current_generator" != "Ninja" ]]; then
        echo "Existing build directory uses '$current_generator'; recreating it for Ninja."
        rm -rf -- "$build_directory"
    fi
fi

cmake -S "$project_directory" -B "$build_directory" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_directory" --parallel

executable="$build_directory/aowis-server-map"
if [[ ! -x "$executable" ]]; then
    echo "Error: build completed without creating the expected executable: $executable" >&2
    exit 1
fi

if [[ "$run_after_build" == false ]]; then
    echo "Build completed: $executable"
    exit 0
fi

exec "$executable" "${application_arguments[@]}"
