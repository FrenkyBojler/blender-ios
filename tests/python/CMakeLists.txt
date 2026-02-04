# SPDX-FileCopyrightText: 2011-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Use '--write-blend=/tmp/test.blend' to view output

set(TEST_SRC_DIR ${CMAKE_SOURCE_DIR}/tests/files)
set(TEST_PYTHON_DIR ${CMAKE_SOURCE_DIR}/tests/python)
set(TEST_OUT_DIR ${CMAKE_BINARY_DIR}/tests)

# ugh, any better way to do this on testing only?
file(MAKE_DIRECTORY ${TEST_OUT_DIR})
file(MAKE_DIRECTORY ${TEST_OUT_DIR}/io_tests)
file(MAKE_DIRECTORY ${TEST_OUT_DIR}/blendfile_io)

# Check which tests can be enabled.
if(IS_DIRECTORY ${TEST_SRC_DIR}/render)
  set(TEST_SRC_DIR_EXISTS TRUE)

  if(OPENIMAGEIO_TOOL)
    set(TEST_OPENIMAGEIO_TOOL_EXISTS TRUE)
  else()
    set(TEST_OPENIMAGEIO_TOOL_EXISTS FALSE)
    message(STATUS "Tests: Disabling render tests, missing oiiotool")
  endif()
else()
  set(TEST_SRC_DIR_EXISTS FALSE)
  message(STATUS "Tests: Disabling most tests, no test data in ${TEST_SRC_DIR}")
endif()

if(WITH_SYSTEM_PYTHON_TESTS)
  if(NOT EXISTS "${TEST_SYSTEM_PYTHON_EXE}")
    message(STATUS
      "Tests: Disabling 'System Python', no valid system python path, set TEST_SYSTEM_PYTHON_EXE."
    )
    set(WITH_SYSTEM_PYTHON_TESTS OFF)
  endif()
endif()

# Run Blender command with parameters.
function(add_blender_test_impl testname envvars_list exe)
  add_test(
    NAME ${testname}
    COMMAND ${exe} ${ARGN}
  )
  blender_test_set_envvars("${testname}" "${envvars_list}")
endfunction()

function(add_blender_test testname)
  add_blender_test_impl(
    "${testname}"
    ""
    "${TEST_BLENDER_EXE}"
    ${TEST_BLENDER_EXE_PARAMS}
    "${TEST_BLENDER_EXE_PARAMS_NO_THUMB}"
    ${ARGN}
  )
endfunction()

function(add_blender_test_allow_error testname)
  # Remove `--debug-exit-on-error` since errors are printed on e.g. meshes with
  # invalid data or failed image loading, but sometimes we want to test those.
  set(EXE_PARAMS ${TEST_BLENDER_EXE_PARAMS})
  list(REMOVE_ITEM EXE_PARAMS --debug-exit-on-error)
  add_blender_test_impl(
    "${testname}"
    ""
    "${TEST_BLENDER_EXE}"
    ${EXE_PARAMS}
    "${TEST_BLENDER_EXE_PARAMS_NO_THUMB}"
    ${ARGN}
  )
endfunction()

if(WITH_UI_TESTS)
  set(_blender_headless_env_vars "BLENDER_BIN=${TEST_BLENDER_EXE}")
  if(WITH_UI_TESTS_HEADLESS)

    # Currently only WAYLAND is supported, support for others may be added later.
    # In this case none of the WESTON environment variables will be used.
    if(WITH_GHOST_WAYLAND)
      set(_weston_bin_in_libdir OFF)
      if(DEFINED LIBDIR)
        set(_weston_bin_default "${LIBDIR}/wayland_weston/bin/weston")
      else()
        set(_weston_bin_default "weston")
      endif()
      set(WESTON_BIN "${_weston_bin_default}" CACHE STRING "\
  The location of weston, leave blank for the default location."
      )
      mark_as_advanced(WESTON_BIN)
      if((DEFINED LIBDIR) AND ("${WESTON_BIN}" STREQUAL "${_weston_bin_default}"))
        set(_weston_bin_in_libdir ON)
      endif()

      list(APPEND _blender_headless_env_vars
        "WESTON_BIN=${WESTON_BIN}"
      )

      if(_weston_bin_in_libdir)
        list(APPEND _blender_headless_env_vars
          "WAYLAND_ROOT_DIR=${LIBDIR}/wayland"
          "WESTON_ROOT_DIR=${LIBDIR}/wayland_weston"
        )
      endif()
    endif()
  else()
    list(APPEND _blender_headless_env_vars
      "PASS_THROUGH=1"
    )
  endif()
  function(add_blender_test_ui_backend testname backend)
    # Remove `--background`
    set(EXE_PARAMS ${TEST_BLENDER_EXE_PARAMS})
    list(REMOVE_ITEM EXE_PARAMS --background)
    add_blender_test_impl(
      "${testname}"
      "${_blender_headless_env_vars}"
      "${TEST_PYTHON_EXE}"
      "${CMAKE_SOURCE_DIR}/tests/utils/blender_headless.py"
      # NOTE: attempting to maximize the window causes problems with a headless `weston`,
      # while this could be investigated, use windowed mode instead.
      # Use a window size that balances software GPU rendering with enough room to use the UI.
      --factory-startup
      # No need for window frame with tests.
      --no-window-frame
      -p 0 0 800 600
      --gpu-backend ${backend}
      "${EXE_PARAMS}"
      "${TEST_BLENDER_EXE_PARAMS_NO_THUMB}"
      "${ARGN}"
    )
  endfunction()
  function(add_blender_test_ui testname)
    if(WITH_OPENGL_BACKEND)
      add_blender_test_ui_backend(
        "ui_opengl_${testname}"
        "opengl"
        "${ARGN}"
      )
    endif()
    if(WITH_METAL_BACKEND)
      add_blender_test_ui_backend(
        "ui_metal_${testname}"
        "metal"
        "${ARGN}"
      )
    endif()
    if(WITH_VULKAN_BACKEND AND WITH_GPU_RENDER_TESTS_VULKAN AND WITH_UI_TESTS_VULKAN)
      add_blender_test_ui_backend(
        "ui_vulkan_${testname}"
        "vulkan"
        "${ARGN}"
      )
    endif()
  endfunction()
endif()

# Run Python script outside Blender.
function(add_python_test testname testscript)
  if(NOT TEST_PYTHON_EXE)
    message(FATAL_ERROR "No Python configured for running tests, set TEST_PYTHON_EXE.")
  endif()

  add_test(
    NAME ${testname}
    COMMAND ${TEST_PYTHON_EXE} ${TEST_PYTHON_EXE_EXTRA_ARGS} ${testscript} ${ARGN}
    WORKING_DIRECTORY $<TARGET_FILE_DIR:blender>
  )
  blender_test_set_envvars("${testname}" "")
endfunction()

# Run Python render test.
function(add_render_test testname testscript)
  if(TEST_OPENIMAGEIO_TOOL_EXISTS)
    set(_args ${ARGN} --blender "${TEST_BLENDER_EXE}" --oiiotool "${OPENIMAGEIO_TOOL}")
    if(WITH_TESTS_BATCHED)
      list(APPEND _args --batch)
    endif()
    add_python_test(${testname} ${testscript} ${_args})
  endif()
endfunction()

# Run Python script outside Blender, using system default Python3 interpreter,
# NOT the one specified in `TEST_PYTHON_EXE`.
function(add_system_python_test testname testscript)
  if(NOT WITH_SYSTEM_PYTHON_TESTS)
    return()
  endif()

  add_test(
    NAME ${testname}
    COMMAND ${TEST_SYSTEM_PYTHON_EXE} ${testscript} ${ARGN}
    WORKING_DIRECTORY $<TARGET_FILE_DIR:blender>
  )
  blender_test_set_envvars("${testname}" "")
endfunction()

# ------------------------------------------------------------------------------
# TESTS USING SYSTEM PYTHON
# ------------------------------------------------------------------------------

add_system_python_test(
  script_validate_on_system_python
  ${CMAKE_CURRENT_LIST_DIR}/system_python/load_tool_scripts.py
  --root-dir ${CMAKE_SOURCE_DIR}
)

# ------------------------------------------------------------------------------
# RENDER TESTS
# ------------------------------------------------------------------------------

if((WITH_CYCLES OR WITH_GPU_RENDER_TESTS) AND TEST_SRC_DIR_EXISTS)
  set(render_tests
    integrator
  )

  list(SORT render_tests)

  # Cycles
  if(WITH_CYCLES)
    set(_cycles_all_test_devices CPU CUDA OPTIX HIP HIP-RT METAL METAL-RT ONEAPI ONEAPI-RT)
    set(_cycles_osl_test_devices CPU OPTIX)
    foreach(_cycles_device ${CYCLES_TEST_DEVICES})
      if(NOT ${_cycles_device} IN_LIST _cycles_all_test_devices)
        message(FATAL_ERROR "Unknown Cycles test device ${_cycles_device}."
            "Supported devices are: ${_cycles_all_test_devices}")
      endif()
      string(TOLOWER "${_cycles_device}" _cycles_device_lower)
      set(_cycles_render_tests ${render_tests})

      set(_cycles_osl_test_type none)
      if(WITH_CYCLES_OSL)
        # If the render device is CPU, or a GPU WITH_CYCLES_TEST_OSL enabled,
        # then enable the limited OSL tests (Tests OSL camera with OSL shading turned off).
        # This specific configuration is chosen to avoid long test times on the GPU due to
        # OSL JIT compilation, unless the developer explicitly enables OSL tests.
        if(("${_cycles_device_lower}" STREQUAL "cpu") OR (WITH_CYCLES_TEST_OSL AND ${_cycles_device} IN_LIST _cycles_osl_test_devices))
          set(_cycles_osl_test_type limited)
        endif()
      endif()

      foreach(render_test ${_cycles_render_tests})
        set(_cycles_test_name "cycles_${render_test}_${_cycles_device_lower}")
        if(NOT(WITH_CYCLES_TEST_OSL AND WITH_CYCLES_OSL AND ("${render_test}" STREQUAL "osl")))
          # Only run OSL specific tests during this phase if WITH_CYCLES_TEST_OSL isn't enabled
          add_render_test(
            ${_cycles_test_name}
            ${CMAKE_CURRENT_LIST_DIR}/cycles_render_tests.py
            --testdir "${TEST_SRC_DIR}/render/${render_test}"
            --outdir "${TEST_OUT_DIR}/cycles"
            --device ${_cycles_device}
            --osl "${_cycles_osl_test_type}"
          )
          if(NOT ("${_cycles_device_lower}" STREQUAL "cpu"))
            set_tests_properties(${_cycles_test_name} PROPERTIES RUN_SERIAL TRUE)
          endif()
        endif()

        # OSL variations of every test if WITH_CYCLES_TEST_OSL is set.
        if(WITH_CYCLES_TEST_OSL AND NOT "${_cycles_osl_test_type}" STREQUAL "none")
          add_render_test(
            ${_cycles_test_name}_osl
            ${CMAKE_CURRENT_LIST_DIR}/cycles_render_tests.py
            --testdir "${TEST_SRC_DIR}/render/${render_test}"
            --outdir "${TEST_OUT_DIR}/cycles_osl"
            --device ${_cycles_device}
            --osl "all"
          )
          if(NOT ("${_cycles_device_lower}" STREQUAL "cpu"))
            set_tests_properties(${_cycles_test_name}_osl PROPERTIES RUN_SERIAL TRUE)
          endif()
        endif()

        unset(_cycles_test_name)
      endforeach()
      unset(_cycles_osl_test_type)
    endforeach()
    unset(_cycles_osl_test_devices)
    unset(_cycles_all_test_devices)
  endif()
endif()
