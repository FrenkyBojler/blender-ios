/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file extends RNA types from `bpy.types` with C/Python API methods and attributes.
 *
 * We should avoid adding code here, and prefer:
 * - `source/blender/makesrna/intern/rna_context.cc` using the RNA C API.
 * - `scripts/modules/_bpy_types.py` when additions c an be written in Python.
 *
 * Otherwise functions can be added here as a last resort.
 */

#include <Python.h>
#include <descrobject.h>

#include "BLI_utildefines.h"

#include "BKE_volume_grid.hh"

#include "bpy_library.hh"
#include "bpy_rna.hh"
#include "bpy_rna_callback.hh"
#include "bpy_rna_context.hh"
#include "bpy_rna_data.hh"
#include "bpy_rna_id_collection.hh"
#include "bpy_rna_text.hh"
#include "bpy_rna_types_capi.hh"
#include "bpy_rna_ui.hh"

#include "bpy_rna_operator.hh"

#include "../generic/py_capi_utils.hh"

#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"

/* -------------------------------------------------------------------- */
/** \name Blend Data
 * \{ */

static PyMethodDef pyrna_blenddata_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_user_map_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_file_path_map_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_file_path_foreach_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_batch_remove_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_id_collection_orphans_purge_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_data_context_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Data Libraries
 * \{ */

static PyMethodDef pyrna_blenddatalibraries_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_library_load_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_library_write_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Layout
 * \{ */

static PyMethodDef pyrna_uilayout_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_uilayout_introspect_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static PyMethodDef pyrna_operator_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_operator_poll_message_set */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Editor
 * \{ */

static PyMethodDef pyrna_text_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_region_as_string_method_def */
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_region_from_string_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Manager Clipboard Property
 *
 * Avoid using the RNA API because this value may change between checking its length
 * and creating the buffer, causing writes past the allocated length.
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_WindowManager_clipboard_doc,
    "Clipboard text storage.\n"
    "\n"
    ":type: str\n");
static PyObject *pyrna_WindowManager_clipboard_get(PyObject * /*self*/, void * /*flag*/)
{
  int text_len = 0;
  /* No need for UTF8 validation as #PyC_UnicodeFromBytesAndSize handles invalid byte sequences. */
  char *text = WM_clipboard_text_get(false, false, &text_len);
  PyObject *result = PyC_UnicodeFromBytesAndSize(text ? text : "", text_len);
  if (text != nullptr) {
    MEM_freeN(text);
  }
  return result;
}

static int pyrna_WindowManager_clipboard_set(PyObject * /*self*/, PyObject *value, void * /*flag*/)
{
  PyObject *value_coerce = nullptr;
  const char *text = PyC_UnicodeAsBytes(value, &value_coerce);
  if (text == nullptr) {
    return -1;
  }
  WM_clipboard_text_set(text, false);
  Py_XDECREF(value_coerce);
  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window Manager Type
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_draw_cursor_add_doc,
    ".. classmethod:: draw_cursor_add(callback, args, space_type, region_type)\n"
    "\n"
    "   Add a new draw cursor handler to this space type.\n"
    "   It will be called every time the cursor for the specified region in the space "
    "type will be drawn.\n"
    "   Note: All arguments are positional only for now.\n"
    "\n"
    "   :arg callback:\n"
    "      A function that will be called when the cursor is drawn.\n"
    "      It gets the specified arguments as input with the mouse position "
    "(``tuple[int, int]``) as last argument.\n"
    "   :type callback: Callable[..., Any]\n"
    "   :arg args: Arguments that will be passed to the callback.\n"
    "   :type args: tuple[Any, ...]\n"
    "   :arg space_type: The space type the callback draws in; for example ``VIEW_3D``. "
    "(:class:`bpy.types.Space.type`)\n"
    "   :type space_type: str\n"
    "   :arg region_type: The region type the callback draws in; usually ``WINDOW``. "
    "(:class:`bpy.types.Region.type`)\n"
    "   :type region_type: str\n"
    "   :return: Handler that can be removed later on.\n"
    "   :rtype: object\n");
PyDoc_STRVAR(
    /* Wrap. */
    pyrna_draw_cursor_remove_doc,
    ".. classmethod:: draw_cursor_remove(handler)\n"
    "\n"
    "   Remove a draw cursor handler that was added previously.\n"
    "\n"
    "   :arg handler: The draw cursor handler that should be removed.\n"
    "   :type handler: object\n");

static PyMethodDef pyrna_windowmanager_methods[] = {
    {"draw_cursor_add",
     (PyCFunction)pyrna_callback_classmethod_add,
     METH_VARARGS | METH_CLASS,
     pyrna_draw_cursor_add_doc},
    {"draw_cursor_remove",
     (PyCFunction)pyrna_callback_classmethod_remove,
     METH_VARARGS | METH_CLASS,
     pyrna_draw_cursor_remove_doc},
    {nullptr, nullptr, 0, nullptr},
};

static PyGetSetDef pyrna_windowmanager_getset[] = {
    {"clipboard",
     pyrna_WindowManager_clipboard_get,
     pyrna_WindowManager_clipboard_set,
     pyrna_WindowManager_clipboard_doc,
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

/** \} */

#ifdef WITH_OPENVDB

/* -------------------------------------------------------------------- */
/** \name Volume Grid Type
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_VolumeGrid_openvdb_grid_pointer_doc,
    "OpenVDB grid wrapper for external C++ processing with write access.\n"
    "This creates a copy if the grid is shared. Use openvdb_grid_pointer_readonly for exporters.\n"
    "\n"
    "The wrapper holds both the grid shared_ptr and access token, ensuring the grid\n"
    "remains valid for the lifetime of the wrapper object. Access the raw pointer\n"
    "via the 'pointer' attribute.\n"
    "\n"
    ":type: VolumeGridWrapper\n");

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_VolumeGrid_openvdb_grid_pointer_readonly_doc,
    "OpenVDB grid wrapper for external C++ processing with read-only access.\n"
    "This is more efficient for exporters as it doesn't create unnecessary copies.\n"
    "\n"
    "The wrapper holds both the grid shared_ptr and access token, ensuring the grid\n"
    "remains valid for the lifetime of the wrapper object. Access the raw pointer\n"
    "via the 'pointer' attribute.\n"
    "\n"
    ":type: VolumeGridWrapper\n");

/* -------------------------------------------------------------------- */
/** \name Volume Grid Wrapper Type
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    BPy_VolumeGridWrapper_doc,
    "OpenVDB Grid Wrapper with Access Token\n"
    "\n"
    "A wrapper object that holds an OpenVDB grid pointer along with its access token,\n"
    "ensuring the grid remains valid for the lifetime of this wrapper object.\n"
    "\n"
    "This prevents use-after-free crashes that could occur if the grid was unloaded\n"
    "while external code still held a raw pointer to it.\n"
    "\n"
    ".. attribute:: pointer\n"
    "\n"
    "   Raw OpenVDB grid pointer for use with external C++ libraries.\n"
    "\n"
    "   :type: int\n");

struct BPy_VolumeGridWrapper {
  PyObject_HEAD
  std::shared_ptr<openvdb::GridBase> grid;
  blender::bke::VolumeTreeAccessToken token;
  // TODO this currently does nothing. remove
  bool is_readonly;
};

static void BPy_VolumeGridWrapper_dealloc(BPy_VolumeGridWrapper *self)
{
  using blender::bke::VolumeTreeAccessToken;
  using std::shared_ptr;

  self->grid.~shared_ptr();
  self->token.~VolumeTreeAccessToken();
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *BPy_VolumeGridWrapper_get_pointer(BPy_VolumeGridWrapper *self, void * /*flag*/)
{
  return PyLong_FromVoidPtr(self->grid.get());
}

static PyGetSetDef BPy_VolumeGridWrapper_getset[] = {
    {"pointer",
     (getter)BPy_VolumeGridWrapper_get_pointer,
     nullptr,
     "OpenVDB grid pointer",
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

static PyTypeObject BPy_VolumeGridWrapper_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    /*tp_name*/ "VolumeGridWrapper",
    /*tp_basicsize*/ sizeof(BPy_VolumeGridWrapper),
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ (destructor)BPy_VolumeGridWrapper_dealloc,
    /*tp_vectorcall_offset*/ 0,
    /*tp_getattr*/ nullptr,
    /*tp_setattr*/ nullptr,
    /*tp_as_async*/ nullptr,
    /*tp_repr*/ nullptr,
    /*tp_as_number*/ nullptr,
    /*tp_as_sequence*/ nullptr,
    /*tp_as_mapping*/ nullptr,
    /*tp_hash*/ nullptr,
    /*tp_call*/ nullptr,
    /*tp_str*/ nullptr,
    /*tp_getattro*/ PyObject_GenericGetAttr,
    /*tp_setattro*/ PyObject_GenericSetAttr,
    /*tp_as_buffer*/ nullptr,
    /*tp_flags*/ Py_TPFLAGS_DEFAULT,
    /*tp_doc*/ BPy_VolumeGridWrapper_doc,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ nullptr,
    /*tp_members*/ nullptr,
    /*tp_getset*/ BPy_VolumeGridWrapper_getset,
    /*tp_base*/ nullptr,
    /*tp_dict*/ nullptr,
    /*tp_descr_get*/ nullptr,
    /*tp_descr_set*/ nullptr,
    /*tp_dictoffset*/ 0,
    /*tp_init*/ nullptr,
    /*tp_alloc*/ nullptr,
    /*tp_new*/ nullptr,
};

static PyObject *BPy_VolumeGridWrapper_create(std::shared_ptr<openvdb::GridBase> grid,
                                              blender::bke::VolumeTreeAccessToken &&token,
                                              bool is_readonly)
{
  BPy_VolumeGridWrapper *self = (BPy_VolumeGridWrapper *)PyObject_New(BPy_VolumeGridWrapper,
                                                                      &BPy_VolumeGridWrapper_Type);
  if (self == nullptr) {
    return nullptr;
  }

  new (&self->grid) std::shared_ptr(std::move(grid));
  new (&self->token) blender::bke::VolumeTreeAccessToken(std::move(token));
  self->is_readonly = is_readonly;

  return (PyObject *)self;
}

/** \} */

static PyObject *pyrna_VolumeGrid_openvdb_grid_pointer_get(PyObject *self, void * /*flag*/)
{
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  auto *grid = static_cast<blender::bke::VolumeGridData *>(pyrna->ptr->data);

  blender::bke::VolumeTreeAccessToken token;
  auto grid_ptr = grid->grid_ptr_for_write(token);
  return BPy_VolumeGridWrapper_create(grid_ptr, std::move(token), false);
}

static PyObject *pyrna_VolumeGrid_openvdb_grid_pointer_readonly_get(PyObject *self,
                                                                    void * /*flag*/)
{
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  const auto *grid = static_cast<const blender::bke::VolumeGridData *>(pyrna->ptr->data);

  blender::bke::VolumeTreeAccessToken token;
  auto grid_ptr = grid->grid_ptr(token);
  return BPy_VolumeGridWrapper_create(
      std::const_pointer_cast<openvdb::GridBase>(grid_ptr), std::move(token), true);
}

static PyGetSetDef pyrna_volumegrid_getset[] = {
    {"openvdb_grid_pointer",
     pyrna_VolumeGrid_openvdb_grid_pointer_get,
     nullptr,
     pyrna_VolumeGrid_openvdb_grid_pointer_doc,
     nullptr},
    {"openvdb_grid_pointer_readonly",
     pyrna_VolumeGrid_openvdb_grid_pointer_readonly_get,
     nullptr,
     pyrna_VolumeGrid_openvdb_grid_pointer_readonly_doc,
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context Type
 * \{ */

static PyMethodDef pyrna_context_methods[] = {
    {nullptr, nullptr, 0, nullptr}, /* #BPY_rna_context_temp_override_method_def */
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Space Type
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    pyrna_draw_handler_add_doc,
    ".. classmethod:: draw_handler_add(callback, args, region_type, draw_type)\n"
    "\n"
    "   Add a new draw handler to this space type.\n"
    "   It will be called every time the specified region in the space type will be drawn.\n"
    "   Note: All arguments are positional only for now.\n"
    "\n"
    "   :arg callback:\n"
    "      A function that will be called when the region is drawn.\n"
    "      It gets the specified arguments as input, it's return value is ignored.\n"
    "   :type callback: Callable[..., Any]\n"
    "   :arg args: Arguments that will be passed to the callback.\n"
    "   :type args: tuple[Any, ...]\n"
    "   :arg region_type: The region type the callback draws in; usually ``WINDOW``. "
    "(:class:`bpy.types.Region.type`)\n"
    "   :type region_type: str\n"
    "   :arg draw_type: Usually ``POST_PIXEL`` for 2D drawing and ``POST_VIEW`` for 3D drawing. "
    "In some cases ``PRE_VIEW`` can be used. ``BACKDROP`` can be used for backdrops in the node "
    "editor.\n"
    "   :type draw_type: str\n"
    "   :return: Handler that can be removed later on.\n"
    "   :rtype: object\n");
PyDoc_STRVAR(
    /* Wrap. */
    pyrna_draw_handler_remove_doc,
    ".. classmethod:: draw_handler_remove(handler, region_type)\n"
    "\n"
    "   Remove a draw handler that was added previously.\n"
    "\n"
    "   :arg handler: The draw handler that should be removed.\n"
    "   :type handler: object\n"
    "   :arg region_type: Region type the callback was added to.\n"
    "   :type region_type: str\n");

static PyMethodDef pyrna_space_methods[] = {
    {"draw_handler_add",
     (PyCFunction)pyrna_callback_classmethod_add,
     METH_VARARGS | METH_CLASS,
     pyrna_draw_handler_add_doc},
    {"draw_handler_remove",
     (PyCFunction)pyrna_callback_classmethod_remove,
     METH_VARARGS | METH_CLASS,
     pyrna_draw_handler_remove_doc},
    {nullptr, nullptr, 0, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

void BPY_rna_types_extend_capi()
{
  /* BlendData */
  ARRAY_SET_ITEMS(pyrna_blenddata_methods,
                  BPY_rna_id_collection_user_map_method_def,
                  BPY_rna_id_collection_file_path_map_method_def,
                  BPY_rna_id_collection_file_path_foreach_method_def,
                  BPY_rna_id_collection_batch_remove_method_def,
                  BPY_rna_id_collection_orphans_purge_method_def,
                  BPY_rna_data_context_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_blenddata_methods) == 7, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(&RNA_BlendData, pyrna_blenddata_methods, nullptr);

  /* BlendDataLibraries */
  ARRAY_SET_ITEMS(
      pyrna_blenddatalibraries_methods, BPY_library_load_method_def, BPY_library_write_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_blenddatalibraries_methods) == 3,
                    "Unexpected number of methods")
  pyrna_struct_type_extend_capi(
      &RNA_BlendDataLibraries, pyrna_blenddatalibraries_methods, nullptr);

  /* uiLayout */
  ARRAY_SET_ITEMS(pyrna_uilayout_methods, BPY_rna_uilayout_introspect_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_uilayout_methods) == 2, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(&RNA_UILayout, pyrna_uilayout_methods, nullptr);

  /* Space */
  pyrna_struct_type_extend_capi(&RNA_Space, pyrna_space_methods, nullptr);

  /* Text Editor */
  ARRAY_SET_ITEMS(pyrna_text_methods,
                  BPY_rna_region_as_string_method_def,
                  BPY_rna_region_from_string_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_text_methods) == 3, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(&RNA_Text, pyrna_text_methods, nullptr);

  /* wmOperator */
  ARRAY_SET_ITEMS(pyrna_operator_methods, BPY_rna_operator_poll_message_set_method_def);
  BLI_STATIC_ASSERT(ARRAY_SIZE(pyrna_operator_methods) == 2, "Unexpected number of methods")
  pyrna_struct_type_extend_capi(&RNA_Operator, pyrna_operator_methods, nullptr);

  /* WindowManager */
  pyrna_struct_type_extend_capi(
      &RNA_WindowManager, pyrna_windowmanager_methods, pyrna_windowmanager_getset);

  /* Context */
  bpy_rna_context_types_init();

  ARRAY_SET_ITEMS(pyrna_context_methods, BPY_rna_context_temp_override_method_def);
  pyrna_struct_type_extend_capi(&RNA_Context, pyrna_context_methods, nullptr);

#ifdef WITH_OPENVDB
  /* Initialize VolumeGridWrapper type */
  if (PyType_Ready(&BPy_VolumeGridWrapper_Type) < 0) {
    return;
  }

  /* VolumeGrid */
  pyrna_struct_type_extend_capi(&RNA_VolumeGrid, nullptr, pyrna_volumegrid_getset);
#endif
}

/** \} */
