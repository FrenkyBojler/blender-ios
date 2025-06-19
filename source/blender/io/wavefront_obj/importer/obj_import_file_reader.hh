/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup obj
 */

#pragma once

#include "IO_wavefront_obj.hh"

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "obj_import_objects.hh"

namespace blender::io::obj {

struct MTLMaterial;

/* NOTE: the OBJ parser implementation is planned to get fairly large changes "soon",
 * so don't read too much into current implementation... */
class OBJParser {
 private:
  const OBJImportParams &import_params_;
  FILE *obj_file_;
  size_t read_buffer_size_;

 public:
  /**
   * Open OBJ file at the path given in import parameters.
   */
  OBJParser(const OBJImportParams &import_params, size_t read_buffer_size);
  ~OBJParser();

  class Content {
   public:
    /* List of geometries parsed from the OBJ file. */
    Vector<std::unique_ptr<Geometry>> all_geometries;
    /* List of material library references parsed from the OBJ file. */
    Vector<std::string> mtl_libraries;
    /* Container for vertices position, normal, and UV coordinates. */
    GlobalVertices global_vertices;

    Content() = default;
    ~Content() = default;

    Content(const Content &copy) = delete;
    Content(Content &&move) = default;
    Content &operator=(const Content &copy) = delete;
    Content &operator=(Content &&move) = default;
  };

  /**
   * Reads the OBJ file and parses line by line to form OBJ Geometry instances. Parsed data is
   * returned in the content wrapper, which includes: generated geometries, material references,
   * and coordinate data.
   */
  Content parse();
};

class MTLParser {
 private:
  char mtl_file_path_[FILE_MAX];
  /**
   * Directory in which the MTL file is found.
   */
  char mtl_dir_path_[FILE_MAX];

 public:
  /**
   * Open material library file.
   */
  MTLParser(StringRefNull mtl_library_, StringRefNull obj_filepath);

  /**
   * Read MTL file(s) and add MTLMaterial instances to the given Map reference.
   */
  void parse_and_store(Map<std::string, std::unique_ptr<MTLMaterial>> &r_materials);
};
}  // namespace blender::io::obj
