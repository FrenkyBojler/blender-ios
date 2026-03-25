/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include <cstring>
#include <vector>

#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "CLG_log.h"

#include "ktx.h"

namespace blender {

/* KTX2 magic identifier: 12 bytes */
static const unsigned char ktx2_magic[] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

static CLG_LogRef LOG = {"image.ktx"};

const char *imb_file_extensions_ktx[] = {".ktx2", nullptr};

bool imb_is_a_ktx(const unsigned char *mem, size_t size)
{
  if (size < sizeof(ktx2_magic)) {
    return false;
  }
  return memcmp(mem, ktx2_magic, sizeof(ktx2_magic)) == 0;
}

ImBuf *imb_load_ktx(const unsigned char * /*mem*/,
                    size_t /*size*/,
                    int /*flags*/,
                    ImFileColorSpace & /*r_colorspace*/)
{
  /* TODO: implement KTX loading */
  return nullptr;
}

bool imb_save_ktx(ImBuf *ibuf, const char *filepath, int /*flags*/)
{
  // use https://github.khronos.org/KTX-Software/libktx/index.html
  // as a reference ( see "Writing a Basis-compressed Universal Texture" chapter of the url documentation )

  // Create a KTX2 teture




}

}  // namespace blender
