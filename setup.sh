#!/usr/bin/env bash
# Installs the build dependencies, builds the engine and optionally runs the self-tests.
# Usage: ./setup.sh [--no-install] [--debug] [--sanitize] [--examples] [--test] [--help]
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

install_deps=1
build_type=Release
build_dir=build
sanitize=OFF
examples=OFF
run_tests=0

usage() {
    sed -n '2,3p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'TXT'

  --no-install  skip the system package step (use when dependencies are already present)
  --debug       Debug build instead of Release
  --sanitize    Debug build with AddressSanitizer and UBSan, in build-sanitize/
  --examples    also build the native plugin example
  --test        run ./dfe --selftest after building
TXT
}

for arg in "$@"; do
    case "$arg" in
        --no-install) install_deps=0 ;;
        --debug) build_type=Debug ;;
        --sanitize) build_type=Debug; sanitize=ON; build_dir=build-sanitize ;;
        --examples) examples=ON ;;
        --test) run_tests=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 1 ;;
    esac
done

say() { printf '\n==> %s\n' "$*"; }

sudo_cmd() {
    if [ "$(id -u)" -eq 0 ]; then "$@"; elif command -v sudo >/dev/null 2>&1; then sudo "$@"; else
        echo "Root access is needed to install packages. Install them yourself and rerun with --no-install." >&2
        exit 1
    fi
}

install_packages() {
    case "$(uname -s)" in
        Darwin)
            command -v brew >/dev/null 2>&1 || { echo "Install Homebrew first: https://brew.sh" >&2; exit 1; }
            brew install cmake ninja glfw pkg-config
            ;;
        Linux)
            if command -v pacman >/dev/null 2>&1; then
                sudo_cmd pacman -S --needed --noconfirm base-devel cmake ninja git glfw libx11 libxrandr libxinerama libxcursor libxi
            elif command -v apt-get >/dev/null 2>&1; then
                sudo_cmd apt-get update
                sudo_cmd apt-get install -y --no-install-recommends build-essential cmake ninja-build git pkg-config \
                    libglfw3-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxxf86vm-dev
            elif command -v dnf >/dev/null 2>&1; then
                sudo_cmd dnf install -y gcc make cmake ninja-build git pkgconf-pkg-config glfw-devel \
                    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXxf86vm-devel
            elif command -v zypper >/dev/null 2>&1; then
                sudo_cmd zypper install -y gcc make cmake ninja git pkg-config glfw3-devel \
                    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXxf86vm-devel
            else
                echo "Unrecognised Linux distribution. Install CMake, Ninja, a C compiler and the GLFW and X11 development files, then rerun with --no-install." >&2
                exit 1
            fi
            ;;
        *)
            echo "Unsupported system. On Windows use CMake with Visual Studio or MSYS2 as described in the README." >&2
            exit 1
            ;;
    esac
}

if [ "$install_deps" -eq 1 ]; then
    say "Installing dependencies"
    install_packages
fi

for tool in cmake cc; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool" >&2; exit 1; }
done

generator=()
if command -v ninja >/dev/null 2>&1; then generator=(-G Ninja); fi

say "Configuring ($build_type) in $build_dir/"
cmake -S . -B "$build_dir" "${generator[@]}" -DCMAKE_BUILD_TYPE="$build_type" \
    -DDFE_ENABLE_SANITIZERS="$sanitize" -DDFE_BUILD_EXAMPLES="$examples"

say "Building"
cmake --build "$build_dir" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

mkdir -p saves

if [ "$run_tests" -eq 1 ]; then
    say "Running self-tests"
    "./$build_dir/dfe" --selftest
fi

say "Done. Run it from this folder with: ./$build_dir/dfe"
