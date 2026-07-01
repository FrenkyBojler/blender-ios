#!/usr/bin/env bash
#
# macOS build environment setup for Blender dependencies
# Combines: Homebrew, Xcode (with Metal Toolchain), CMake, brew packages
set -euo pipefail

if [ "$(uname -m)" != "arm64" ]; then
  echo "Only ARM64 is supported"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Versions and packages
XCODE_VERSION="26.1.1"
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
This script will install software on your system:
  - Homebrew (+ shellenv in ~/.zprofile)
  - Xcode ${XCODE_VERSION} (with Metal Toolchain)
  - CMake ${CMAKE_VERSION}
  - Brew packages: ${BREW_PACKAGES[*]}
############################################################
EOF
  read -r -p "Continue? [y/N] " CONFIRM
  [[ "${CONFIRM}" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
fi

# Homebrew
install_homebrew() {
  if command -v brew >/dev/null 2>&1; then
    echo "[brew]:  Homebrew already installed"
    return 0
  fi
  echo "[brew]:  Installing Homebrew..."
  NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  local SHELLENV_LINE='eval "$(/opt/homebrew/bin/brew shellenv zsh)"'
  if grep -qxF "${SHELLENV_LINE}" "${HOME}/.zprofile" 2>/dev/null; then
    echo "[brew]:  .zprofile already set"
  else
    echo "[brew]:  Adding shellenv to .zprofile"
    echo >> "${HOME}/.zprofile"
    echo "${SHELLENV_LINE}" >> "${HOME}/.zprofile"
  fi
  eval "${SHELLENV_LINE}"
  echo "[brew]:  Homebrew installed"
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
      echo "[xcode]: Missing ${XIP}"
      echo "[xcode]: Download URL (Apple account needed):"
      echo "[xcode]:  ${DL_URL}"
      echo "[xcode]: Save file here: ${XIP}"
      echo "[xcode]: Waiting for file..."
      until [ -f "${XIP}" ]; do
        sleep 5
      done
      echo "[xcode]: Found file. Continuing."
    fi

    echo "[xcode]: Extracting xip..."
    rm -rf /tmp/Xcode.app
    cd /tmp && xip -x "${XIP}"

    echo "[xcode]: Moving to ${APP}"
    sudo mv /tmp/Xcode.app "${APP}"
    rm -f "${XIP}"
    echo "[xcode]: Xcode ${VERSION} installed"
  else
    echo "[xcode]: Xcode ${VERSION} already installed"
  fi

  echo "[xcode]: Selecting Xcode ${VERSION}"
  sudo xcode-select -s "${APP}/Contents/Developer"
  sudo xcodebuild -license accept
  echo "[xcode]: Running first launch"
  sudo xcodebuild -runFirstLaunch

  echo "[xcode]: Checking MetalToolchain support"
  if xcodebuild -downloadComponent 2>&1 | grep -qv "invalid option"; then
    echo "[xcode]: Downloading MetalToolchain"
    sudo xcodebuild -downloadComponent MetalToolchain
    echo "[xcode]: MetalToolchain installed"
  else
    echo "[xcode]: Xcode ${VERSION} has MetalToolchain embedded. Skip downloading."
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
    echo "[cmake]: CMake ${VERSION} already installed"
    return 0
  fi

  echo "[cmake]: Removing old install"
  sudo rm -f "${BIN_DIR}/cmake"
  sudo rm -rf /Applications/CMake.app
  sudo mkdir -p "${BIN_DIR}"
  sudo chown root:wheel "${BIN_DIR}"
  sudo chmod 755 "${BIN_DIR}"

  echo "[cmake]: Downloading ${VERSION}"
  curl -L "https://github.com/Kitware/CMake/releases/download/v${VERSION}/${DMG_NAME}" -o "${TMP_PATH}"

  echo "[cmake]: Mounting dmg"
  hdiutil attach "${TMP_PATH}" -mountpoint "${MOUNT_POINT}"
  sudo cp -R "${MOUNT_POINT}/CMake.app" /Applications/
  sudo ln -sf /Applications/CMake.app/Contents/bin/* "${BIN_DIR}/"

  echo "[cmake]: Unmounting, cleanup"
  hdiutil detach "${MOUNT_POINT}"
  rm -f "${TMP_PATH}"

  echo "[cmake]: CMake ${VERSION} installed: $("${BIN_DIR}/cmake" --version | head -1)"
}

install_brew_packages() {
  echo "[brew]:  Installing:"
  printf '  - %s\n' "${BREW_PACKAGES[@]}"
  brew install -y "${BREW_PACKAGES[@]}"
}

sudo_keepalive() {
  sudo -v
  ( while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null ) &
  SUDO_KEEPALIVE_PID=$!
  trap 'kill "${SUDO_KEEPALIVE_PID}" 2>/dev/null || true' EXIT
}

install_homebrew
sudo_keepalive
install_xcode "${XCODE_VERSION}"
install_cmake "${CMAKE_VERSION}"
install_brew_packages
