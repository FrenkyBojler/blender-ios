#!/usr/bin/env bash
#
# macOS build environment setup for Blender dependencies
# Combines: Homebrew, Xcode (with Metal Toolchain), CMake, brew packages
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Versions and packages
XCODE_VERSION="15"
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
ASSUME_YES="${ASSUME_YES:-0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --xip-location)
            XIP_LOCATION="$2"
            shift 2
            ;;
        -y|--yes)
            ASSUME_YES=1
            shift
            ;;
        *)
            echo "Unknown arg: $1"
            exit 1
            ;;
    esac
done

# Warning prompt
if [[ "${ASSUME_YES}" != "1" ]]; then
    cat <<EOF
############################################################
WARNING
This script installs/modifies system-wide stuff:
  - Homebrew (+ shellenv in ~/.zprofile)
  - Xcode ${XCODE_VERSION} (with Metal Toolchain)
  - CMake ${CMAKE_VERSION}
  - Brew packages: ${BREW_PACKAGES[*]}

Recommended ONLY on a fresh system / VM.
############################################################
EOF
    read -r -p "Continue? [y/N] " CONFIRM
    [[ "${CONFIRM}" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
fi

# sudo upfront, keepalive bg loop so no repeat prompts
sudo -v
( while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill "${SUDO_KEEPALIVE_PID}" 2>/dev/null || true' EXIT

# Homebrew
install_homebrew() {
    if command -v brew >/dev/null 2>&1; then
        echo "Homebrew already installed"
        return 0
    fi
    NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    echo >> "${HOME}/.zprofile"
    echo 'eval "$(/opt/homebrew/bin/brew shellenv zsh)"' >> "${HOME}/.zprofile"
    eval "$(/opt/homebrew/bin/brew shellenv zsh)"
}

# Xcode
install_xcode() {
    local VERSION="$1"
    local XIP="${XIP_LOCATION}/Xcode_${VERSION}.xip"
    local APP="/Applications/Xcode-${VERSION}.app"
    local MAJOR="${VERSION%%.*}"
    local DL_URL="https://developer.apple.com/services-account/download?path=/Developer_Tools/Xcode_${MAJOR}/Xcode_${MAJOR}.xip"

    if [ ! -d "${APP}" ]; then
        if [ ! -f "${XIP}" ]; then
            echo "Missing ${XIP}"
            echo "Grab it (Apple account needed):"
            echo "  ${DL_URL}"
            echo "Put file at: ${XIP}"
            echo "Waiting..."
            until [ -f "${XIP}" ]; do
                sleep 5
            done
            echo "Found. Continuing."
        fi

        rm -rf /tmp/Xcode.app
        cd /tmp && xip -x "${XIP}"
        sudo mv /tmp/Xcode.app "${APP}"
        rm -f "${XIP}"
        echo "Xcode ${VERSION} installed"
    else
        echo "Xcode ${VERSION} already installed"
    fi

    sudo xcode-select -s "${APP}/Contents/Developer"
    sudo xcodebuild -license accept
    sudo xcodebuild -runFirstLaunch
    if xcodebuild -downloadComponent 2>&1 | grep -qv "invalid option"; then
        sudo xcodebuild -downloadComponent MetalToolchain
    else
        echo "Xcode ${VERSION} no support MetalToolchain download. Skip."
    fi
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
install_xcode "${XCODE_VERSION}"
install_cmake "${CMAKE_VERSION}"
brew install "${BREW_PACKAGES[@]}"
