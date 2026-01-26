/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_build_config.h"
#include "BLI_enum_flags.hh"

#include "BLT_translation.hh"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_dynamic_override_types.h"

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

namespace blender {

namespace bke {

struct DynamicOverrideRuntime {};

}  // namespace bke

static void dynoverride_init_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

static void dynoverride_copy_data(Main * /*bmain*/,
                                  std::optional<Library *> /*owner_library*/,
                                  ID *id_dst,
                                  const ID * /*id_src*/,
                                  const int /*flag*/)
{
  DynamicOverride *dynoverride_dst = id_cast<DynamicOverride *>(id_dst);

  dynoverride_dst->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

static void dynoverride_free_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  MEM_delete(dynoverride->runtime);
}

static void dynoverride_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  dynoverride->runtime = nullptr;

  /* write LibData */
  writer->write_id_struct(id_address, dynoverride);
  BKE_id_blend_write(writer, &dynoverride->id);
}

static void dynoverride_blend_read_data(BlendDataReader * /*reader*/, ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

IDTypeInfo IDType_ID_OV = {
    /*id_code*/ DynamicOverride::id_type,
    /*id_filter*/ FILTER_ID_OV,
    /*dependencies_id_types*/ FILTER_ID_ALL,
    /*main_listbase_index*/ INDEX_ID_OV,
    /*struct_size*/ sizeof(DynamicOverride),
    /*name*/ "Dynamic Override",
    /*name_plural*/ N_("dynamic overrides"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_DYNAMIC_OVERRIDE,
    /*flags*/ 0,
    /*asset_type_info*/ nullptr,

    /*init_data*/ dynoverride_init_data,
    /*copy_data*/ dynoverride_copy_data,
    /*free_data*/ dynoverride_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ dynoverride_blend_write,
    /*blend_read_data*/ dynoverride_blend_read_data,
    /*blend_read_after_liblink*/ nullptr,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ nullptr,
};

}  // namespace blender
