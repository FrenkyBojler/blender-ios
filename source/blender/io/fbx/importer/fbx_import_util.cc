/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include "BKE_idprop.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "BLI_string.h"

#include "DNA_object_types.h"

#include "fbx_import_util.hh"

namespace blender::io::fbx {

const char *get_fbx_name(const ufbx_string &name, const char *def)
{
  return name.length > 0 ? name.data : def;
}

void matrix_to_m44(const ufbx_matrix &src, float dst[4][4])
{
  dst[0][0] = src.m00;
  dst[1][0] = src.m01;
  dst[2][0] = src.m02;
  dst[3][0] = src.m03;
  dst[0][1] = src.m10;
  dst[1][1] = src.m11;
  dst[2][1] = src.m12;
  dst[3][1] = src.m13;
  dst[0][2] = src.m20;
  dst[1][2] = src.m21;
  dst[2][2] = src.m22;
  dst[3][2] = src.m23;
  dst[0][3] = 0.0f;
  dst[1][3] = 0.0f;
  dst[2][3] = 0.0f;
  dst[3][3] = 1.0f;
}

void m44_to_matrix(const float src[4][4], ufbx_matrix &dst)
{
  dst.m00 = src[0][0];
  dst.m01 = src[1][0];
  dst.m02 = src[2][0];
  dst.m03 = src[3][0];
  dst.m10 = src[0][1];
  dst.m11 = src[1][1];
  dst.m12 = src[2][1];
  dst.m13 = src[3][1];
  dst.m20 = src[0][2];
  dst.m21 = src[1][2];
  dst.m22 = src[2][2];
  dst.m23 = src[3][2];
}

void ufbx_matrix_to_obj(const ufbx_matrix &mtx, Object *obj)
{
  float obmat[4][4];
  matrix_to_m44(mtx, obmat);
  BKE_object_apply_mat4(obj, obmat, true, false);
  BKE_object_to_mat4(obj, obj->runtime->object_to_world.ptr());
}

void node_matrix_to_obj(const ufbx_node *node, Object *obj)
{
  ufbx_matrix mtx = ufbx_matrix_mul(node->is_root ? &node->node_to_world : &node->node_to_parent,
                                    &node->geometry_to_node);
  ufbx_matrix_to_obj(mtx, obj);
}

void read_custom_properties(const ufbx_props &props, ID &id)
{
  for (const ufbx_prop &prop : props.props) {
    if ((prop.flags & UFBX_PROP_FLAG_USER_DEFINED) == 0) {
      continue;
    }

    IDProperty *idgroup = IDP_EnsureProperties(&id);
    IDProperty *idprop = nullptr;
    IDPropertyTemplate val = {0};
    //@TODO: validate_blend_names on the property name
    const char *name = prop.name.data;

    switch (prop.type) {
      case UFBX_PROP_BOOLEAN:
        val.i = prop.value_int;
        idprop = IDP_New(IDP_BOOLEAN, &val, name);
        break;
      case UFBX_PROP_INTEGER:
        val.i = prop.value_int;
        idprop = IDP_New(IDP_INT, &val, name);
        break;
      case UFBX_PROP_NUMBER:
        val.d = prop.value_real;
        idprop = IDP_New(IDP_DOUBLE, &val, name);
        break;
      case UFBX_PROP_STRING:
        if (STREQ(name, "UDP3DSMAX")) {
          /* 3dsmax user properties are coming with UDP3DSMAX name, and a multi-line
           * string split by '=' character. */
          const char *line = prop.value_str.data;
          while (true) {
            const char *line_start = line;
            line = BLI_strchr_or_end(line_start, '\n');
            if (line == line_start) {
              break;
            }

            /* We have a line, split it by '=' and trim name/value. */
            const char *eq_pos = line_start;
            while (eq_pos != line && eq_pos[0] != '=') {
              eq_pos++;
            }
            if (eq_pos[0] == '=') {
              std::string str_name = StringRef(line_start, eq_pos).trim();
              std::string str_val = StringRef(eq_pos + 1, line).trim();
              //@TODO validate_blend_names on str_name
              if (!str_name.empty() && !str_val.empty()) {
                val.string.str = str_val.c_str();
                val.string.len = str_val.size() + 1; /* .len needs to include null terminator. */
                val.string.subtype = IDP_STRING_SUB_UTF8;
                IDProperty *str_prop = IDP_New(IDP_STRING, &val, str_name.c_str());
                IDP_AddToGroup(idgroup, str_prop);
              }
            }

            if (line[0] == 0) {
              break;
            }
          }
        }
        else {
          val.string.str = prop.value_str.data;
          val.string.len = prop.value_str.length + 1; /* .len needs to include null terminator. */
          val.string.subtype = IDP_STRING_SUB_UTF8;
          idprop = IDP_New(IDP_STRING, &val, name);
        }
        break;
      case UFBX_PROP_VECTOR:
      case UFBX_PROP_COLOR:
        val.array.len = 3;
        val.array.type = IDP_DOUBLE;
        idprop = IDP_New(IDP_ARRAY, &val, name);
        {
          double *dst = static_cast<double *>(idprop->data.pointer);
          dst[0] = prop.value_vec3.x;
          dst[1] = prop.value_vec3.y;
          dst[2] = prop.value_vec3.z;
        }
        break;
      case UFBX_PROP_COLOR_WITH_ALPHA:
        val.array.len = 4;
        val.array.type = IDP_DOUBLE;
        idprop = IDP_New(IDP_ARRAY, &val, name);
        {
          double *dst = static_cast<double *>(idprop->data.pointer);
          dst[0] = prop.value_vec4.x;
          dst[1] = prop.value_vec4.y;
          dst[2] = prop.value_vec4.z;
          dst[3] = prop.value_vec4.z;
        }
        break;
      default:
        break;
    }

    if (idprop != nullptr) {
      IDP_AddToGroup(idgroup, idprop);
    }
  }
}

}  // namespace blender::io::fbx
