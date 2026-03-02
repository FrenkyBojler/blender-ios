/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstdio>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"

#include "BLT_translation.hh"

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_shader_fx_types.h"

#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_screen.hh"
#include "BKE_shader_fx.hh"

#include "FX_shader_types.hh"

#include "BLO_read_write.hh"

namespace blender {

static ShaderFxTypeInfo *shader_fx_types[NUM_SHADER_FX_TYPES] = {nullptr};

/* *************************************************** */
/* Methods - Evaluation Loops, etc. */

bool BKE_shaderfx_has_gpencil(const Object *ob)
{
  for (const ShaderFxData &fx : ob->shader_fx) {
    const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx.type));
    if (fxi->type == eShaderFxType_GpencilType) {
      return true;
    }
  }
  return false;
}

void BKE_shaderfx_init()
{
  /* Initialize shaders */
  shaderfx_type_init(shader_fx_types); /* `FX_shader_util.cc`. */
}

ShaderFxData *BKE_shaderfx_new(int type)
{
  const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(type));
  ShaderFxData *fx = fxi->new_data();

  /* NOTE: this name must be made unique later. */
  STRNCPY_UTF8(fx->name, DATA_(fxi->name));

  fx->type = type;
  fx->mode = eShaderFxMode_Realtime | eShaderFxMode_Render;
  fx->flag = eShaderFxFlag_OverrideLibrary_Local;
  /* Expand only the parent panel by default. */
  fx->ui_expand_flag |= UI_PANEL_DATA_EXPAND_ROOT;

  if (fxi->flags & eShaderFxTypeFlag_EnableInEditmode) {
    fx->mode |= eShaderFxMode_Editmode;
  }

  return fx;
}

static void shaderfx_free_data_id_us_cb(void * /*user_data*/,
                                        Object * /*ob*/,
                                        ID **idpoin,
                                        const LibraryForeachIDCallbackFlag cb_flag)
{
  ID *id = *idpoin;
  if (id != nullptr && (cb_flag & IDWALK_CB_USER) != 0) {
    id_us_min(id);
  }
}

void BKE_shaderfx_free_ex(ShaderFxData *fx, const int flag)
{
  const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx->type));

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    if (fxi->foreach_ID_link) {
      fxi->foreach_ID_link(fx, nullptr, shaderfx_free_data_id_us_cb, nullptr);
    }
  }

  fxi->free_data(fx);

  if (fx->error) {
    MEM_delete(fx->error);
  }

  MEM_delete(fx);
}

void BKE_shaderfx_free(ShaderFxData *fx)
{
  BKE_shaderfx_free_ex(fx, 0);
}

void BKE_shaderfx_unique_name(ListBaseT<ShaderFxData> *shaders, ShaderFxData *fx)
{
  if (shaders && fx) {
    const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx->type));
    BLI_uniquename(
        shaders, fx, DATA_(fxi->name), '.', offsetof(ShaderFxData, name), sizeof(fx->name));
  }
}

bool BKE_shaderfx_depends_ontime(ShaderFxData *fx)
{
  const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx->type));

  return fxi->depends_on_time && fxi->depends_on_time(fx);
}

const ShaderFxTypeInfo *BKE_shaderfx_get_info(ShaderFxType type)
{
  /* type unsigned, no need to check < 0 */
  if (type < NUM_SHADER_FX_TYPES && type > 0 && shader_fx_types[type]->name[0] != '\0') {
    return shader_fx_types[type];
  }

  return nullptr;
}

bool BKE_shaderfx_is_nonlocal_in_liboverride(const Object *ob, const ShaderFxData *shaderfx)
{
  return (ID_IS_OVERRIDE_LIBRARY(ob) &&
          ((shaderfx == nullptr) || (shaderfx->flag & eShaderFxFlag_OverrideLibrary_Local) == 0));
}

void BKE_shaderfxType_panel_id(ShaderFxType type, char *r_idname)
{
  const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(type);
  BLI_string_join(r_idname, BKE_ST_MAXNAME, SHADERFX_TYPE_PANEL_PREFIX, fxi->name);
}

void BKE_shaderfx_panel_expand(ShaderFxData *fx)
{
  fx->ui_expand_flag |= UI_PANEL_DATA_EXPAND_ROOT;
}

static void shaderfx_copy_data_id_us_cb(void * /*user_data*/,
                                        Object * /*ob*/,
                                        ID **idpoin,
                                        const LibraryForeachIDCallbackFlag cb_flag)
{
  ID *id = *idpoin;
  if (id != nullptr && (cb_flag & IDWALK_CB_USER) != 0) {
    id_us_plus(id);
  }
}

void BKE_shaderfx_copydata_ex(ShaderFxData *fx, ShaderFxData *target, const int flag)
{
  const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx->type));

  /* Preserve next and prev pointers. */
  ShaderFxData *next = target->next;
  ShaderFxData *prev = target->prev;

  fxi->copy_data(fx, target);

  target->next = next;
  target->prev = prev;

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    if (fxi->foreach_ID_link) {
      fxi->foreach_ID_link(target, nullptr, shaderfx_copy_data_id_us_cb, nullptr);
    }
  }
}

void BKE_shaderfx_copydata(ShaderFxData *fx, ShaderFxData *target)
{
  BKE_shaderfx_copydata_ex(fx, target, 0);
}

void BKE_shaderfx_copy(ListBaseT<ShaderFxData> *dst, const ListBaseT<ShaderFxData> *src)
{
  ShaderFxData *fx;
  ShaderFxData *srcfx;

  BLI_listbase_clear(dst);
  BLI_duplicatelist(dst, src);

  for (srcfx = static_cast<ShaderFxData *>(src->first),
      fx = static_cast<ShaderFxData *>(dst->first);
       srcfx && fx;
       srcfx = srcfx->next, fx = fx->next)
  {
    BKE_shaderfx_copydata(srcfx, fx);
  }
}

ShaderFxData *BKE_shaderfx_findby_type(Object *ob, ShaderFxType type)
{
  ShaderFxData *fx = static_cast<ShaderFxData *>(ob->shader_fx.first);

  for (; fx; fx = fx->next) {
    if (fx->type == type) {
      break;
    }
  }

  return fx;
}

void BKE_shaderfx_foreach_ID_link(Object *ob, ShaderFxIDWalkFunc walk, void *user_data)
{
  ShaderFxData *fx = static_cast<ShaderFxData *>(ob->shader_fx.first);

  for (; fx; fx = fx->next) {
    const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx->type));

    if (fxi->foreach_ID_link) {
      fxi->foreach_ID_link(fx, ob, walk, user_data);
    }
  }
}

ShaderFxData *BKE_shaderfx_findby_name(Object *ob, const char *name)
{
  return static_cast<ShaderFxData *>(
      BLI_findstring(&(ob->shader_fx), name, offsetof(ShaderFxData, name)));
}

void BKE_shaderfx_blend_write(BlendWriter *writer, ListBaseT<ShaderFxData> *fxbase)
{
  if (fxbase == nullptr) {
    return;
  }

  for (ShaderFxData &fx : *fxbase) {
    const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(fx.type));
    if (fxi == nullptr) {
      return;
    }

    writer->write_struct_by_name(fxi->struct_name, &fx);
  }
}

void BKE_shaderfx_blend_read_data(BlendDataReader *reader, ListBaseT<ShaderFxData> *lb, Object *ob)
{
  BLO_read_struct_list(reader, ShaderFxData, lb);

  for (ShaderFxData &fx : *lb) {
    fx.error = nullptr;

    /* If linking from a library, clear 'local' library override flag. */
    if (ID_IS_LINKED(ob)) {
      fx.flag &= ~eShaderFxFlag_OverrideLibrary_Local;
    }

    /* if shader disappear, or for upward compatibility */
    if (nullptr == BKE_shaderfx_get_info(ShaderFxType(fx.type))) {
      fx.type = eShaderFxType_None;
    }
  }
}

}  // namespace blender
