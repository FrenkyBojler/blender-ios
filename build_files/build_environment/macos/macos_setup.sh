#!/usr/bin/env bash
#
# macOS build environment setup for Blender dependencies
# Combines: Homebrew, Xcode (with Metal), CMake, brew packages
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Versions and packages
XCODE_VERSIONS=("15.0")
CMAKE_VERSION="3.31.12"

BREW_PACKAGES=(
    autoconf
    automake
    bison
    dos2unix
    flex
    libtool
    meson
    ninja
    pkg-config
    yasm
)

# XIP location resolve order: env/flag > script dir
XIP_LOCATION="${XIP_LOCATION:-${SCRIPT_DIR}}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --xip-location)
            XIP_LOCATION="$2"
            shift 2
            ;;
        *)
            echo "Unknown arg: $1"
            exit 1
            ;;
    esac
done

# Warning prompt
cat <<EOF
############################################################
WARNING
This script installs/modifies system-wide stuff:
  - Homebrew (+ shellenv in ~/.zprofile)
  - Xcode: ${XCODE_VERSIONS[*]} (with Metal Toolchain)
  - CMake ${CMAKE_VERSION}
  - Brew packages: ${BREW_PACKAGES[*]}

Recommended ONLY on a fresh system / VM.
############################################################
EOF
read -r -p "Continue? [y/N] " CONFIRM
[[ "${CONFIRM}" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }

# Homebrew
install_homebrew() {
    if command -v brew >/dev/null 2>&1; then
        echo "Homebrew already installed"
        return 0
    fi
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    echo >> "${HOME}/.zprofile"
    echo 'eval "$(/opt/homebrew/bin/brew shellenv zsh)"' >> "${HOME}/.zprofile"
    eval "$(/opt/homebrew/bin/brew shellenv zsh)"
}

# Xcode
install_xcode() {
    local VERSIONS=("$@")
    local LATEST
    LATEST=$(printf '%s\n' "${VERSIONS[@]}" | sort -V | tail -1)

    for VERSION in "${VERSIONS[@]}"; do
        local XIP="${XIP_LOCATION}/Xcode_${VERSION}.xip"
        local APP="/Applications/Xcode-${VERSION}.app"
        local MAJOR="${VERSION%%.*}"
        local DL_URL="https://developer.apple.com/services-account/download?path=/Developer_Tools/Xcode_${MAJOR}/Xcode_${MAJOR}.xip"

        if [ -d "${APP}" ]; then
            echo "Xcode ${VERSION} already installed"
            continue
        fi

        if [ ! -f "${XIP}" ]; then
            echo "Missing ${XIP}"
            echo "Grab it (Apple account needed):"
            echo "  ${DL_URL}"
            echo "Put file at: ${XIP}"
            echo "Then re-run script."
            exit 1
        fi

        rm -rf /tmp/Xcode.app
        cd /tmp && xip -x "${XIP}"
        sudo mv /tmp/Xcode.app "${APP}"
        rm -f "${XIP}"

        echo "Xcode ${VERSION} installed"
    done

    sudo xcode-select -s "/Applications/Xcode-${LATEST}.app/Contents/Developer"
    sudo xcodebuild -license accept
    sudo xcodebuild -runFirstLaunch
    sudo xcodebuild -downloadComponent MetalToolchain
}

# CMake
install_cmake() {
    local VERSION="${1:?Usage: install_cmake <version>}"
    local DMG_NAME="cmake-${VERSION}-macos-universal.dmg"
    local TMP_PATH="/tmp/${DMG_NAME}"
    local MOUNT_POINT="/Volumes/CMake"
    local BIN_DIR="/usr/local/bin"

    if "${BIN_DIR}/cmake" --version 2>/dev/null | grep -q "${VERSION}"; then
        echo "CMake ${VERSION} already installed"
        return 0
    fi

    sudo rm -f "${BIN_DIR}/cmake"
    sudo rm -rf /Applications/CMake.app

    sudo mkdir -p "${BIN_DIR}"
    sudo chown root:wheel "${BIN_DIR}"
    sudo chmod 755 "${BIN_DIR}"

    curl -L "https://github.com/Kitware/CMake/releases/download/v${VERSION}/${DMG_NAME}" -o "${TMP_PATH}"

    hdiutil attach "${TMP_PATH}" -mountpoint "${MOUNT_POINT}"
    sudo cp -R "${MOUNT_POINT}/CMake.app" /Applications/
    sudo ln -sf /Applications/CMake.app/Contents/bin/* "${BIN_DIR}/"

    hdiutil detach "${MOUNT_POINT}"
    rm -f "${TMP_PATH}"

    echo "CMake ${VERSION} installed: $("${BIN_DIR}/cmake" --version | head -1)"
}

install_homebrew
install_xcode "${XCODE_VERSIONS[@]}"
install_cmake "${CMAKE_VERSION}"
brew install "${BREW_PACKAGES[@]}"
