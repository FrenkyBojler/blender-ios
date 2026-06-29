/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "gpu_util_intel.hh"

namespace blender::gpu {

IntelGpuArch get_intel_gpu_arch(uint32_t device_id)
{
  /* Source for device IDs:
   * https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/include/pci_ids/iris_pci_ids.h
   */
  switch (device_id & 0xFF00) {
    case 0x2900:  // Broadwater
    case 0x2A00:  // Broadwater/Eagle Lake
    case 0x2E00:  // Eagle Lake
    case 0x0000:  // Iron Lake
    case 0x0100:  // Ivy Bridge/Sandy Bridge/Baytrail
    case 0x0F00:  // Baytrail
    case 0x0400:  // Haswell
    case 0x0C00:  // Haswell
    case 0x0D00:  // Haswell
    case 0x0A00:  // Haswell / Appollo Lake
    case 0x2200:  // Cherrytrail
    case 0x1600:  // Broadwell
    case 0x5A00:  // Apollo Lake
    case 0x1900:  // Skylake
    case 0x1A00:  // Apollo Lake
    case 0x3100:  // Gemini Lake
    case 0x5900:  // Kaby Lake/Amber Lake
    case 0x8700:  // Kaby Lake/Coffee Lake
    case 0x3E00:  // Coffee Lake/Whiskey Lake
    case 0x9B00:  // Comet Lake
      return IntelGpuArch::Gen9AndOlder;
    case 0x8A00:  // Ice Lake
    case 0x4500:  // Elkhart Lake
    case 0x4E00:  // Jasper Lake
      return IntelGpuArch::Gen11;
    case 0x9A00:  // Tiger Lake
    case 0x4C00:  // Rocket Lake
    case 0x4900:  // DG1
    case 0x4600:  // Alder Lake
    case 0x4F00:  // Alchemist
    case 0x5600:  // Alchemist
    case 0xA700:  // Raptor Lake
    case 0x7D00:  // Meteor Lake / Arrow Lake
    case 0xB600:  // Meteor Lake / Arrow Lake
      return IntelGpuArch::Xe;
    case 0x6400:  // Lunar Lake
    case 0xE200:  // Battlemage
      return IntelGpuArch::Xe2;
    case 0xB000:  // Panther Lake
    case 0xFD00:  // Wildcat Lake
    default:
      return IntelGpuArch::Xe3AndNewer;
  }
}

}  // namespace blender::gpu
