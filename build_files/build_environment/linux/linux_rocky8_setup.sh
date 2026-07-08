#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2022-2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# This script is part of the official build environment, see wiki page for details.
# https://developer.blender.org/docs/handbook/release_process/build/rocky_8/
set -euo pipefail

# Values below need to be aligned with build_files/config/pipeline_config.yaml
ROCM_VERSION="7.2.1"
CUDA_VERSION="12.8"
GCC_VERSION="14"
DEPS_PACKAGES=(
  git
  git-lfs
  cmake3

  # Required for 'cmake/check_software.cmake'
  autoconf
  automake
  bison
  libtool        # libtoolize
  flex
  ninja-build    # ninja
  yasm
  gettext-devel  # autopoint
  patchelf
  help2man
  texinfo        # makeinfo

  # Required for 'external_ssl'
  # perl-Net-Curl # Cannot find "WWW::Curl::Easy" in podpath: cannot find suitable replacement path, cannot resolve link
  perl-IPC-Cmd
  perl-Time-Piece
  perl-Pod-Html

  # Required for 'external_openal'
  pulseaudio-libs-devel
  alsa-lib-devel

  # Required for 'external_openjph'
  patch

  # Required for 'external_epoxy'
  libX11-devel   # for <X11/Xlib.h>
  libglvnd-devel # for <EGL/eglplatform.h>

  # Required for 'external_sdl'
  # Source: https://wiki.libsdl.org/SDL3/README-linux#build-dependencies
  alsa-lib-devel
  # fribidi-devel
  pulseaudio-libs-devel
  # pipewire-devel
  libX11-devel
  # libXext-devel
  libXrandr-devel
  libXcursor-devel
  # libXfixes-devel
  libXi-devel
  # libXScrnSaver-devel
  # libXtst-devel
  # dbus-devel
  # ibus-devel
  systemd-devel
  mesa-libGL-devel
  libxkbcommon-devel
  # mesa-libGLES-devel
  mesa-libEGL-devel
  # vulkan-devel
  # wayland-devel
  # wayland-protocols-devel
  libdrm-devel
  mesa-libgbm-devel
  # libusb1-devel
  # libdecor-devel
  # jack-audio-connection-kit-devel
  # libthai-devel

  # Required for 'external_materialx'
  libXt-devel

  # bzip2
  # make
  # autoconf
  # automake
  # libtool
  # patchelf
  # mesa-libGL-devel
  # mesa-libGLU-devel
  # zlib-devel
  # tcl
  # python3
  # python3-mako
  # python3-pyyaml
  # bison
  # flex
  # ncurses-devel
  # libstdc++-static
  # cairo-devel
  # libdrm-devel
  # pixman-devel
  # libffi-devel
  # libinput-devel
  # libevdev-devel
  # mesa-libgbm-devel
  # systemd-devel
  # mesa-dri-drivers
  # mesa-libEGL
  # mesa-libGL
  # libxkbcommon-devel
  # libX11-devel
  # libXcursor-devel
  # libXi-devel
  # libXinerama-devel
  # libXrandr-devel
  # libXt-devel
  # libXxf86vm-devel
)

ASSUME_YES="${ASSUME_YES:-0}"
VERBOSE=0

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -v|--verbose)
        VERBOSE=1
        shift
        ;;
      -y|--yes)
        ASSUME_YES=1
        shift
        ;;
      *)
        echo "[main]: Unknown arg: $1"
        exit 1
        ;;
    esac
  done
}

check_requirements() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "[check_requirements]: This script must be run as root"
    exit 1
  fi

  if [ ! -f /etc/os-release ]; then
    echo "[check_requirements]: Cannot detect OS. /etc/os-release missing."
    exit 1
  fi

  . /etc/os-release

  case "$ID" in
    rocky|rhel|almalinux) ;;
    *)
      echo "[check_requirements]: Unsupported OS: $ID. Script need Rocky/RHEL/Alma 8."
      exit 1
      ;;
  esac

  if [ "${VERSION_ID%%.*}" != "8" ]; then
    echo "[check_requirements]: Unsupported version: $VERSION_ID. Script need major version 8."
    exit 1
  fi

  ARCH=$(uname -m)
}

show_warning() {
  if [[ "${ASSUME_YES}" != "1" ]]; then
    cat <<EOF
######################### WARNING ##########################
This script will install repositories and packages for
building Blender library dependencies, including:
  - GCC $GCC_VERSION
  - CUDA $CUDA_VERSION
EOF
    if [ "$ARCH" = "x86_64" ]; then
      echo "  - ROCm $ROCM_VERSION"
    fi
    cat <<EOF
############################################################
EOF
    read -r -p "Continue? [y/N] " CONFIRM
    [[ "${CONFIRM}" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
  fi
}

dnf() {
  if [[ "${VERBOSE}" == "1" ]]; then
    command dnf "$@"
  else
    command dnf "$@" > /dev/null
  fi
}

# Repos
dnf_config() {
  echo "[dnf_config]: Configuring DNF subcommands and repositories"

  # Required by config manager command below to enable powertools
  echo "[dnf_config]: Installing config-manager subcommand"
  dnf -y install 'dnf-command(config-manager)'

  # Required for version locking CUDA installation
  echo "[dnf_config]: Installing versionlock subcommand"
  dnf -y install 'dnf-command(versionlock)'

  # Packages `ninja-build` and `meson` are not available unless CBR or PowerTools repositories are enabled.
  # See: https://wiki.rockylinux.org/rocky/repo/#notes-on-unlisted-repositories
  echo "[dnf_config]: Enable PowerTools repository"
  dnf config-manager --set-enabled powertools

  # Required by epel-release has the patchelf and rubygem-asciidoctor packages
  echo "[dnf_config]: Enable EPEL repository"
  dnf -y install epel-release

  echo "[dnf_config]: Done"
}

# GCC toolset
install_gcc() {
  local VERSION="${1:?Usage: install_gcc <version>}"

  echo "[install_gcc]: Installing SCL utilities"
  # Keep this separate from the packages install, since otherwise older tool-chain will be installed.
  dnf -y install scl-utils
  dnf -y install scl-utils-build

  echo "[install_gcc]: Installing GCC $VERSION"
  dnf -y install gcc-toolset-$VERSION

  # Set as default shell env
  echo "[install_gcc]: Enabling GCC $VERSION by default via /etc/profile.d"
  tee /etc/profile.d/enablegcc$VERSION.sh > /dev/null <<PROFEOF
#!/bin/bash
source scl_source enable gcc-toolset-$VERSION
PROFEOF
  chmod +x /etc/profile.d/enablegcc$VERSION.sh

  echo "[install_gcc]: GCC $VERSION installed"

  echo "[install_gcc]: Re-open shell or run 'source /etc/profile.d/enablegcc14.sh' to enable GCC 14"
}

# CUDA
install_cuda() {
  local VERSION="${1:?Usage: install_cuda <version>}"
  local DNF_VER="${VERSION//./-}"

  echo "[install_cuda]: Adding CUDA repository"
  # There is no AArch64 repo, instead we use SBSA which works for device binaries
  local CUDA_ARCH="x86_64"
  if [ "$ARCH" = "aarch64" ]; then
      CUDA_ARCH="sbsa"
  fi
  dnf config-manager --add-repo "http://developer.download.nvidia.com/compute/cuda/repos/rhel8/${CUDA_ARCH}/cuda-rhel8.repo"

  # Without setting up a version lock "dnf update" commands may pull in newer CUDA package versions
  echo "[install_cuda]: Configuring CUDA version lock"
  dnf versionlock add "cuda-*-${DNF_VER}*"

  echo "[install_cuda]: Installing CUDA $VERSION"
  dnf -y install "cuda-toolkit-${DNF_VER}"

  echo "[install_cuda]: CUDA $VERSION installed"
}

# ROCm
install_rocm() {
  local VERSION="${1:?Usage: install_rocm <version>}"
  # Source: https://rocm.docs.amd.com/projects/install-on-linux/en/docs-7.2.1/install/install-methods/package-manager/package-manager-rl.html

  # There is no aarch64 repo for rhel8 systems
  if [ "$ARCH" = "aarch64" ]; then
    echo "[install_rocm]: Skipping (aarch64 has no ROCm repo)"
    return 0
  fi

  echo "[install_rocm]: Adding ROCm $VERSION repository"
  rpm --import https://repo.radeon.com/rocm/rocm.gpg.key
  tee /etc/yum.repos.d/graphics-$VERSION.repo > /dev/null <<EOF
[graphics-$VERSION]
name=graphics-$VERSION
baseurl=https://repo.radeon.com/graphics/$VERSION/el/8.10/main/x86_64/
enabled=1
priority=50
gpgcheck=1
gpgkey=https://repo.radeon.com/rocm/rocm.gpg.key
EOF

  tee /etc/yum.repos.d/rocm-$VERSION.repo > /dev/null <<EOF
[ROCm-$VERSION]
name=ROCm-$VERSION
baseurl=https://repo.radeon.com/rocm/el8/$VERSION/main
enabled=1
gpgcheck=1
exclude=rock-dkms
gpgkey=https://repo.radeon.com/rocm/rocm.gpg.key
EOF

  dnf -y makecache

  echo "[install_rocm]: Installing ROCm $VERSION"

  dnf -y install hipcc$VERSION hip-devel$VERSION rocm-llvm$VERSION rocm-core$VERSION rocm-device-libs$VERSION
  update-alternatives --set rocm /opt/rocm-$VERSION

  echo "[install_rocm]: ROCm $VERSION installed"
}

# Packages
install_packages() {
  local -n PACKAGES="${1:?Usage: install_packages <array_name>}"
  echo "[install_packages]: Installing packages"

  dnf -y install "${PACKAGES[@]}"

  echo "[install_packages]: Packages installed"
}

parse_args "$@"
check_requirements
show_warning
dnf_config
install_cuda "${CUDA_VERSION}"
install_rocm "${ROCM_VERSION}"
install_gcc "${GCC_VERSION}"
install_packages DEPS_PACKAGES

echo "[main]: Completed"
