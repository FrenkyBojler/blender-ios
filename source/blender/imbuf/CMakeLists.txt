# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  ../blenkernel
  ../blenloader
  ../gpu
  ../makesrna
  ../sequencer
  ../../../intern/memutil
)

set(INC_SYS
  ${JPEG_INCLUDE_DIR}
  ${ZLIB_INCLUDE_DIRS}
  ${OPENIMAGEIO_INCLUDE_DIRS}
)

set(SRC
  intern/allocimbuf.cc
  intern/colormanagement.cc
  intern/colormanagement_inline.h
  intern/divers.cc
  intern/filetype.cc
  intern/filter.cc
  intern/format_bmp.cc
  intern/format_dds.cc
  intern/format_hdr.cc
  intern/format_png.cc
  intern/format_psd.cc
  intern/format_svg.cc
  intern/format_targa.cc
  intern/format_tiff.cc
  intern/imageprocess.cc
  intern/interp.cc
  intern/iris.cc
  intern/jpeg.cc
  intern/metadata.cc
  intern/module.cc
  intern/moviecache.cc
  intern/readimage.cc
  intern/rectop.cc
  intern/rotate.cc
  intern/scaling.cc
  intern/stereoimbuf.cc
  intern/thumbs.cc
  intern/thumbs_blend.cc
  intern/thumbs_font.cc
  intern/transform.cc
  intern/util.cc
  intern/util_gpu.cc
  intern/writeimage.cc

  IMB_colormanagement.hh
  IMB_imbuf.hh
  IMB_imbuf_enums.h
  IMB_imbuf_types.hh
  IMB_interp.hh
  IMB_metadata.hh
  IMB_moviecache.hh
  IMB_openexr.hh
  IMB_thumbs.hh
  intern/IMB_allocimbuf.hh
  intern/IMB_colormanagement_intern.hh
  intern/IMB_filetype.hh
  intern/IMB_filter.hh
  intern/imbuf.hh

  movie/intern/ffmpeg_compat.h
  movie/intern/ffmpeg_util.cc
  movie/intern/ffmpeg_util.hh
  movie/intern/movie_proxy_indexer.cc
  movie/intern/movie_proxy_indexer.hh
  movie/intern/readmovie.cc
  movie/intern/readmovie.hh
  movie/intern/writemovie.cc
  movie/IMB_anim.hh
  movie/IMB_movie_enums.hh
  movie/IMB_writemovie.hh
)

set(LIB
  bf_blenkernel
  PRIVATE bf::blenlib
  bf_blenloader
  PRIVATE bf::dna
  bf_imbuf_openimageio
  PRIVATE bf::intern::guardedalloc
  bf_intern_memutil
  bf_intern_opencolorio
  PRIVATE bf::extern::nanosvg

  ${JPEG_LIBRARIES}
)

if(WITH_IMAGE_OPENEXR)
  list(APPEND LIB
    bf_imbuf_openexr
  )
  add_definitions(-DWITH_OPENEXR)
else()
  list(APPEND SRC
    intern/openexr/openexr_stub.cpp
  )
endif()

# Keep until APPLE/ARM libraries are updated.
if((APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES arm64) OR
   (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64"))
  list(APPEND INC_SYS
    ${IMATH_INCLUDE_DIRS}
  )
endif()

if(WITH_IMAGE_OPENJPEG)
  list(APPEND INC_SYS
    ${OPENJPEG_INCLUDE_DIRS}
  )
  list(APPEND SRC
    intern/jp2.cc
  )
  list(APPEND LIB
    ${OPENJPEG_LIBRARIES}
  )

  add_definitions(-DWITH_OPENJPEG ${OPENJPEG_DEFINES})
endif()

if(WITH_CODEC_FFMPEG)
  list(APPEND SRC
    movie/intern/swscale.cc
    movie/intern/swscale.hh
    movie/intern/writeffmpeg.cc
    movie/intern/writeffmpeg.hh
  )
  list(APPEND INC
    ../../../intern/ffmpeg
  )
  list(APPEND INC_SYS
    ${FFMPEG_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${FFMPEG_LIBRARIES}
    ${OPENJPEG_LIBRARIES}
  )
  add_definitions(-DWITH_FFMPEG)
endif()

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  if(WITH_SYSTEM_AUDASPACE)
    list(APPEND LIB
      ${AUDASPACE_C_LIBRARIES}
    )
  endif()
  add_definitions(-DWITH_AUDASPACE)
endif()

if(WITH_IMAGE_CINEON)
  list(APPEND SRC
    intern/format_dpx.cc
  )
  list(APPEND LIB
    bf_imbuf_cineon
  )
  add_definitions(-DWITH_CINEON)
endif()

if(WITH_IMAGE_WEBP)
  list(APPEND SRC
    intern/webp.cc
  )
  list(APPEND INC_SYS
    ${WEBP_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${WEBP_LIBRARIES}
  )
  add_definitions(-DWITH_WEBP)
endif()

list(APPEND INC
  ../../../intern/opencolorio
)

if(WIN32)
  list(APPEND INC
    ../../../intern/utfconv
  )
endif()

if(WIN32)
  add_definitions(-DNOMINMAX)
endif()

# no need to compile object files for inline headers.
set_source_files_properties(
  intern/colormanagement_inline.cc
  PROPERTIES HEADER_FILE_ONLY TRUE
)

blender_add_lib(bf_imbuf "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

if(WITH_GTESTS)
  set(TEST_SRC
    tests/IMB_scaling_test.cc
    tests/IMB_transform_test.cc
  )
  blender_add_test_suite_lib(imbuf "${TEST_SRC}" "${INC}" "${INC_SYS}" "${LIB}")
  add_subdirectory(tests/performance)
endif()

if(WITH_GTESTS AND WITH_CODEC_FFMPEG)
  set(TEST_SRC
    movie/tests/ffmpeg_codecs.cc
    movie/tests/ffmpeg_cpu_flags.cc
  )
  set(TEST_INC
    movie/intern
  )
  set(TEST_INC_SYS
    ${FFMPEG_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
  )
  set(TEST_LIB
    ${PNG_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${ZLIB_LIBRARIES}
  )
  if(WITH_IMAGE_OPENJPEG)
    set(TEST_LIB ${TEST_LIB} ${OPENJPEG_LIBRARIES})
  endif()
  blender_add_test_suite_lib(ffmpeg_libs "${TEST_SRC}" "${TEST_INC}" "${TEST_INC_SYS}" "${TEST_LIB}")

endif()
