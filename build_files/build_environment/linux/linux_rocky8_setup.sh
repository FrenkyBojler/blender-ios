#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# This script is part of the official build environment, see wiki page for details.
# https://developer.blender.org/docs/handbook/release_process/build/rocky_8/
set -euo pipefail

ASSUME_YES="${ASSUME_YES:-0}"
SKIP_BLENDER_PACKAGES="${SKIP_BLENDER_PACKAGES:-0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -y|--yes)
      ASSUME_YES=1
      shift
      ;;
    --no-blender-packages)
      SKIP_BLENDER_PACKAGES=1
      shift
      ;;
    *)
      echo "Unknown arg: $1"
      exit 1
      ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "This script must be run as root"
  exit 1
fi

# Current architecture
ARCH=$(uname -i)

if [[ "${ASSUME_YES}" != "1" ]]; then
  cat <<EOF
######################### WARNING ##########################
This script will install software on your system:
  - PowerTools/EPEL repos
  - gcc-toolset-14 (set as default via profile.d)
  - CUDA 12.8 (version locked)
  - ROCm 7.2.1 (non-aarch64 only)
  - Many dnf packages for Blender dependencies
EOF
  if [[ "${SKIP_BLENDER_PACKAGES}" != "1" ]]; then
    echo "  - Packages to build Blender (e.g. X11/Wayland)"
  fi
  cat <<EOF
############################################################
EOF
  read -r -p "Continue? [y/N] " CONFIRM
  [[ "${CONFIRM}" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
fi

# Repos
enable_repos() {
  echo "[repos]: Enabling config-manager, versionlock, powertools, epel"

  # Required by config manager command below to enable powertools
  dnf -y -q install 'dnf-command(config-manager)'

  # Required for version locking CUDA installation
  dnf -y -q install 'dnf-command(versionlock)'

  # Packages `ninja-build` and `meson` are not available unless CBR or PowerTools repositories are enabled.
  # See: https://wiki.rockylinux.org/rocky/repo/#notes-on-unlisted-repositories
  dnf config-manager --set-enabled powertools

  # Required by epel-release has the patchelf and rubygem-asciidoctor packages
  dnf -y -q install epel-release

  echo "[repos]: Done"
}

# GCC toolset
install_gcc() {
  echo "[gcc]:   Installing gcc-toolset-14"

  # Install all the packages needed for a new tool-chain.
  #
  # NOTE: Keep this separate from the packages install, since otherwise
  # older tool-chain will be installed.
  dnf -y -q install scl-utils
  dnf -y -q install scl-utils-build

  # Currently this is defined by the VFX platform (CY2026), see: https://vfxplatform.com
  dnf -y -q install gcc-toolset-14

  # Set gcc-toolset-14 as default shell env, so nvcc/other tools building
  # against it don't need to be invoked with `scl enable` manually.
  echo "[gcc]:   Enabling gcc-toolset-14 by default via /etc/profile.d"
tee /etc/profile.d/enablegcc14.sh > /dev/null <<'PROFEOF'
#!/bin/bash
set +u
source scl_source enable gcc-toolset-14
set -u
PROFEOF
  chmod +x /etc/profile.d/enablegcc14.sh
  set +u
  source /etc/profile.d/enablegcc14.sh
  set -u

  echo "[gcc]:   gcc-toolset-14 installed"
}

# CUDA
install_cuda() {
  echo "[cuda]:  Adding CUDA repository"

  # For RHEL8 there is no aarch64 repo so instead we use sbsa which works for device binaries
  # For RHEL9 the aarch64 repo exists and this fallback will no longer be needed
  local CUDA_ARCH="x86_64"
  if [ "$ARCH" = "aarch64" ]; then
      CUDA_ARCH="sbsa"
  fi

  # Repository for CUDA (`nvcc`)
  dnf config-manager --add-repo "http://developer.download.nvidia.com/compute/cuda/repos/rhel8/${CUDA_ARCH}/cuda-rhel8.repo"

  echo "[cuda]:  Configuring CUDA version lock"
  dnf versionlock add 'cuda-*-12-8*'

  echo "[cuda]:  Installing CUDA 12.8"
  dnf install -y -q cuda-toolkit-12-8

  echo "[cuda]:  CUDA 12.8 Installed"
}

# ROCm
install_rocm() {
  # AMD's ROCM
  # Based on instructions from:
  # https://rocm.docs.amd.com/projects/install-on-linux/en/latest/how-to/native-install/rhel.html
  # NOTE: the following steps have intentionally been skipped as they aren't needed:
  # - "Register kernel-mode driver".
  # - "Install kernel driver".

  # For ROCm there is no aarch64 repo
  if [ "$ARCH" = "aarch64" ]; then
    echo "[rocm]:  Skipping (aarch64 has no ROCm repo)"
    return 0
  fi

  echo "[rocm]:  Adding ROCm 7.2.1 repository"
  rpm --import https://repo.radeon.com/rocm/rocm.gpg.key
  tee /etc/yum.repos.d/graphics-7.2.1.repo > /dev/null <<EOF
[graphics-7.2.1]
name=graphics-7.2.1
baseurl=https://repo.radeon.com/graphics/7.2.1/el/8.10/main/x86_64/
enabled=1
priority=50
gpgcheck=1
gpgkey=https://repo.radeon.com/rocm/rocm.gpg.key
EOF

  tee /etc/yum.repos.d/rocm-7.2.1.repo > /dev/null <<EOF
[ROCm-7.2.1]
name=ROCm-7.2.1
baseurl=https://repo.radeon.com/rocm/el8/7.2.1/main
enabled=1
gpgcheck=1
exclude=rock-dkms
gpgkey=https://repo.radeon.com/rocm/rocm.gpg.key
EOF

  dnf -y -q update

  echo "[rocm]:  Installing ROCm 7.2.1"

  dnf -y -q install hipcc7.2.1 hip-devel7.2.1 rocm-llvm7.2.1 rocm-core7.2.1 rocm-device-libs7.2.1
  update-alternatives --set rocm /opt/rocm-7.2.1

  echo "[rocm]:  ROCm 7.2.1 installed"
}

# Packages
install_packages() {
  echo "[pkgs]:  Installing Blender dependency packages"

  local PACKAGES_FOR_LIBS=(
      git
      git-lfs
      bzip2
      tar
      cmake3
      patch
      make

      # Required by `external_nasm` which uses an `autoconf` build-system
      autoconf
      automake
      libtool

      # Required by `flex`
      help2man

      # Required by `external_libsndfile` configure scripts
      autogen

      # Used to set rpath on shared libraries
      patchelf

      # Builds generated by meson use Ninja for the actual build
      ninja-build

      # Required by `WITH_GHOST_WAYLAND` build option
      mesa-libEGL-devel
      # Required by `external_opensubdiv` and Blender
      mesa-libGL-devel
      mesa-libGLU-devel

      # NOTE(@ideasman42): Currently flex's `autogen.sh` is required to run because the bundled
      # configuration is looking for an older version of `aclocal` than the system provides
      # This is resolved by generating new configuration files which requires the `autopoint`
      # command from `gettext-devel`, if the flex package is updated we could remove this
      # Required by `flex` running `autogen.sh` for `autopoint`
      gettext-devel
      # NOTE(@ideasman42): It seems newer files generated by `autogen.sh` also require `makeinfo`
      # and there isn't a flag to disable GNU "info"
      # Required by `flex` as a build-time dependency for `makeinfo`
      texinfo

      # Required by `external_ispc`
      zlib-devel
      # TODO: dependencies build without this, consider removal
      rubygem-asciidoctor
      # TODO: dependencies build without this, consider removal
      wget
      # Required by `external_sqlite` as a build-time dependency (needed for the `tclsh` command)
      tcl
      # Required by `external_aom`
      # TODO: Blender is already building `external_nasm` which is listed as an alternative to `yasm`
      # Why are both needed?
      yasm

      # NOTE(@ideasman42): while `python39` is available, the default Python version is 3.6
      # For example, this is used for the `python3-mako` package
      # So use the "default" system Python since it means it's most compatible with other packages
      python3

      # Required by `external_igc`
      python3-mako
      python3-pyyaml

      # Required by `external_igc` and `external_osl` as a build-time dependency
      bison
      # Required by `external_osl` as a build-time dependency
      flex

      # Required by `external_ispc`
      ncurses-devel
      # Required by `external_ispc` when building with Clang
      libstdc++-static

      # Required by `external_ssl` as build-time dependency
      perl-core
      perl-IPC-Cmd
      perl-Pod-Html

      # Required by `external_wayland_weston`
      cairo-devel
      libdrm-devel
      pixman-devel
      libffi-devel
      libinput-devel
      libevdev-devel
      mesa-libgbm-devel
      # Required by `libudev`
      systemd-devel
      # Required by `weston --headless` as run-time requirement for off screen rendering
      mesa-dri-drivers
      mesa-libEGL
      mesa-libGL
  )

  dnf -y -q install "${PACKAGES_FOR_LIBS[@]}"

  if [[ "${SKIP_BLENDER_PACKAGES}" != "1" ]]; then
    # Additional packages needed for building Blender
    local PACKAGES_FOR_BLENDER=(
        # Required for `WITH_GHOST_WAYLAND` build option
        libxkbcommon-devel

        # Required for `WITH_GHOST_X11` build option
        libX11-devel
        libXcursor-devel
        libXi-devel
        libXinerama-devel
        libXrandr-devel
        libXt-devel
        libXxf86vm-devel
    )
    dnf -y -q install "${PACKAGES_FOR_BLENDER[@]}"
  else
    echo "[pkgs]:  Skipping Blender GUI packages (--no-blender-packages)"
  fi

  # Dependencies for pip
  dnf -y -q install python3 python3-pip python3-devel

  # Dependencies for asound
  dnf -y -q install alsa-lib-devel pulseaudio-libs-devel

  # Required for `WITH_JACK` build option
  dnf -y -q install jack-audio-connection-kit-devel

  echo "[pkgs]:  Packages installed"
}

enable_repos
install_gcc
install_cuda
install_packages
install_rocm

echo "[done]:  Setup completed"
