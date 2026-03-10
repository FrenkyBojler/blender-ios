# SPDX-FileCopyrightText: 2026 KhronosGroup
#
# SPDX-License-Identifier: GPL-2.0-or-later

remove_strict_flags()


set(INC
  src
  src/utils
  src/include
  src/external
  src/external/dfdutils
  src/other_include
  ${ZSTD_INCLUDE_DIRS}
)

set(SRC
  src/external/dfdutils/createdfd.c
  src/external/dfdutils/colourspaces.c
  src/external/dfdutils/dfd.h
  src/external/dfdutils/interpretdfd.c
  src/external/dfdutils/printdfd.c
  src/external/dfdutils/queries.c
  src/external/dfdutils/vk2dfd.c
  src/external/dfdutils/vk2dfd.inl
  src/external/dfdutils/vulkan/vk_platform.h
  src/external/dfdutils/vulkan/vulkan_core.h
  src/external/basisu/transcoder/basisu_containers.h
  src/external/basisu/transcoder/basisu_containers_impl.h
  src/external/basisu/transcoder/basisu_file_headers.h
  src/external/basisu/transcoder/basisu_transcoder_internal.h
  src/external/basisu/transcoder/basisu_transcoder_uastc.h
  src/external/basisu/transcoder/basisu_transcoder.cpp
  src/external/basisu/transcoder/basisu_transcoder.h
  src/external/basisu/transcoder/basisu.h
  src/lib/astc_codec.cpp
  src/lib/basis_encode.cpp
  src/lib/basis_sgd.h
  src/lib/basis_transcode.cpp
  src/lib/checkheader.c
  src/lib/etcunpack.cxx
  src/lib/filestream.c
  src/lib/filestream.h
  src/lib/formatsize.h
  src/lib/gl_format.h
  src/lib/glformat_str.c
  src/lib/gl_funclist.inl
  src/lib/gl_funcs.c
  src/lib/gl_funcs.h
  src/lib/glloader.c
  src/lib/hashlist.c
  src/lib/info.c
  src/lib/internalexport.def
  src/lib/internalexport_mingw.def
  src/lib/internalexport_write.def
  src/lib/internalexport_write_mingw.def
  src/lib/ktxint.h
  src/lib/libktx_mainpage.md
  src/lib/memstream.c
  src/lib/memstream.h
  src/lib/miniz_wrapper.cpp
  src/lib/nmkvkff
  src/lib/strings.c
  src/lib/swap.c
  src/lib/texture.c
  src/lib/texture.h
  src/lib/texture1.c
  src/lib/texture1.h
  src/lib/texture2.c
  src/lib/texture2.h
  src/lib/texture_funcs.inl
  src/lib/uthash.h
  src/lib/vk2gl.h
  src/lib/vk_format.h
  src/lib/vkFormat2glFormat.inl
  src/lib/vkFormat2glInternalFormat.inl
  src/lib/vkFormat2glType.inl
  src/lib/vkformat_check.c
  src/lib/vkformat_check_variant.c
  src/lib/vkformat_enum.h
  src/lib/vkformat_str.c
  src/lib/vkformat_typesize.c
  src/lib/vk_funcs.c
  src/lib/vk_funcs.h
  src/lib/vkloader.c
  src/lib/writer1.c
  src/lib/writer2.c
)


blender_add_lib(ktx "${SRC}" "${INC}" "" "${LIB}")

target_compile_definitions(ktx PRIVATE BASISU_SUPPORT_OPENCL=1 LIBKTX)