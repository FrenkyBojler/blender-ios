/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdint>
#include <string>

#include "BLI_hash.hh"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "DNA_packedFile_types.h"
#include "DNA_vfont_types.h"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_vfont.hh"

#include "BLF_api.hh"

#include "COM_context.hh"
#include "COM_result.hh"
#include "COM_string_image.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

/* --------------------------------------------------------------------
 * String Image Key.
 */

StringImageKey::StringImageKey(const std::string string, const VFont *font, const float size)
    : string(string), font(font), size(size)
{
}

uint64_t StringImageKey::hash() const
{
  return get_default_hash(string, font, size);
}

/* --------------------------------------------------------------------
 * String Image.
 */

static int load_font(const VFont *font)
{
  if (!font || BKE_vfont_is_builtin(font)) {
    return BLF_load_default(true);
  }

  if (font->packedfile != nullptr) {
    char name[MAX_ID_FULL_NAME];
    BKE_id_full_name_get(name, &font->id, 0);
    return BLF_load_mem_unique(
        name, static_cast<const uchar *>(font->packedfile->data), font->packedfile->size);
  }

  char file_path[FILE_MAX];
  STRNCPY(file_path, font->filepath);
  BLI_path_abs(file_path, ID_BLEND_PATH_FROM_GLOBAL(&font->id));
  return BLF_load_unique(file_path);
}

StringImage::StringImage(Context &context,
                         const std::string string,
                         const VFont *font,
                         const float size)
    : result(context.create_result(ResultType::Color))
{
  if (string.empty() || !font || size <= 0.0f) {
    this->result.allocate_invalid();
    return;
  }

  const int font_identifier = load_font(font);
  if (font_identifier == -1) {
    this->result.allocate_invalid();
    return;
  }
  BLI_SCOPED_DEFER([&]() { BLF_unload_id(font_identifier); });

  rcti box;
  BLF_size(font_identifier, size);
  BLF_boundbox(font_identifier, string.c_str(), string.length(), &box);
  const int width = BLI_rcti_size_x(&box);
  const int height = BLI_rcti_size_y(&box);

  this->result.allocate_texture(int2(width, height), false, ResultStorageType::CPU);
  parallel_for(this->result.domain().data_size,
               [&](const int2 texel) { this->result.store_pixel(texel, Color(float4(0.0f))); });

  BLF_buffer_col(font_identifier, Color(1.0f, 1.0f, 1.0f, 1.0f));
  BLF_buffer(font_identifier,
             static_cast<float *>(this->result.cpu_data().data()),
             nullptr,
             width,
             height,
             nullptr);

  BLF_position(font_identifier, -float(box.xmin), -float(box.ymin), 0.0f);
  BLF_draw_buffer(font_identifier, string.c_str(), string.length());

  BLF_buffer(font_identifier, nullptr, nullptr, 0, 0, nullptr);

  if (context.use_gpu()) {
    const Result gpu_result = this->result.upload_to_gpu(false);
    this->result.release();
    this->result = gpu_result;
  }
}

StringImage::~StringImage()
{
  this->result.release();
}

/* --------------------------------------------------------------------
 * String Image Container.
 */

void StringImageContainer::reset()
{
  /* First, delete all resources that are no longer needed. */
  map_.remove_if([](auto item) { return !item.value->needed; });

  /* Second, reset the needed status of the remaining resources to false to ready them to track
   * their needed status for the next evaluation. */
  for (auto &value : map_.values()) {
    value->needed = false;
  }
}

Result &StringImageContainer::get(Context &context,
                                  const std::string string,
                                  const VFont *font,
                                  const float size)
{
  const StringImageKey key(string, font, size);

  auto &string_image = *map_.lookup_or_add_cb(
      key, [&]() { return std::make_unique<StringImage>(context, string, font, size); });

  string_image.needed = true;
  return string_image.result;
}

}  // namespace blender::compositor
