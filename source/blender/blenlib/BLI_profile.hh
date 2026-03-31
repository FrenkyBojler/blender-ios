/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A tiny wrapper around the TracyClient library profiling API which takes care of including the
 * Tracy header and exposing it via BLI_profile_* macros. When building without Tracy enabled
 * the macros are evaluated to no-op.
 */

#pragma once

#define BLI_profile_color_wm 0xA00030
#define BLI_profile_color_python 0x30A000
#define BLI_profile_color_draw 0x0030A0
#define BLI_profile_color_ghost 0xA06000

#ifdef WITH_TRACY_CLIENT
#  include <tracy/tracy/Tracy.hpp>

/* TODO: WIP incomplete API. See Tracy.hpp header for a complete list of implementable macros. */

#  if 0 /* Optionally, use full signature for function names. */
// TODO: See if it would be possible to trim the blender:: namespace and arguments
#    if defined(__clang__) || defined(__GNUC__)
#      undef TracyFunction
#      define TracyFunction __PRETTY_FUNCTION__
#    elif defined(_MSC_VER)
#      undef TracyFunction
#      define TracyFunction __FUNCSIG__
#    endif
#  endif

/* Frame markers. */
#  define BLI_profile_frame_mark FrameMark
#  define BLI_profile_frame_mark_start(name) FrameMarkStart(name)
#  define BLI_profile_frame_mark_end(name) FrameMarkEnd(name)

/* Scoped zones, create a profiling zone lasting until end of current scope. */
#  define BLI_profile_zone_scoped ZoneScoped
#  define BLI_profile_zone_scoped_n(name) ZoneScopedN(name)
#  define BLI_profile_zone_scoped_c(color) ZoneScopedC(color)
#  define BLI_profile_zone_scoped_nc(name, color) ZoneScopedNC(name, color)

/* Named zones, zones attached to a specific string, allowing multiple zones in a single scope. */
#  define BLI_profile_zone_named(zone) ZoneNamed(zone, true)
#  define BLI_profile_zone_named_n(zone, ui_name) ZoneNamedN(zone_name, ui_name, true)
#  define BLI_profile_zone_named_c(zone, color) ZoneNamedC(zone_name, color, true)
#  define BLI_profile_zone_named_nc(zone, ui_name, color) \
    ZoneNamedNC(zone_name, ui_name, color, true)

/* Set dynamic zone name, text, color, and value. */
#  define BLI_profile_zone_set_name(text, size) ZoneName(text, size)
#  define BLI_profile_zone_set_name_fmt(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_text(text, size) ZoneText(text, size)
#  define BLI_profile_zone_set_text_fmt(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_color(color) ZoneColor(color)
#  define BLI_profile_zone_set_value(value) ZoneValue(value)

/* Named zone variants, taking zone name as first argument. */
#  define BLI_profile_zone_set_name_z(zone, text, size) ZoneNameV(zone, text, size)
#  define BLI_profile_zone_set_name_fmt_z(zone, fmt, ...) ZoneNameVF(zone, fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_text_z(zone, text, size) ZoneTextV(zone, text, size)
#  define BLI_profile_zone_set_text_fmt_z(zone, fmt, ...) ZoneTextVF(zone, fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_color_z(zone, color) ZoneColorV(zone, color)
#  define BLI_profile_zone_set_value_z(zone, value) ZoneValueV(zone, value)

/* Memory allocation profiling. */
#  define BLI_profile_memory_alloc(ptr, size) TracyAlloc(ptr, size)
#  define BLI_profile_memory_free(ptr) TracyFree(ptr)

/* Set current thread name. */
#  define BLI_profile_set_thread_name(name) tracy::SetThreadName(name)
#  define BLI_profile_set_thread_name_with_hint(name, hint) \
    tracy::SetThreadNameWithHint(name, hint)

/* PyObject_Call* wrappers to profile Python code execution.
 * API exposes both Python function parsing and explicit object name variants. */
// TODO: Check if it would be possible to override the source location data to show the original
//       function name, and not the lambda operator().
#  define __bli_profile_python_base_zone_label(label) "Python " label " call"
#  define __bli_profile_python_unknown_label "<unknown python func>"

#  define __bli_profile_python_zone(profile_label) \
    BLI_profile_zone_scoped_nc(__bli_profile_python_base_zone_label(profile_label), \
                               BLI_profile_color_python);

#  define __bli_profile_python_parsefunc(py_callable) \
    if (PyFunction_Check(py_callable)) { \
      const PyCodeObject *code_obj = (PyCodeObject *)PyFunction_GetCode(py_callable); \
      const char *py_func_name = PyUnicode_AsUTF8AndSize(code_obj->co_qualname, nullptr); \
      BLI_profile_zone_set_name_fmt("Python Function: %s", py_func_name); \
    } \
    else { \
      BLI_profile_zone_set_name(__bli_profile_python_unknown_label, \
                                sizeof(__bli_profile_python_unknown_label)); \
    }

#  define __bli_profile_python_objectname(object_name) \
    BLI_profile_zone_set_name_fmt("Python Instance: %s", object_name);

#  define BLI_profile_PyObject_Call_parsefunc(profile_label, py_callable, py_args, py_kwargs) \
    [](PyObject *callable, PyObject *args, PyObject *kwargs) -> PyObject * { \
      __bli_profile_python_zone(profile_label); \
      __bli_profile_python_parsefunc(callable); \
      return PyObject_Call(callable, args, kwargs); \
    }(py_callable, py_args, py_kwargs)

#  define BLI_profile_PyObject_CallObject_parsefunc(profile_label, py_callable, py_args) \
    [](PyObject *callable, PyObject *args) -> PyObject * { \
      __bli_profile_python_zone(profile_label); \
      __bli_profile_python_parsefunc(callable); \
      return PyObject_CallObject(callable, args); \
    }(py_callable, py_args)

#  define BLI_profile_PyObject_CallOneArg_parsefunc(profile_label, py_callable, py_arg) \
    [](PyObject *callable, PyObject *arg) -> PyObject * { \
      __bli_profile_python_zone(profile_label); \
      __bli_profile_python_parsefunc(callable); \
      return PyObject_CallOneArg(callable, arg); \
    }(py_callable, py_arg)

#  define BLI_profile_PyObject_Call_objectname( \
      profile_label, object_name, py_callable, py_args, py_kwargs) \
    [&object_name](PyObject *callable, PyObject *args, PyObject *kwargs) -> PyObject * { \
      __bli_profile_python_zone(profile_label); \
      __bli_profile_python_objectname(object_name); \
      return PyObject_Call(callable, args, kwargs); \
    }(py_callable, py_args, py_kwargs)

#  define BLI_profile_PyObject_CallObject_objectname( \
      profile_label, object_name, py_callable, py_args) \
    [&object_name](PyObject *callable, PyObject *args) -> PyObject * { \
      __bli_profile_python_zone(profile_label); \
      __bli_profile_python_objectname(object_name); \
      return PyObject_CallObject(callable, args); \
    }(py_callable, py_args)

#  define BLI_profile_PyObject_CallOneArg_objectname( \
      profile_label, object_name, py_callable, py_arg) \
    [&object_name](PyObject *callable, PyObject *arg) -> PyObject * { \
      __bli_profile_python_zone(profile_label); \
      __bli_profile_python_objectname(object_name); \
      return PyObject_CallOneArg(callable, arg); \
    }(py_callable, py_arg)

#else
#  define BLI_profile_zone_scoped
#  define BLI_profile_zone_scoped_n(name)
#  define BLI_profile_zone_scoped_c(color)
#  define BLI_profile_zone_scoped_nc(name, color)

#  define BLI_profile_zone_named(zone_name)
#  define BLI_profile_zone_named_n(zone_name, ui_name)
#  define BLI_profile_zone_named_c(zone, color)
#  define BLI_profile_zone_named_nc(zone, ui_name, color)

#  define BLI_profile_zone_set_name(text, size)
#  define BLI_profile_zone_set_name_fmt(fmt, ...)
#  define BLI_profile_zone_set_text(text, size)
#  define BLI_profile_zone_set_text_fmt(fmt, ...)
#  define BLI_profile_zone_set_color(color)
#  define BLI_profile_zone_set_value(value)

#  define BLI_profile_zone_set_name_z(zone, text, size)
#  define BLI_profile_zone_set_name_fmt_z(zone, fmt, ...)
#  define BLI_profile_zone_set_text_z(zone, text, size)
#  define BLI_profile_zone_set_text_fmt_z(zone, fmt, ...)
#  define BLI_profile_zone_set_color_z(zone, color)
#  define BLI_profile_zone_set_value_z(zone, value)

#  define BLI_profile_frame_mark
#  define BLI_profile_frame_mark_start(name)
#  define BLI_profile_frame_mark_end(name)

#  define BLI_profile_memory_alloc(ptr, size)
#  define BLI_profile_memory_alloc_n(ptr, size, name)
#  define BLI_profile_memory_free(ptr, size)
#  define BLI_profile_memory_free_n(ptr, size, name)

#  define BLI_profile_set_thread_name(name)
#  define BLI_profile_set_thread_name_with_hint(name, hint)
#endif
